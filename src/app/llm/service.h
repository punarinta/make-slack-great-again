// The app's LLM facade: the configured providers, the
// one requests go to, and the calls themselves over net::Client.
//
//   service.chat(req, [](llm::ChatResult r) { … r.ok ? r.response.text : r.error … });
//   service.transcribe(in, [](llm::TranscriptionResult r) { … });
//
// The shell owns the configuration (Settings → AI assistance, persisted in
// shell::Settings) and pushes it in with setProviders / setLanguage after
// every change; callers only ask available(), active() and chat().
//
// Threading: UI thread only. Every `done` runs later on the UI thread —
// never inside the call that started it, also when it fails at once (no
// provider) — and never after the Service is destroyed.
#pragma once

#include "app/llm/provider.h"
#include "base/observers.h"
#include "net/net.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace llm {

class AudioTranscriber;
class VoiceInput;

class Service {
public:
    explicit Service(plat::App &app);
    ~Service();
    Service(const Service &)            = delete;
    Service &operator=(const Service &) = delete;

    // Presets first, then custom servers; defaultId = the user's pick.
    void setProviders(std::vector<Provider> providers, std::string defaultId);
    const std::vector<Provider> &providers() const { return _providers; }
    // The language AI features address the user in (ISO 639-1, "en").
    void                         setLanguage(std::string code) { _language = std::move(code); }
    const std::string           &language() const { return _language; }

    // What chat() routes to: the default while connected, else the first
    // connected provider, else null.
    const Provider *active() const;
    bool            available() const { return active() != nullptr; }

    // What transcribe() routes to: active() when it has a transcription
    // endpoint, else the first connected provider that does (so an Anthropic
    // default still transcribes through a connected OpenAI-compatible one),
    // else null. Never the stand-in (it answers chats only).
    const Provider *sttProvider() const;

    using ChatDone       = std::function<void(ChatResult)>;
    using ModelsDone     = std::function<void(ModelsResult)>;
    using TranscribeDone = std::function<void(TranscriptionResult)>;
    // A completion on active() (req.model "" → its model).
    net::RequestId chat(Request req, ChatDone done);
    // …on this provider.
    net::RequestId chat(const Provider &p, Request req, ChatDone done);
    // GET /models, which doubles as the connection and key test (the
    // settings editor's "Test connection" / "Fetch models", for a provider
    // that need not be saved yet).
    net::RequestId listModels(const Provider &p, ModelsDone done);
    // Speech-to-text on sttProvider() (in.model "" → its sttModel). No such
    // provider: the error says which setting is missing.
    net::RequestId transcribe(TranscriptionInput in, TranscribeDone done);
    // …on this provider; one without a transcription endpoint refuses.
    net::RequestId transcribe(const Provider &p, TranscriptionInput in, TranscribeDone done);
    void           cancel(net::RequestId id) { _net.cancel(id); }

    // The "Transcribe with AI" results of audio files (audio_transcriber.h).
    AudioTranscriber &transcriber() { return *_transcriber; }
    // The composers' dictation (voice_input.h): one recording app-wide.
    VoiceInput       &voice() { return *_voice; }

    // After every setProviders (the composers show or hide their mic).
    uint32_t observeProviders(std::function<void()> fn);
    void     unobserveProviders(uint32_t id);

    // A provider answered in-process by `answer` (given the request's text)
    // instead of a server; active() falls back to it while no real provider
    // is connected. It never transcribes.
    void setStandIn(Provider p, std::function<std::string(std::string_view request)> answer);
    bool isStandIn(const Provider *p) const { return _standInAnswer && p == &_standIn; }

private:
    // `done` with `error`, later (never re-entered), unless the Service is gone.
    void fail(TranscribeDone done, std::string error);
    void failChat(ChatDone done, std::string error);

    plat::App                                   &_app;
    net::Client                                  _net;
    std::vector<Provider>                        _providers;
    std::string                                  _defaultId, _language = "en";
    Provider                                     _standIn;
    std::function<std::string(std::string_view)> _standInAnswer;
    std::unique_ptr<AudioTranscriber>            _transcriber;
    std::unique_ptr<VoiceInput>                  _voice;
    base::Observers                              _providerObservers;
    std::shared_ptr<char>                        _alive = std::make_shared<char>(0);
};

} // namespace llm
