// Audio on Linux and the BSDs. There is no dependency-free in-process audio
// path: every sound server needs a shared library that won't link into the
// static release binary, and the static (musl) build can't dlopen one. So
// audio goes through the sound server's own command-line tools, present on
// essentially every desktop:
//
//   playback  source (PCM frames)  ──►  sink (helper reading raw PCM on stdin)
//             sources: the vendored miniaudio decoders in-process (MP3/WAV/
//             Vorbis), else `ffmpeg … -f s16le pipe:1` when it is on
//             PATH (AAC/M4A, WebM/Opus, FLAC, …). Sinks: the first of pw-cat /
//             paplay / aplay / aucat that starts.
//   capture   pw-record / parecord / arecord / aucat writing 16 kHz mono s16le
//             to a pipe
//   sounds    canberra-gtk-play / pw-play / paplay / ffplay / aplay / aucat on
//             a file; system sounds from the freedesktop sound theme
// aucat is sndio's: in OpenBSD's base system, a package on FreeBSD (whose
// desktops otherwise run PulseAudio or PipeWire, and have no ALSA).
//
// Players and recorders each run a worker thread that owns the helper
// processes and multiplexes their pipes with poll(), so the UI thread never
// blocks on a pipe (and the headless test backend, which has no fd watching,
// works the same). Results are posted back to the App's thread.
//
// Pause and seek kill the sink (its buffered audio must not play out) and
// restart it at the new offset; the position is the wall clock since the sink
// started plus that offset, capped by what has been written.
#include "audio/audio_internal.h"

#include "freedesktop/files.h"
#include "freedesktop/process.h"
#include "plat/plat.h"
#include "pcm_decoder.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace plat::audio {

namespace {

using Clock = std::chrono::steady_clock;

int64_t msSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// ── Helper processes ────────────────────────────────────────────────────────

using linux_process::Child;
using linux_process::closeFd;
using linux_process::kStderr;
using linux_process::kStdin;
using linux_process::kStdout;
using linux_process::reaped;

// Where audio helpers are searched: $PATH, or only $PLAT_AUDIO_HELPERS when
// that is set: tests point it at fake helpers (or at nothing), so no test
// ever reaches the real speakers or mic.
std::string_view helperPath() {
    const char *only = std::getenv("PLAT_AUDIO_HELPERS");
    return only ? std::string_view(only) : linux_process::defaultSearchPath();
}

// Absolute path of an audio helper program, or "".
std::string findHelper(const char *name) {
    return linux_process::findExecutable(name, helperPath());
}

// Reads what is there; false at end of file (the fd is then closed).
bool drain(int &fd, std::string &into) {
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            into.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        closeFd(fd);
        return false;
    }
}

// The last non-empty line of a helper's stderr, trimmed.
std::string lastLine(const std::string &s) {
    size_t end = s.size();
    while (end > 0) {
        size_t b = s.rfind('\n', end - 1);
        b        = b == std::string::npos ? 0 : b + 1;
        std::string line(s, b, end - b);
        while (!line.empty() && std::strchr(" \t\r", line.back()))
            line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && std::strchr(" \t", line[lead]))
            ++lead;
        if (lead < line.size())
            return line.substr(lead);
        end = b == 0 ? 0 : b - 1;
    }
    return {};
}

// Notification sounds look up the same few helpers and the sound theme on
// every play (canberra missing means a gsettings run of up to 800 ms each
// time): both are remembered for a while, per search path, so a burst of
// notifications costs one lookup. A helper installed or a theme switched
// meanwhile is seen once the entry expires.
constexpr auto kSoundCacheTtl = std::chrono::seconds(60);

struct SoundCache {
    std::mutex        mutex;
    std::string       key; // the search path + data dirs it was built for
    Clock::time_point built{};
    std::vector<std::pair<std::string, std::string>> helpers; // name → path ("" = none)
    std::optional<std::string>                       themeDir;
};

SoundCache &soundCache() {
    static SoundCache c;
    return c;
}

// The cache, emptied when it expired or the environment it was built for
// changed; mutex held by the caller.
SoundCache &freshSoundCache() {
    SoundCache       &c   = soundCache();
    const char       *dh  = std::getenv("XDG_DATA_HOME");
    const char       *h   = std::getenv("HOME");
    const std::string key = std::string(helperPath()) + '\n' + (dh ? dh : "") + '\n' + (h ? h : "");
    if (key != c.key || Clock::now() - c.built > kSoundCacheTtl) {
        c.key   = key;
        c.built = Clock::now();
        c.helpers.clear();
        c.themeDir.reset();
    }
    return c;
}

// findHelper through the sound cache.
std::string soundHelper(const char *name) {
    {
        std::lock_guard lock(soundCache().mutex);
        for (const auto &[n, p] : freshSoundCache().helpers)
            if (n == name)
                return p;
    }
    std::string     path = findHelper(name);
    std::lock_guard lock(soundCache().mutex);
    freshSoundCache().helpers.emplace_back(name, path);
    return path;
}

// Spawns a fire-and-forget helper and waits for it (this runs on a detached
// thread). True when it could be started.
bool runHelper(const char *name, const std::vector<std::string> &args) {
    const std::string exe = soundHelper(name);
    Child             c;
    if (exe.empty() || !linux_process::spawn(exe, args, 0, &c))
        return false;
    while (::waitpid(c.pid, nullptr, 0) < 0 && errno == EINTR) {
    }
    return true;
}

