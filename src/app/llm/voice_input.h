// Voice input for the composer: record → speech-to-text
// (gpt-transcribe on the OpenAI wire, Service::sttProvider) → optional AI
// clean-up → the text handed back to the composer that started it. One
// recording app-wide, but a recording already stopped keeps transcribing
// while the next one starts (its composer may have moved on: the text still
// lands in its draft). The composers never touch the recorder or the wire.
//
//   VoiceInput &vi = service.voice();
//   id = vi.observe({.finished = [this](const void *owner, const std::string &text) {
//       if (owner == this) insertAtCursor(text);
//   }, …});
//   vi.start(this, contextForCurrentConversation());
//   …
//   vi.stop(); // or vi.cancel()
//
// The microphone is plat::audio::Recorder (in test builds $MSGA_VOICE_FAKE_WAV
// replaces it with a file). The glossary and the clean-up
// switch are Settings → AI assistance → Voice input; the shell pushes them
// in. UI thread only (listeners run on it; start() may report a failure
// inside the call).
#pragma once

#include "app/llm/voice_prompt.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plat {
class App;
namespace audio {
class Recorder;
}
} // namespace plat

namespace llm {

class Service;

class VoiceInput {
public:
    enum class State : uint8_t { Idle, Recording, Transcribing, Cleaning };

    // Recordings longer than this stop on their own (OpenAI's upload limit is
    // 25 MB; 16 kHz mono s16 is ~1.9 MB a minute).
    static constexpr int   kMaxRecordingMs = 10 * 60 * 1000;
    // Below this there is nothing worth an API call (a click on the mic and
    // straight off again), and Whisper-style models hallucinate on silence.
    static constexpr int   kMinSpeechMs    = 300;
    // Peak amplitude (0..1) a recording must reach somewhere to count as
    // sound: ~−46 dBFS, under quiet speech, above a muted mic's zero.
    static constexpr float kSilencePeak    = 0.005f;

    VoiceInput(plat::App &app, Service &service);
    ~VoiceInput();
    VoiceInput(const VoiceInput &)            = delete;
    VoiceInput &operator=(const VoiceInput &) = delete;

    // A connected provider can do speech-to-text (the composer shows its mic).
    bool        available() const;
    // The newest dictation in flight (Idle / null when there is none).
    State       state() const;
    const void *owner() const;
    // The dictation in flight for `owner` (Idle when it has none).
    State       stateOf(const void *owner) const;

    // Starts recording for `owner`; a recording already running (anyone's)
    // and whatever `owner` had in flight are cancelled first. Others' stopped
    // recordings go on transcribing. Without a provider: failed() at once.
    void start(const void *owner, VoiceContext ctx);
    // Stops the recording and transcribes it. Ignored unless one is running.
    void stop();
    // Abandons whatever is in flight (recording, transcription or clean-up)
    // — everyone's / owner's; no finished / failed follows.
    void cancel();
    void cancel(const void *owner);

    // Settings → Voice input: terms always sent as keywords; the AI clean-up.
    void setGlossary(std::vector<std::string> terms) { _glossary = std::move(terms); }
    void setCleanup(bool on) { _cleanup = on; }
    const std::vector<std::string> &glossary() const { return _glossary; }
    bool                            cleanup() const { return _cleanup; }

    // Tests: replaces plat::audio::Recorder::create.
    std::function<std::unique_ptr<plat::audio::Recorder>()> recorderFactory;

    struct Listener {
        std::function<void(State)>                                      stateChanged;
        std::function<void(float peak)>                                 level; // while Recording
        // The final text (cleaned up when that is on) / a translated error.
        std::function<void(const void *owner, const std::string &text)> finished;
        std::function<void(const void *owner, const std::string &err)>  failed;
    };
    using ObserverId = uint32_t;
    ObserverId observe(Listener l);
    void       unobserve(ObserverId id);

    // Duration and peak of a 16-bit PCM RIFF/WAVE file (others read as empty).
    // The scan stops at the first sample reaching `enough`: then `peak` is
    // that sample's, not necessarily the loudest.
    struct WavStats {
        int64_t durationMs = 0;
        float   peak       = 0;
    };
    static WavStats analyseWav(std::string_view wav, float enough = 2.0f);

private:
    // One dictation: its recorder while recording, then the requests. Async
    // callbacks hold it and are no-ops once it is `over` (cancelled or done).
    struct Job {
        const void                            *owner = nullptr;
        VoiceContext                           ctx;
        State                                  state = State::Recording;
        bool                                   over  = false;
        std::unique_ptr<plat::audio::Recorder> recorder;
        uint64_t                               maxTimer = 0;
    };
    using JobPtr = std::shared_ptr<Job>;

    Job *recording() const;
    void setState(const JobPtr &j, State s);
    void onRecorded(const JobPtr &j, std::string wav);
    void onTranscribed(const JobPtr &j, const std::string &text);
    void succeed(const JobPtr &j, const std::string &text); // ends it, then finished()
    void fail(const JobPtr &j, const std::string &error);   // ends it, then failed()
    void end(const JobPtr &j);                              // over, out of _jobs, Idle reported
    void releaseRecorder(Job &j);
    template <class F>
    void each(F f);

    plat::App               &_app;
    Service                 &_service;
    std::vector<JobPtr>      _jobs; // in start order; at most one Recording (the newest)
    std::vector<std::string> _glossary;
    bool                     _cleanup = true;
    struct Slot {
        ObserverId id;
        Listener   l;
    };
    std::vector<Slot> _listeners; // id 0: removed during each(), dropped after it
    std::vector<Slot> _added;     // observed during each(), joining after it
    ObserverId        _nextId = 1;
    int               _depth  = 0; // each() nesting
};

} // namespace llm
