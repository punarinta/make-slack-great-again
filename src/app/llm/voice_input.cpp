#include "app/llm/voice_input.h"

#include "app/llm/service.h"
#include "app/media/audio_errors.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "plat/audio.h"
#include "plat/plat.h"
#include "prim/bytes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace llm {

using i18n::arg;
using i18n::tr;

namespace {

uint32_t le32(std::string_view s, size_t at) {
    return prim::le32(s.data() + at);
}
uint16_t le16(std::string_view s, size_t at) {
    return uint16_t(prim::le16(s.data() + at));
}

// A plausible ISO 639-1 code ("sv"): two ASCII letters.
bool isLanguageCode(std::string_view c) {
    return c.size() == 2 && c[0] >= 'a' && c[0] <= 'z' && c[1] >= 'a' && c[1] <= 'z';
}

} // namespace

VoiceInput::VoiceInput(plat::App &app, Service &service) : _app(app), _service(service) {}

VoiceInput::~VoiceInput() {
    for (const JobPtr &j : _jobs) {
        j->over = true;
        if (j->recorder)
            j->recorder->cancel();
        if (j->maxTimer)
            _app.cancelTimer(j->maxTimer);
    }
}

bool VoiceInput::available() const {
    return _service.sttProvider() != nullptr;
}

VoiceInput::State VoiceInput::state() const {
    return _jobs.empty() ? State::Idle : _jobs.back()->state;
}

const void *VoiceInput::owner() const {
    return _jobs.empty() ? nullptr : _jobs.back()->owner;
}

VoiceInput::State VoiceInput::stateOf(const void *owner) const {
    if (!owner)
        return State::Idle;
    for (const JobPtr &j : _jobs)
        if (j->owner == owner)
            return j->state;
    return State::Idle;
}

// Only ever the newest: start() cancels a running one before adding its own.
VoiceInput::Job *VoiceInput::recording() const {
    return !_jobs.empty() && _jobs.back()->state == State::Recording ? _jobs.back().get() : nullptr;
}

VoiceInput::ObserverId VoiceInput::observe(Listener l) {
    // While each() walks _listeners it must not grow (a reallocation would
    // move the listener being called): new ones wait in _added.
    (_depth > 0 ? _added : _listeners).push_back({_nextId, std::move(l)});
    return _nextId++;
}

void VoiceInput::unobserve(ObserverId id) {
    if (std::erase_if(_added, [id](const Slot &s) { return s.id == id; }))
        return;
    for (auto it = _listeners.begin(); it != _listeners.end(); ++it)
        if (it->id == id) {
            if (_depth > 0)
                it->id = 0; // each() is walking the list: dropped after it
            else
                _listeners.erase(it);
            return;
        }
}

// A listener may unobserve (a composer going away) or observe mid-call: the
// removed one isn't called any more, the added one from the next call on.
template <class F>
void VoiceInput::each(F f) {
    ++_depth;
    for (size_t i = 0; i < _listeners.size(); ++i)
        if (_listeners[i].id)
            f(_listeners[i].l);
    if (--_depth > 0)
        return;
    std::erase_if(_listeners, [](const Slot &s) { return s.id == 0; });
    for (Slot &s : _added)
        _listeners.push_back(std::move(s));
    _added.clear();
}

void VoiceInput::setState(const JobPtr &j, State s) {
    if (j->state == s)
        return;
    j->state = s;
    each([s](const Listener &l) {
        if (l.stateChanged)
            l.stateChanged(s);
    });
}