// A thread with a roomy stack (musl's default is 128 KiB; stb_vorbis uses
// alloca), detached or joinable.
bool startThread(pthread_t *t, void *(*fn)(void *), void *arg, bool detached) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);
    if (detached)
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    const bool ok = ::pthread_create(t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    return ok;
}

// A wake-up pipe the owner writes to so the worker's poll() returns.
struct Wake {
    int r = -1, w = -1;
    Wake() {
        int p[2];
        if (::pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0)
            r = p[0], w = p[1];
    }
    ~Wake() { closeFd(r), closeFd(w); }
    void ping() const { (void)!::write(w, "x", 1); }
    void clear() const {
        char b[64];
        while (::read(r, b, sizeof b) > 0) {
        }
    }
};

// ── Playback ────────────────────────────────────────────────────────────────

struct PcmFormat {
    int rate = 48000, channels = 2;
};
int64_t bytesPerFrame(const PcmFormat &f) {
    return 2 * int64_t(f.channels); // s16le
}

constexpr int    kSinkCount = 4;
constexpr size_t kFfmpegCap = 1 << 20; // decoded PCM buffered ahead of the sink

// Output helper i for raw s16le in format f: its name, *args its arguments.
const char *sinkArgs(int i, const PcmFormat &f, std::vector<std::string> *args) {
    const std::string rate = std::to_string(f.rate), ch = std::to_string(f.channels);
    switch (i) {
    case 0:
        *args = {"--playback", "--raw", "--format", "s16", "--rate", rate, "--channels", ch, "-"};
        return "pw-cat";
    case 1:
        *args = {"--raw", "--format=s16le", "--rate=" + rate, "--channels=" + ch};
        return "paplay";
    case 2:
        *args = {"-q", "-t", "raw", "-f", "S16_LE", "-r", rate, "-c", ch, "-"};
        return "aplay";
    default: {
        const std::string range = "0:" + std::to_string(f.channels - 1); // first:last channel
        *args                   = {"-h", "raw", "-e", "s16le", "-r", rate, "-c", range, "-i", "-"};
        return "aucat";
    }
    }
}

constexpr const char *kInProcess[] = {"mp3", "wav", "ogg", "oga"};

class LinuxPlayer;

// What a player and its worker thread share. The worker owns the media
// (decoder, helper processes); the player reads the position snapshot.
struct PlayerShared {
    enum class Op : uint8_t { Load, Play, Pause, Seek, Stop, Quit };
    struct Cmd {
        Op          op;
        uint64_t    gen = 0;
        int64_t     ms  = 0;
        std::string path;
    };

    std::mutex        m;
    std::vector<Cmd>  cmds; // for the worker, in order
    // The position snapshot, written by the worker.
    bool              hasMedia = false, playing = false;
    PcmFormat         fmt;
    int64_t           baseFrame = 0, pausedFrame = 0, written = 0, total = 0;
    Clock::time_point started;

    Wake         wake;
    App         *app   = nullptr;
    LinuxPlayer *owner = nullptr; // UI thread only; null once the player is gone

    int64_t currentFrame() const { // with m held
        if (!playing)
            return pausedFrame;
        int64_t f = baseFrame + msSince(started) * fmt.rate / 1000;
        f         = std::min(f, baseFrame + written);
        return total > 0 ? std::min(f, total) : f;
    }
};

enum class PlayerEv : uint8_t { Loaded, Ended, Failed };

class PlayerWorker {
public:
    explicit PlayerWorker(std::shared_ptr<PlayerShared> s) : _s(std::move(s)) {}
    ~PlayerWorker() { stopMedia(); }

    static void *entry(void *p) {
        auto *w = static_cast<PlayerWorker *>(p);
        w->run();
        delete w;
        return nullptr;
    }

private:
    void run() {
        for (;;) {
            // Clear before taking: a command queued in between still leaves
            // its ping, so poll() can't sleep on it.
            _s->wake.clear();
            std::vector<PlayerShared::Cmd> cmds;
            {
                std::lock_guard lock(_s->m);
                cmds.swap(_s->cmds);
            }
            for (const auto &c : cmds) {
                switch (c.op) {
                case PlayerShared::Op::Load:
                    _gen = c.gen;
                    load(c.path);
                    break;
                case PlayerShared::Op::Play:
                    play();
                    break;
                case PlayerShared::Op::Pause:
                    pause();
                    break;
                case PlayerShared::Op::Seek:
                    seek(c.ms);
                    break;
                case PlayerShared::Op::Stop:
                    _gen = c.gen;
                    stopMedia();
                    break;
                case PlayerShared::Op::Quit:
                    return;
                }
            }
            pump();
            waitAndRead();
        }
    }

    void post(PlayerEv ev, Failure f = {});

    void publish(int64_t total) {
        std::lock_guard lock(_s->m);
        _s->hasMedia  = true;
        _s->playing   = false;
        _s->fmt       = _fmt;
        _s->total     = total;
        _s->baseFrame = _s->pausedFrame = _s->written = 0;
    }
    void snapshot() {
        std::lock_guard lock(_s->m);
        _s->playing     = _playing;
        _s->baseFrame   = _baseFrame;
        _s->pausedFrame = _pausedFrame;
    }
    int64_t current() {
        std::lock_guard lock(_s->m);
        return _s->currentFrame();
    }
    bool hasSource() const { return _dec || _ff; }

