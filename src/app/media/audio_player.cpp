#include "app/media/audio_player.h"

#include "app/identity.h"
#include "app/media/audio_errors.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/str.h"
#include "plat/audio.h"
#include "plat/plat.h"

#include <algorithm>
#include <utility>

namespace media {

namespace {
constexpr int kTickMs = 200; // position updates while playing
}

AudioPlayer::AudioPlayer(plat::App &app) : _app(app) {}

AudioPlayer::~AudioPlayer() {
    if (_tick)
        _app.cancelTimer(_tick);
    if (_engine)
        _engine->stop();
}

void AudioPlayer::ensureEngine() {
    if (_engine)
        return;
    _engine = playerFactory ? playerFactory() : plat::audio::Player::create(_app);
    if (!_engine)
        return;
    _engine->onLoaded = [this] {
        if (_status.state != State::Loading)
            return;
        _engine->play();
        setState(State::Playing);
    };
    _engine->onFailed = [this](const plat::audio::Failure &f) {
        _status.error = audioErrorText(f);
        setState(State::Error);
    };
    _engine->onEnded = [this] {
        if (_status.state != State::Playing)
            return;
        if (const int64_t d = _engine->durationMs(); d > 0)
            _status.positionMs = d;
        setState(State::Ended);
    };
}

std::string AudioPlayer::takeOver(const std::string &key) {
    if (_status.key == key)
        return {};
    std::string prev = _status.key;
    if (_engine)
        _engine->stop();
    if (_tick)
        _app.cancelTimer(std::exchange(_tick, 0));
    _status     = Status{};
    _status.key = key;
    return prev;
}

void AudioPlayer::beginLoading(const std::string &key, int64_t knownDurationMs) {
    const std::string prev = takeOver(key);
    _knownDurationMs       = knownDurationMs;
    _status.durationMs     = knownDurationMs;
    _status.positionMs     = 0;
    _status.error.clear();
    _status.state = State::Loading;
    if (!prev.empty())
        notify(prev);
    changed();
}

void AudioPlayer::loadFailed(const std::string &key, const std::string &error) {
    if (!isCurrent(key) || _status.state != State::Loading)
        return;
    _status.error = error;
    setState(State::Error);
}

void AudioPlayer::play(
    const std::string &key, const std::string &localPath, int64_t knownDurationMs
) {
    if (isCurrent(key)) {
        if (_status.state == State::Playing)
            return;
        if (_status.state == State::Paused || _status.state == State::Ended) {
            resume();
            return;
        }
    }
    const std::string prev = takeOver(key);
    ensureEngine();
    if (knownDurationMs > 0)
        _knownDurationMs = knownDurationMs;
    _status.durationMs = _knownDurationMs;
    _status.positionMs = 0;
    _status.error.clear();
    _status.state = State::Loading;
    if (!prev.empty())
        notify(prev);
    if (!_engine) {
        _status.error = audioErrorText({plat::audio::Error::Unavailable, {}});
        setState(State::Error);
        return;
    }
    changed();
    _engine->load(localPath);
}

void AudioPlayer::togglePause() {
    switch (_status.state) {
    case State::Playing:
        pause();
        break;
    case State::Paused:
    case State::Ended:
        resume();
        break;
    default:
        break;
    }
}

void AudioPlayer::pause() {
    if (_status.state != State::Playing || !_engine)
        return;
    _status.positionMs = _engine->positionMs();
    _engine->pause();
    setState(State::Paused);
}

void AudioPlayer::resume() {
    if (!_engine)
        return;
    if (_status.state == State::Ended) {
        _engine->seek(0);
        _status.positionMs = 0;
    } else if (_status.state != State::Paused) {
        return;
    }
    _engine->play();
    setState(State::Playing);
}

void AudioPlayer::seek(int64_t ms) {
    if (!_engine || (_status.state != State::Playing && _status.state != State::Paused &&
                     _status.state != State::Ended))
        return;
    const int64_t dur = _status.durationMs;
    ms                = std::max<int64_t>(0, dur > 0 ? std::min(ms, dur) : ms);
    _engine->seek(ms);
    _status.positionMs = ms;
    if (_status.state == State::Ended) {
        _engine->play();
        setState(State::Playing);
    } else {
        changed();
    }
}

void AudioPlayer::stop() {
    if (_status.key.empty())
        return;
    const std::string prev = _status.key;
    if (_engine)
        _engine->stop();
    if (_tick)
        _app.cancelTimer(std::exchange(_tick, 0));
    _status = Status{};
    notify(prev);
}

void AudioPlayer::setState(State s) {
    _status.state = s;
    if (s == State::Playing) {
        if (!_tick)
            _tick = _app.addTimer(kTickMs, true, [this] { tick(); });
    } else if (_tick) {
        _app.cancelTimer(std::exchange(_tick, 0));
    }
    changed();
}

void AudioPlayer::tick() {
    if (_status.state != State::Playing || !_engine)
        return;
    _status.positionMs = _engine->positionMs();
    changed();
}

void AudioPlayer::changed() {
    if (_engine && _status.state != State::Idle && _status.state != State::Loading) {
        if (const int64_t d = _engine->durationMs(); d > 0)
            _status.durationMs = d;
        else if (_knownDurationMs > 0)
            _status.durationMs = _knownDurationMs;
    }
    if (_status.durationMs > 0)
        _status.positionMs = std::clamp<int64_t>(_status.positionMs, 0, _status.durationMs);
    notify(_status.key);
}

void AudioPlayer::notify(const std::string &key) {
    _observers.notify(key);
}

AudioPlayer::ObserverId AudioPlayer::observe(std::function<void(const std::string &key)> fn) {
    return _observers.add(std::move(fn));
}

void AudioPlayer::unobserve(ObserverId id) {
    _observers.remove(id);
}

bool AudioPlayer::canPlay(const std::string &ext) {
    for (const auto &[e, ok] : _playable)
        if (e == ext)
            return ok;
    const bool ok = plat::audio::canPlayExtension(ext);
    _playable.emplace_back(ext, ok);
    return ok;
}

std::string AudioPlayer::extensionOf(std::string_view nameOrUrl) {
    std::string_view path = nameOrUrl;
    if (const size_t scheme = path.find("://"); scheme != std::string_view::npos) {
        path               = path.substr(scheme + 3);
        const size_t slash = path.find('/');
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash);
        path = path.substr(0, path.find_first_of("?#"));
    }
    return str::asciiLower(file::extension(file::baseName(path)));
}

std::string AudioPlayer::sourceFor(const model::File &f) {
    for (const std::string *url : {&f.path, &f.original}) {
        if (url->empty())
            continue;
        std::string ext = extensionOf(*url);
        if (ext.empty())
            ext = extensionOf(f.name);
        if (canPlay(ext))
            return *url;
    }
    return f.path;
}

std::string AudioPlayer::cachePath(const model::File &f, const std::string &url) const {
    const std::string cache = identity::cacheDir(_app);
    if (cache.empty())
        return {};
    std::string ext = extensionOf(url);
    if (ext.empty())
        ext = extensionOf(f.name);
    if (ext.empty())
        ext = "audio";
    const std::string hash = crypto::hex(crypto::bytes(crypto::sha1(url))).substr(0, 10);
    std::string       id   = f.id.empty() ? std::string("file") : f.id;
    for (char &c : id) // an id is a file name here
        if (c == '/' || c == '\\' || c == '.')
            c = '_';
    return file::join(file::join(cache, "audio"), str::concat({id, "-", hash, ".", ext}));
}

} // namespace media