void VoiceInput::start(const void *owner, VoiceContext ctx) {
    // One microphone; and a new dictation for `owner` replaces its last.
    if (Job *r = recording())
        cancel(r->owner);
    cancel(owner);
    if (!available()) {
        const std::string err =
            tr("Voice input needs a speech-to-text provider. Connect OpenAI or an "
               "OpenAI-compatible server in Settings \xE2\x86\x92 AI assistance.");
        each([owner, &err](const Listener &l) {
            if (owner && l.failed)
                l.failed(owner, err);
        });
        return;
    }
    auto j      = std::make_shared<Job>();
    j->owner    = owner;
    j->ctx      = std::move(ctx);
    j->state    = State::Idle; // until setState reports Recording below
    j->recorder = recorderFactory ? recorderFactory() : plat::audio::Recorder::create(_app);
    _jobs.push_back(j);
    if (!j->recorder) {
        fail(j, tr("No microphone recording is available on this system"));
        return;
    }
    const std::weak_ptr<Job> w = j;
    j->recorder->onLevel       = [this, w](float peak) {
        const JobPtr j = w.lock();
        if (!j || j->over || j->state != State::Recording)
            return;
        each([peak](const Listener &l) {
            if (l.level)
                l.level(peak);
        });
    };
    j->recorder->onFinished = [this, w](std::string wav) {
        if (const JobPtr j = w.lock(); j && !j->over)
            onRecorded(j, std::move(wav));
    };
    j->recorder->onFailed = [this, w](const plat::audio::Failure &f) {
        if (const JobPtr j = w.lock(); j && !j->over)
            fail(j, arg(tr("Couldn't record: %1"), media::audioErrorText(f)));
    };
    setState(j, State::Recording);
    if (j->over)
        return; // a stateChanged listener cancelled already
    j->maxTimer = _app.addTimer(kMaxRecordingMs, false, [this, w] {
        const JobPtr j = w.lock();
        if (!j)
            return;
        j->maxTimer = 0;
        if (recording() == j.get())
            stop();
    });
    j->recorder->start();
}

void VoiceInput::stop() {
    if (!recording() || !_jobs.back()->recorder)
        return;
    const JobPtr j = _jobs.back();
    if (j->maxTimer)
        _app.cancelTimer(std::exchange(j->maxTimer, 0));
    // Transcribing from the moment the user lets go: onFinished may take a
    // moment (the capture helper flushing its pipe).
    setState(j, State::Transcribing);
    if (!j->over && j->recorder) // a stateChanged listener may have cancelled
        j->recorder->stop();
}

void VoiceInput::cancel() {
    while (!_jobs.empty()) {
        const JobPtr j = _jobs.back();
        if (j->recorder)
            j->recorder->cancel();
        end(j);
    }
}

void VoiceInput::cancel(const void *owner) {
    for (size_t i = _jobs.size(); i-- > 0;) {
        if (i >= _jobs.size() || _jobs[i]->owner != owner)
            continue; // a listener of end() may have cancelled more
        const JobPtr j = _jobs[i];
        if (j->recorder)
            j->recorder->cancel();
        end(j);
    }
}

VoiceInput::WavStats VoiceInput::analyseWav(std::string_view wav, float enough) {
    WavStats st;
    if (wav.size() < 12 || wav.substr(0, 4) != "RIFF" || wav.substr(8, 4) != "WAVE")
        return st;
    int    channels = plat::audio::Recorder::kChannels;
    int    rate     = plat::audio::Recorder::kSampleRate;
    int    bits     = 16;
    size_t pos      = 12;
    while (pos + 8 <= wav.size()) {
        const std::string_view id   = wav.substr(pos, 4);
        const size_t           size = le32(wav, pos + 4);
        const size_t           body = pos + 8;
        if (id == "fmt " && size >= 16 && body + 16 <= wav.size()) {
            channels = le16(wav, body + 2);
            rate     = int(le32(wav, body + 4));
            bits     = le16(wav, body + 14);
        } else if (id == "data") {
            if (bits != 16 || channels <= 0 || rate <= 0)
                return st;
            // A streaming writer may leave the size unset/oversized: clamp.
            const size_t len    = std::min(size, wav.size() - body);
            const size_t frames = len / (2 * size_t(channels));
            st.durationMs       = int64_t(frames) * 1000 / rate;
            // Any sample this loud ends the scan (19 MB at the length limit).
            const int stopAt    = enough > 1.0f ? 32769 : int(std::ceil(enough * 32768.0f));
            int       maxAbs    = 0;
            for (size_t i = 0; i + 1 < len && maxAbs < stopAt; i += 2)
                maxAbs = std::max(maxAbs, std::abs(int(int16_t(le16(wav, body + i)))));
            st.peak = float(maxAbs) / 32768.0f;
            return st;
        }
        if (size > wav.size())
            break;
        pos = body + size + (size & 1); // chunks are word-aligned
    }
    return st;
}