    void load(const std::string &path) {
        stopMedia();
        int       rate = 0, ch = 0;
        long long total = 0;
        if ((_dec = pcm_decoder_open(path.c_str(), &rate, &ch, &total))) {
            _fmt      = {rate, ch};
            _decTotal = total;
            _decEof   = false;
            publish(total);
            post(PlayerEv::Loaded);
            return;
        }
        // miniaudio's decoders are the whole in-process repertoire; anything
        // else (AAC/M4A, WebM/Opus, …) only plays with ffmpeg on PATH.
        const std::string exe = findHelper("ffmpeg");
        if (exe.empty()) {
            post(PlayerEv::Failed, {Error::NeedsFfmpeg, {}});
            return;
        }
        _ff       = std::make_unique<Ffmpeg>();
        _ff->exe  = exe;
        _ff->path = path;
        _fmt      = {48000, 2};
        publish(0);
        // ffmpeg can't tell us up front whether it can decode the file:
        // loaded only once the first PCM arrives (or failed).
        _probing = true;
        ffmpegStart(0);
    }

    void play() {
        if (!hasSource() || _playing)
            return;
        _playing = true;
        sourceSeek(_pausedFrame);
        _baseFrame = _pausedFrame;
        _attempt   = 0;
        startSink();
    }

    void pause() {
        if (!_playing)
            return;
        _pausedFrame = current();
        _playing     = false;
        linux_process::kill(_sink);
        snapshot();
    }

    void seek(int64_t ms) {
        if (!hasSource())
            return;
        int64_t frame = ms * _fmt.rate / 1000;
        if (const int64_t total = sourceTotal(); total > 0)
            frame = std::min(frame, total);
        frame        = std::max<int64_t>(0, frame);
        _pausedFrame = frame;
        if (_playing) {
            linux_process::kill(_sink);
            sourceSeek(frame);
            _baseFrame = frame;
            startSink();
        } else {
            snapshot();
        }
    }

    void stopMedia() {
        _playing = _probing = false;
        linux_process::kill(_sink);
        if (_dec)
            pcm_decoder_close(_dec);
        _dec = nullptr;
        if (_ff)
            linux_process::kill(_ff->c);
        _ff.reset();
        _pending.clear();
        _pendingOff = 0;
        _baseFrame = _pausedFrame = 0;
        std::lock_guard lock(_s->m);
        _s->hasMedia = _s->playing = false;
        _s->baseFrame = _s->pausedFrame = _s->written = _s->total = 0;
    }

    // ── Source ──────────────────────────────────────────────────────────────
    struct Ffmpeg {
        std::string exe, path;
        Child       c;
        std::string buf, err;
        size_t      off        = 0;
        int64_t     startFrame = -1;
        int64_t     total      = 0;
        bool        finished = false, gotData = false;
    };

    // `ffmpeg … -f s16le pipe:1` from `frame` on; seeking restarts it with -ss.
    void ffmpegStart(int64_t frame) {
        Ffmpeg &f = *_ff;
        linux_process::kill(f.c);
        f.buf.clear();
        f.err.clear();
        f.off        = 0;
        f.finished   = false;
        f.gotData    = false;
        f.startFrame = frame;
        std::vector<std::string> args{"-nostdin", "-hide_banner", "-nostats", "-loglevel", "info"};
        if (frame > 0) {
            char sec[32];
            std::snprintf(sec, sizeof sec, "%.3f", double(frame) / _fmt.rate);
            args.insert(args.end(), {"-ss", sec});
        }
        args.insert(
            args.end(),
            {"-i",
             f.path,
             "-vn",
             "-f",
             "s16le",
             "-ac",
             std::to_string(_fmt.channels),
             "-ar",
             std::to_string(_fmt.rate),
             "pipe:1"}
        );
        if (!linux_process::spawn(f.exe, args, kStdout | kStderr, &f.c)) {
            f.finished = true;
            failSource({Error::NeedsFfmpeg, {}});
        }
    }

    // ffmpeg prints "Duration: 00:01:02.50" in its banner on stderr.
    void parseDuration() {
        Ffmpeg &f = *_ff;
        if (f.total)
            return;
        const size_t at = f.err.find("Duration:");
        int          h, m, s, cs;
        if (at == std::string::npos ||
            std::sscanf(f.err.c_str() + at + 9, " %d:%d:%d.%d", &h, &m, &s, &cs) != 4)
            return;
        const int64_t ms = ((int64_t(h) * 60 + m) * 60 + s) * 1000 + int64_t(cs) * 10;
        f.total          = ms * _fmt.rate / 1000;
        std::lock_guard lock(_s->m);
        _s->total = f.total;
    }

    void ffmpegFinished(bool ok) {
        Ffmpeg &f  = *_ff;
        f.finished = true;
        if (!f.gotData && !ok) {
            failSource({Error::Unsupported, lastLine(f.err)});
            return;
        }
        if (_probing) { // finished without any audio at all: an empty file
            _probing = false;
            post(PlayerEv::Loaded);
        }
    }

    void failSource(Failure f) {
        _probing = _playing = false;
        linux_process::kill(_sink);
        snapshot();
        post(PlayerEv::Failed, std::move(f));
    }

    void sourceSeek(int64_t frame) {
        if (_dec) {
            pcm_decoder_seek(_dec, frame);
            _decEof = false;
        } else if (_ff && !(_ff->startFrame == frame && _ff->off == 0 && !_ff->finished)) {
            ffmpegStart(frame); // (the probe's run, untouched, already starts there)
        }
    }

    int64_t sourceTotal() const { return _dec ? _decTotal : _ff ? _ff->total : 0; }

    bool sourceAtEnd() const {
        return _dec ? _decEof : _ff ? _ff->finished && _ff->off >= _ff->buf.size() : true;
    }

    // Appends up to maxBytes of PCM to `out`; may append nothing before the
    // end (ffmpeg has not delivered yet).
    void readSource(std::string &out, size_t maxBytes) {
        const int64_t bpf = bytesPerFrame(_fmt);
        if (_dec) {
            const int64_t frames = std::max<int64_t>(1, int64_t(maxBytes) / bpf);
            out.resize(size_t(frames * bpf));
            const long long got = pcm_decoder_read(_dec, out.data(), frames);
            if (got <= 0)
                _decEof = true;
            out.resize(size_t(std::max<long long>(got, 0) * bpf));
            return;
        }
        if (!_ff)
            return;
        Ffmpeg      &f = *_ff;
        const size_t n = std::min(maxBytes, f.buf.size() - f.off);
        out.append(f.buf, f.off, n);
        f.off += n;
        if (f.off == f.buf.size()) {
            f.buf.clear();
            f.off = 0;
        }
    }

    // ── Sink ────────────────────────────────────────────────────────────────
    // Starts the first output helper at or after _attempt that launches.
    void startSink() {
        _pending.clear();
        _pendingOff   = 0;
        _writtenBytes = 0;
        _inputDone = _sinkGone = false;
        for (int i = _attempt; i < kSinkCount; ++i) {
            std::vector<std::string> args;
            const std::string        exe = findHelper(sinkArgs(i, _fmt, &args));
            if (exe.empty() || !linux_process::spawn(exe, args, kStdin, &_sink))
                continue;
            _attempt         = i;
            // Keep ~250 ms queued for the helper; more only delays pause/seek.
            const int sndbuf = int(std::max<int64_t>(4096, bytesPerFrame(_fmt) * _fmt.rate / 4));
            ::setsockopt(_sink.in, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
            _sinkStart = Clock::now();
            std::lock_guard lock(_s->m);
            _s->playing   = true;
            _s->started   = _sinkStart;
            _s->baseFrame = _baseFrame;
            _s->written   = 0;
            return;
        }
        _playing = false;
        snapshot();
        post(PlayerEv::Failed, {Error::NoOutput, {}});
    }

    // Moves decoded PCM into the sink until its socket is full.
    void pump() {
        if (_probing && _ff && _ff->off < _ff->buf.size()) {
            _probing = false;
            post(PlayerEv::Loaded);
        }
        if (!_playing || _sink.in < 0 || _inputDone || _sinkGone)
            return;
        const int64_t bpf   = bytesPerFrame(_fmt);
        const size_t  chunk = size_t(bpf * _fmt.rate / 20); // 50 ms
        for (;;) {
            if (_pendingOff >= _pending.size()) {
                _pending.clear();
                _pendingOff = 0;
                if (sourceAtEnd()) {
                    _inputDone = true;
                    closeFd(_sink.in); // EOF: the helper plays out and exits
                    return;
                }
                readSource(_pending, chunk);
                if (_pending.empty()) {
                    if (sourceAtEnd())
                        continue;
                    return; // ffmpeg will deliver more
                }
            }
            const ssize_t n = ::send(
                _sink.in,
                _pending.data() + _pendingOff,
                _pending.size() - _pendingOff,
                MSG_NOSIGNAL | MSG_DONTWAIT
            );
            if (n > 0) {
                _pendingOff += size_t(n);
                _writtenBytes += n;
                std::lock_guard lock(_s->m);
                _s->written = _writtenBytes / bpf;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            _sinkGone = true; // the helper died
            return;
        }
    }

    void sinkFinished(bool ok) {
        if (!_playing)
            return;
        if (ok || _inputDone) { // played out to the end
            _playing            = false;
            const int64_t total = sourceTotal();
            _pausedFrame = total > 0 ? total : _baseFrame + _writtenBytes / bytesPerFrame(_fmt);
            snapshot();
            post(PlayerEv::Ended);
            return;
        }
        // Died early (sound server not running, device busy): the next helper.
        if (msSince(_sinkStart) < 2000 && _attempt + 1 < kSinkCount) {
            ++_attempt;
            sourceSeek(_baseFrame);
            startSink();
            return;
        }
        _playing     = false;
        _pausedFrame = _baseFrame;
        snapshot();
        post(PlayerEv::Failed, {Error::OutputFailed, {}});
    }

    // One poll() over the wake pipe, the sink and ffmpeg's pipes.
    void waitAndRead() {
        pollfd fds[4];
        int    n = 0, sinkI = -1, outI = -1, errI = -1;
        fds[n++] = {_s->wake.r, POLLIN, 0};
        if (_sink.in >= 0) {
            const bool wantWrite = _playing && !_inputDone && _pendingOff < _pending.size();
            sinkI                = n;
            fds[n++] = {_sink.in, short(wantWrite ? POLLOUT : 0), 0}; // + HUP/ERR always
        }
        if (_ff && _ff->c.out >= 0 && _ff->buf.size() - _ff->off < kFfmpegCap) {
            outI     = n;
            fds[n++] = {_ff->c.out, POLLIN, 0};
        }
        if (_ff && _ff->c.err >= 0) {
            errI     = n;
            fds[n++] = {_ff->c.err, POLLIN, 0};
        }
        const bool sinkExiting = _sink.pid > 0 && (_inputDone || _sinkGone);
        const bool ffExiting   = _ff && _ff->c.pid > 0 && _ff->c.out < 0 && _ff->c.err < 0;
        if (::poll(fds, nfds_t(n), sinkExiting || ffExiting ? 20 : -1) < 0)
            return;
        if (sinkI >= 0 && (fds[sinkI].revents & (POLLHUP | POLLERR)))
            _sinkGone = true;
        if (outI >= 0 && fds[outI].revents) {
            const size_t before = _ff->buf.size();
            drain(_ff->c.out, _ff->buf);
            if (_ff->buf.size() > before)
                _ff->gotData = true;
        }
        if (errI >= 0 && fds[errI].revents) {
            drain(_ff->c.err, _ff->err);
            parseDuration();
            if (_ff->err.size() > 16384)
                _ff->err.erase(0, _ff->err.size() - 4096);
        }
        bool ok = false;
        if (_ff && _ff->c.pid > 0 && _ff->c.out < 0 && _ff->c.err < 0 && reaped(_ff->c, &ok))
            ffmpegFinished(ok);
        if (_sink.pid > 0 && (_inputDone || _sinkGone) && reaped(_sink, &ok)) {
            closeFd(_sink.in);
            sinkFinished(ok);
        }
    }

    std::shared_ptr<PlayerShared> _s;
    uint64_t                      _gen = 0; // of the media loaded now

    pcm_decoder            *_dec      = nullptr;
    int64_t                 _decTotal = 0;
    bool                    _decEof   = false;
    std::unique_ptr<Ffmpeg> _ff;
    PcmFormat               _fmt;
    bool                    _probing = false;

    Child             _sink;
    std::string       _pending;
    size_t            _pendingOff   = 0;
    int64_t           _writtenBytes = 0;
    bool              _playing = false, _inputDone = false, _sinkGone = false;
    int               _attempt     = 0;
    int64_t           _baseFrame   = 0,
                      _pausedFrame = 0; // source frame the sink started at / position while stopped
    Clock::time_point _sinkStart;
};

class LinuxPlayer final : public Player {
public:
    explicit LinuxPlayer(App &app) {
        _s->app   = &app;
        _s->owner = this;
        auto *w   = new PlayerWorker(_s);
        _running  = startThread(&_thread, &PlayerWorker::entry, w, false);
        if (!_running)
            delete w;
    }
    ~LinuxPlayer() override {
        _s->owner = nullptr;
        if (_running) {
            send({PlayerShared::Op::Quit});
            ::pthread_join(_thread, nullptr);
        }
    }

    void load(const std::string &path) override {
        ++_gen;
        if (!_running) {
            _s->app->post([s = _s, gen = _gen] { deliver(*s, gen, PlayerEv::Failed, {}); });
            return;
        }
        send({PlayerShared::Op::Load, _gen, 0, path});
    }
    void play() override { send({PlayerShared::Op::Play}); }
    // Pause and seek also move the snapshot at once, so the position never
    // lags a seek bar by a worker round trip.
    void pause() override {
        {
            std::lock_guard lock(_s->m);
            if (_s->playing) {
                _s->pausedFrame = _s->currentFrame();
                _s->playing     = false;
            }
        }
        send({PlayerShared::Op::Pause});
    }
    void seek(int64_t ms) override {
        {
            std::lock_guard lock(_s->m);
            if (_s->hasMedia) {
                int64_t frame = std::max<int64_t>(0, ms * _s->fmt.rate / 1000);
                if (_s->total > 0)
                    frame = std::min(frame, _s->total);
                if (_s->playing) {
                    _s->baseFrame = frame;
                    _s->started   = Clock::now();
                    _s->written   = 0;
                } else {
                    _s->pausedFrame = frame;
                }
            }
        }
        send({PlayerShared::Op::Seek, 0, ms});
    }
    void stop() override { send({PlayerShared::Op::Stop, ++_gen}); }

    int64_t positionMs() const override {
        std::lock_guard lock(_s->m);
        return _s->hasMedia ? _s->currentFrame() * 1000 / _s->fmt.rate : 0;
    }
    int64_t durationMs() const override {
        std::lock_guard lock(_s->m);
        return _s->hasMedia ? _s->total * 1000 / _s->fmt.rate : 0;
    }

    // On the UI thread: hands a worker event to the callbacks, unless the
    // media it is about has been replaced (or the player is gone).
    static void deliver(PlayerShared &s, uint64_t gen, PlayerEv ev, const Failure &f) {
        LinuxPlayer *p = s.owner;
        if (!p || gen != p->_gen)
            return;
        switch (ev) {
        case PlayerEv::Loaded:
            if (p->onLoaded)
                p->onLoaded();
            break;
        case PlayerEv::Ended:
            if (p->onEnded)
                p->onEnded();
            break;
        case PlayerEv::Failed:
            if (p->onFailed)
                p->onFailed(f);
            break;
        }
    }

private:
    void send(PlayerShared::Cmd c) {
        if (!_running)
            return;
        {
            std::lock_guard lock(_s->m);
            _s->cmds.push_back(std::move(c));
        }
        _s->wake.ping();
    }

    std::shared_ptr<PlayerShared> _s = std::make_shared<PlayerShared>();
    pthread_t                     _thread{};
    bool                          _running = false;
    uint64_t                      _gen     = 0;
};

void PlayerWorker::post(PlayerEv ev, Failure f) {
    _s->app->post([s = _s, gen = _gen, ev, f = std::move(f)] {
        LinuxPlayer::deliver(*s, gen, ev, f);
    });
}

// ── Capture ─────────────────────────────────────────────────────────────────

constexpr int    kCaptureCount    = 4;
constexpr int    kLevelIntervalMs = 50;   // onLevel at ~20 Hz
constexpr int    kFirstDataMs     = 4000; // helper running but silent this long = broken
constexpr int    kStopGraceMs     = 1000; // helper ignoring SIGTERM this long = SIGKILL
constexpr size_t kLevelWindowBytes =
    size_t(Recorder::kSampleRate) * Recorder::kChannels * 2 * kLevelIntervalMs / 1000;

// Capture helper i writing raw 16 kHz mono s16le to stdout.
const char *captureArgs(int i, std::vector<std::string> *args) {
    const std::string rate = std::to_string(Recorder::kSampleRate);
    const std::string ch   = std::to_string(Recorder::kChannels);
    switch (i) {
    case 0:
        *args = {"--raw", "--format", "s16", "--rate", rate, "--channels", ch, "-"};
        return "pw-record";
    case 1:
        *args = {"--raw", "--format=s16le", "--rate=" + rate, "--channels=" + ch};
        return "parecord";
    case 2:
        *args = {"-q", "-t", "raw", "-f", "S16_LE", "-r", rate, "-c", ch, "-"};
        return "arecord";
    default:
        *args = {"-h", "raw", "-e", "s16le", "-r", rate, "-c", "0:0", "-o", "-"};
        return "aucat";
    }
}

class LinuxRecorder;

struct RecorderShared {
    std::mutex     m;
    bool           stop = false, cancel = false;
    Wake           wake;
    App           *app     = nullptr;
    LinuxRecorder *owner   = nullptr; // UI thread only
    uint64_t       session = 0;
};

enum class RecEv : uint8_t { Started, Level, Finished, Failed };

// One recording: launches the first capture helper that starts; one that
// exits before any audio arrives (pw-record without a running PipeWire,
// arecord on a busy device, …) hands over to the next. onStarted fires on
// the first PCM, so a helper that runs but never delivers times out.
class RecorderWorker {
public:
    explicit RecorderWorker(std::shared_ptr<RecorderShared> s) : _s(std::move(s)) {}
    ~RecorderWorker() { linux_process::kill(_c); }

    static void *entry(void *p) {
        auto *w = static_cast<RecorderWorker *>(p);
        w->run();
        delete w;
        return nullptr;
    }

private:
    void post(RecEv ev, float level = 0, std::string wav = {}, Failure f = {});

    bool launch(int from) {
        for (int i = from; i < kCaptureCount; ++i) {
            std::vector<std::string> args;
            const std::string        exe = findHelper(captureArgs(i, &args));
            if (exe.empty() || !linux_process::spawn(exe, args, kStdout | kStderr, &_c))
                continue;
            _attempt     = i;
            _firstDataAt = Clock::now() + std::chrono::milliseconds(kFirstDataMs);
            return true;
        }
        return false;
    }

    void run() {
        if (!launch(0)) {
            post(RecEv::Failed, 0, {}, {Error::NoCaptureTool, {}});
            return;
        }
        for (;;) {
            // Clear before reading (as the player does).
            _s->wake.clear();
            bool stop, cancel;
            {
                std::lock_guard lock(_s->m);
                stop   = _s->stop;
                cancel = _s->cancel;
            }
            if (cancel)
                return;
            if (stop && !_stopping) {
                _stopping = true;
                if (_c.pid > 0)
                    ::kill(_c.pid, SIGTERM); // every helper flushes and exits
                _graceAt = Clock::now() + std::chrono::milliseconds(kStopGraceMs);
            }

            const auto now     = Clock::now();
            int64_t    timeout = -1;
            auto       until   = [&](Clock::time_point t) {
                const int64_t ms = std::max<int64_t>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(t - now).count() + 1
                );
                timeout = timeout < 0 ? ms : std::min(timeout, ms);
            };
            if (!_gotData && !_stopping)
                until(_firstDataAt);
            if (_stopping && !_killed)
                until(_graceAt);
            if (!_levels.empty())
                until(_levelAt);
            if (_c.out < 0 && _c.err < 0)
                until(now + std::chrono::milliseconds(20)); // waiting to reap
            pollfd fds[3];
            int    n = 0, outI = -1, errI = -1;
            fds[n++] = {_s->wake.r, POLLIN, 0};
            if (_c.out >= 0) {
                outI     = n;
                fds[n++] = {_c.out, POLLIN, 0};
            }
            if (_c.err >= 0) {
                errI     = n;
                fds[n++] = {_c.err, POLLIN, 0};
            }
            if (::poll(fds, nfds_t(n), int(timeout)) < 0 && errno != EINTR)
                return;
            if (errI >= 0 && fds[errI].revents) {
                drain(_c.err, _stderr);
                if (_stderr.size() > 2048)
                    _stderr.erase(0, _stderr.size() - 1024);
            }
            if (outI >= 0 && fds[outI].revents)
                readPcm();

            const auto t = Clock::now();
            if (!_levels.empty() && t >= _levelAt) {
                post(RecEv::Level, _levels.front());
                _levels.erase(_levels.begin());
                _levelAt += std::chrono::milliseconds(kLevelIntervalMs);
                if (_levelAt < t)
                    _levelAt = t + std::chrono::milliseconds(kLevelIntervalMs);
            }
            if (!_gotData && !_stopping && t >= _firstDataAt) {
                linux_process::kill(_c);
                post(RecEv::Failed, 0, {}, {Error::NoAudio, lastLine(_stderr)});
                return;
            }
            if (_stopping && !_killed && t >= _graceAt) {
                if (_c.pid > 0)
                    ::kill(_c.pid, SIGKILL); // the take still arrives below
                _killed = true;
            }
            bool ok = false;
            if (_c.out < 0 && _c.err < 0 && reaped(_c, &ok) && !exited())
                return;
        }
    }

    void readPcm() {
        const size_t before = _pcm.size();
        drain(_c.out, _pcm);
        if (_pcm.size() == before)
            return;
        if (!_gotData) {
            _gotData = true;
            _levelAt = Clock::now();
            post(RecEv::Started);
        }
        // The helpers' stdio buffers hand PCM over in ~4 KiB bursts (~130 ms
        // of audio): each burst is cut into 50 ms windows played out one by
        // one on a steady clock, at most ~200 ms behind.
        for (; _levelFrom + kLevelWindowBytes <= _pcm.size(); _levelFrom += kLevelWindowBytes)
            _levels.push_back(peakOf(_pcm.data() + _levelFrom, kLevelWindowBytes));
        while (_levels.size() > 4)
            _levels.erase(_levels.begin());
    }

    // The helper exited: deliver the take, try the next helper, or fail.
    // False when this recording is over.
    bool exited() {
        if (_stopping) {
            _pcm.resize(_pcm.size() & ~size_t(1)); // whole samples only (the header is even)
            wavInPlace(_pcm, Recorder::kSampleRate, Recorder::kChannels);
            post(RecEv::Finished, 0, std::move(_pcm));
            return false;
        }
        // Before any audio this is a helper that can't reach its sound server;
        // after audio it's a lost device.
        const std::string detail = lastLine(_stderr);
        if (!_gotData) {
            _stderr.clear();
            if (launch(_attempt + 1))
                return true;
            post(RecEv::Failed, 0, {}, {Error::StartFailed, detail});
            return false;
        }
        post(RecEv::Failed, 0, {}, {Error::Stopped, detail});
        return false;
    }

    std::shared_ptr<RecorderShared> _s;
    Child                           _c;
    std::string                     _pcm = std::string(kWavHeader, '\0'); // WAV header room first
    std::string                     _stderr;
    std::vector<float>              _levels; // pending onLevel values, oldest first
    size_t                          _levelFrom = kWavHeader;
    Clock::time_point               _firstDataAt, _graceAt, _levelAt;
    int                             _attempt  = 0;
    bool                            _gotData  = false;
    bool                            _stopping = false, _killed = false;
};

class LinuxRecorder final : public Recorder {
public:
    explicit LinuxRecorder(App &app) : _app(app) {}
    ~LinuxRecorder() override { cancel(); }