void VoiceInput::onRecorded(const JobPtr &j, std::string wav) {
    releaseRecorder(*j);
    setState(j, State::Transcribing);
    if (j->over)
        return;

    const WavStats st = analyseWav(wav, kSilencePeak); // sound or not, not how loud
    if (st.durationMs < kMinSpeechMs || st.peak < kSilencePeak) {
        fail(j, tr("No speech was recorded"));
        return;
    }
    const Provider *stt = _service.sttProvider();
    if (!stt) { // disconnected while recording
        fail(j, tr("Voice input needs a speech-to-text provider"));
        return;
    }
    const bool instructionModel = voice::isInstructionFollowingSttModel(stt->sttModel);

    TranscriptionInput in;
    in.audio                 = std::move(wav);
    in.fileName              = "voice.wav";
    in.mimeType              = "audio/wav";
    // in.model "" → the provider's STT model.
    in.prompt                = voice::buildSttPrompt(j->ctx, instructionModel);
    in.keywords              = voice::extractKeywords(j->ctx, _glossary);
    // Language hints: the user's language plus English — dictation in tech
    // teams is often English (or mixed) even for non-native speakers, and a
    // native-only hint would push English speech into the wrong language.
    // Only sent to gpt-* models (wire.h).
    const std::string native = str::asciiLower(str::trim(_service.language()));
    if (isLanguageCode(native))
        in.languages.push_back(native);
    if (native != "en")
        in.languages.push_back("en");

    _service.transcribe(*stt, std::move(in), [this, j](TranscriptionResult r) {
        if (j->over)
            return;
        if (r.ok)
            onTranscribed(j, r.text);
        else
            fail(j, arg(tr("Couldn't transcribe: %1"), r.error));
    });
}

void VoiceInput::onTranscribed(const JobPtr &j, const std::string &textIn) {
    const std::string raw(str::trim(textIn));
    if (raw.empty()) {
        fail(j, tr("No speech was recognised"));
        return;
    }
    if (!_cleanup || !_service.available()) {
        succeed(j, raw);
        return;
    }
    setState(j, State::Cleaning);
    if (j->over)
        return;
    Request req = voice::buildCleanupRequest(raw, j->ctx, _service.language());
    if (const Provider *p = _service.active())
        req.model = p->summaryModel();
    _service.chat(std::move(req), [this, j, raw](ChatResult r) {
        if (j->over)
            return;
        if (!r.ok) {
            succeed(j, raw); // clean-up is best effort
            return;
        }
        // Never lose the user's words: an empty or cut-off reply, or one far
        // longer than what was said (the model answered or added content
        // instead of cleaning), falls back to the raw transcript.
        const std::string cleaned(str::trim(r.response.text));
        const bool        truncated =
            r.response.stopReason == "length" || r.response.stopReason == "max_tokens";
        const bool runaway = utf8::countCodePoints(cleaned) > utf8::countCodePoints(raw) * 2 + 40;
        succeed(j, cleaned.empty() || truncated || runaway ? raw : cleaned);
    });
}

void VoiceInput::succeed(const JobPtr &j, const std::string &text) {
    const void *owner = j->owner;
    end(j);
    if (owner)
        each([owner, &text](const Listener &l) {
            if (l.finished)
                l.finished(owner, text);
        });
}

void VoiceInput::fail(const JobPtr &j, const std::string &error) {
    const void *owner = j->owner;
    end(j);
    if (owner)
        each([owner, &error](const Listener &l) {
            if (l.failed)
                l.failed(owner, error);
        });
}

void VoiceInput::end(const JobPtr &j) {
    if (j->over)
        return;
    j->over = true;
    releaseRecorder(*j);
    std::erase(_jobs, j);
    // Reported per dictation (its composer goes back to the plain mic), even
    // while another one is still in flight.
    if (std::exchange(j->state, State::Idle) != State::Idle)
        each([](const Listener &l) {
            if (l.stateChanged)
                l.stateChanged(State::Idle);
        });
}

void VoiceInput::releaseRecorder(Job &j) {
    if (j.maxTimer)
        _app.cancelTimer(std::exchange(j.maxTimer, 0));
    if (!j.recorder)
        return;
    // It may be the one calling us (onFinished / onFailed): destroyed later.
    std::shared_ptr<plat::audio::Recorder> rec(j.recorder.release());
    rec->onLevel   = nullptr;
    rec->onStarted = nullptr;
    _app.post([rec] {});
}

} // namespace llm