    void start() override {
        if (_recording)
            return;
        join();
        _s          = std::make_shared<RecorderShared>();
        _s->app     = &_app;
        _s->owner   = this;
        _s->session = ++_session;
        _recording  = true;
        auto *w     = new RecorderWorker(_s);
        _running    = startThread(&_thread, &RecorderWorker::entry, w, false);
        if (!_running) {
            delete w;
            _app.post([s = _s] { deliver(*s, RecEv::Failed, 0, {}, {Error::StartFailed, {}}); });
        }
    }

    void stop() override {
        if (!_recording || !_s)
            return;
        {
            std::lock_guard lock(_s->m);
            _s->stop = true;
        }
        _s->wake.ping();
    }

    void cancel() override {
        if (_s) {
            {
                std::lock_guard lock(_s->m);
                _s->cancel = true;
            }
            _s->wake.ping();
            _s->owner = nullptr; // whatever it posted is dropped
        }
        join();
        _recording = false;
    }

    bool isRecording() const override { return _recording; }

    static void
    deliver(RecorderShared &s, RecEv ev, float level, std::string wav, const Failure &f) {
        LinuxRecorder *r = s.owner;
        if (!r || s.session != r->_session)
            return;
        switch (ev) {
        case RecEv::Started:
            if (r->onStarted)
                r->onStarted();
            break;
        case RecEv::Level:
            if (r->_recording && r->onLevel)
                r->onLevel(level);
            break;
        case RecEv::Finished:
            r->_recording = false;
            if (r->onFinished)
                r->onFinished(std::move(wav));
            break;
        case RecEv::Failed:
            r->_recording = false;
            if (r->onFailed)
                r->onFailed(f);
            break;
        }
    }

private:
    void join() {
        if (_running)
            ::pthread_join(_thread, nullptr);
        _running = false;
        _s.reset();
    }

    App                            &_app;
    std::shared_ptr<RecorderShared> _s;
    pthread_t                       _thread{};
    uint64_t                        _session   = 0;
    bool                            _running   = false;
    bool                            _recording = false;
};

void RecorderWorker::post(RecEv ev, float level, std::string wav, Failure f) {
    _s->app->post([s = _s, ev, level, wav = std::move(wav), f = std::move(f)]() mutable {
        LinuxRecorder::deliver(*s, ev, level, std::move(wav), f);
    });
}

// ── Notification sounds ─────────────────────────────────────────────────────

using linux_files::isDir;
using linux_files::isFile;

// Base dirs that hold sound themes, most specific first (user overrides system).
std::vector<std::string> soundBaseDirs() {
    std::vector<std::string> dirs;
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x)
        dirs.push_back(std::string(x) + "/sounds");
    if (const char *h = std::getenv("HOME"); h && *h)
        dirs.push_back(std::string(h) + "/.local/share/sounds");
    dirs.push_back("/usr/local/share/sounds");
    dirs.push_back("/usr/share/sounds");
    return dirs;
}

// Best-effort current theme name (GNOME's setting); the freedesktop default
// otherwise. Waits at most ~800 ms for gsettings.
std::string currentThemeName() {
    const std::string exe = soundHelper("gsettings");
    Child             c;
    std::string       out;
    if (!exe.empty() &&
        linux_process::spawn(exe, {"get", "org.gnome.desktop.sound", "theme-name"}, kStdout, &c)) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(800);
        while (c.out >= 0 && Clock::now() < deadline) {
            pollfd p{c.out, POLLIN, 0};
            ::poll(&p, 1, 50);
            drain(c.out, out);
        }
        bool ok = false;
        if (c.out >= 0 || !reaped(c, &ok))
            linux_process::kill(c);
        closeFd(c.out);
        std::string name;
        for (char ch : out)
            if (ch != '\'' && ch != '"' && ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t')
                name += ch;
        if (!name.empty())
            return name;
    }
    return "freedesktop";
}

// Directory holding a theme's event sounds (…/<theme>/stereo), or "".
std::string themeStereoDir(const std::string &theme) {
    for (const std::string &base : soundBaseDirs())
        if (const std::string dir = base + "/" + theme + "/stereo"; isDir(dir))
            return dir;
    return {};
}

std::string activeThemeDir() {
    {
        std::lock_guard lock(soundCache().mutex);
        if (const auto &d = freshSoundCache().themeDir)
            return *d;
    }
    std::string dir = themeStereoDir(currentThemeName());
    if (dir.empty())
        dir = themeStereoDir("freedesktop");
    std::lock_guard lock(soundCache().mutex);
    freshSoundCache().themeDir = dir;
    return dir;
}

// "message-new-instant" -> "Message new instant"
std::string prettify(std::string name) {
    for (char &c : name)
        if (c == '-' || c == '_')
            c = ' ';
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
        name[0] = char(name[0] - 'a' + 'A');
    return name;
}

// Tries the common players in order; the first one that launches wins.
bool playFile(const std::string &path) {
    return runHelper("pw-play", {path}) || runHelper("paplay", {path}) ||
           runHelper("ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", path}) ||
           runHelper("aplay", {"-q", path}) || // WAV only, fine for the bundled chime
           runHelper("aucat", {"-i", path}) || // WAV too
           runHelper("canberra-gtk-play", {"-f", path});
}

bool playSystem(const std::string &name) {
    // canberra first: it understands the theme and decodes .oga natively.
    if (runHelper("canberra-gtk-play", {"-i", name}))
        return true;
    // Otherwise resolve the theme file and play it directly.
    if (const std::string dir = activeThemeDir(); !dir.empty())
        for (const char *ext : {".oga", ".ogg", ".wav"})
            if (const std::string path = dir + "/" + name + ext; isFile(path))
                return playFile(path);
    return false;
}

struct SoundJob {
    std::string name, file;
};

void *soundThread(void *p) {
    std::unique_ptr<SoundJob> job(static_cast<SoundJob *>(p));
    if (!job->name.empty() && playSystem(job->name))
        return nullptr;
    if (!job->file.empty())
        playFile(job->file);
    return nullptr;
}

} // namespace

std::unique_ptr<Player> Player::create(App &app) {
    return std::make_unique<LinuxPlayer>(app);
}

bool canPlayExtension(std::string_view ext) {
    for (const char *e : kInProcess)
        if (ext == e)
            return true;
    return !findHelper("ffmpeg").empty();
}

std::unique_ptr<Recorder> createNativeRecorder(App &app) {
    return std::make_unique<LinuxRecorder>(app);
}

std::vector<SystemSound> systemSounds() {
    std::vector<SystemSound> out;
    const std::string        dir = activeThemeDir();
    if (dir.empty())
        return out;
    std::vector<std::string> files;
    if (DIR *d = ::opendir(dir.c_str())) {
        while (const dirent *e = ::readdir(d)) {
            const std::string f   = e->d_name;
            const size_t      dot = f.rfind('.');
            if (dot == std::string::npos || dot == 0)
                continue;
            const std::string ext = f.substr(dot);
            if ((ext == ".oga" || ext == ".ogg" || ext == ".wav") && isFile(dir + "/" + f))
                files.push_back(f);
        }
        ::closedir(d);
    }
    std::sort(files.begin(), files.end());
    for (const std::string &f : files) {
        const std::string name = f.substr(0, f.rfind('.'));
        bool              seen = false;
        for (const auto &s : out)
            seen = seen || s.name == name;
        if (!seen)
            out.push_back({name, prettify(name)});
    }
    return out;
}

void playSound(const std::string &name, const std::string &fallbackFile) {
    auto     *job = new SoundJob{name, fallbackFile};
    pthread_t t;
    if (!startThread(&t, soundThread, job, true))
        delete job;
}

} // namespace plat::audio
