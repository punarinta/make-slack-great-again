// msga_app_llm speech-to-text: the transcription wire, the Service's STT
// routing, the AudioTranscriber, the voice prompt shaping and VoiceInput end
// to end — the microphone is plat's fake recorder ($MSGA_VOICE_FAKE_WAV) and
// the provider tests/support/fake_llm.py, never a real one.
#include "app/llm/audio_transcriber.h"
#include "app/llm/provider.h"
#include "app/llm/service.h"
#include "support/fake_llm_server.h"
#include "app/llm/voice_input.h"
#include "app/llm/voice_prompt.h"
#include "app/llm/wire.h"
#include "base/file.h"
#include "base/json.h"
#include "support/test.h"
#include "base/utf8.h"
#include "plat/audio.h"
#include "plat/plat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace {

plat::App &app() {
    static std::unique_ptr<plat::App> a = plat::App::create();
    return *a;
}

template <class F>
bool pumpUntil(F done, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        app().pump(10);
    }
    return true;
}

std::string header(const net::Request &r, std::string_view name) {
    for (const net::Header &h : r.headers)
        if (h.name == name)
            return h.value;
    return "<none>";
}

net::Response answer(int status, std::string body) {
    net::Response r;
    r.status = status;
    r.body   = std::move(body);
    return r;
}

bool has(const std::string &hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

size_t count(const std::string &hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = 0; (at = hay.find(needle, at)) != std::string::npos; at += needle.size())
        ++n;
    return n;
}

// One second of 16 kHz mono: a 440 Hz tone (amplitude) or silence (0).
std::string wav(float amplitude, int ms = 1000) {
    std::string pcm;
    for (int i = 0; i < 16 * ms; ++i) {
        const int16_t v = int16_t(amplitude * 32767 * std::sin(i * 2 * 3.14159265 * 440 / 16000));
        pcm += char(v & 0xff);
        pcm += char((v >> 8) & 0xff);
    }
    return plat::audio::wavFromPcm16(pcm, 16000, 1);
}

// $MSGA_VOICE_FAKE_WAV for plat's recorder (null: unset).
void fakeMic(const char *wavPath) {
    if (wavPath)
        base::test::setEnv("MSGA_VOICE_FAKE_WAV", wavPath);
    else
        base::test::unsetEnv("MSGA_VOICE_FAKE_WAV");
}

llm::Provider fakeStt(const std::string &base, std::string sttModel = "gpt-transcribe") {
    return llm::fromSettings("custom-1", "Fake", base + "/v1", "", "fake-large", sttModel);
}

} // namespace

// ── Wire ───────────────────────────────────────────────────────────────────

TEST("stt wire: the multipart request") {
    llm::Endpoint ep;
    ep.baseUrl = "https://api.openai.com/v1/";
    ep.apiKey  = "sk";
    llm::TranscriptionInput in;
    in.audio             = std::string("RIFF\0\x01", 6);
    in.fileName          = "F1\"x.wav";
    in.mimeType          = "audio/wav";
    in.model             = "gpt-transcribe";
    in.prompt            = "Clean.";
    in.keywords          = {"msga", " MSGA ", "a<b", "", "Kubernetes"};
    in.languages         = {"SV", "en", "sv", "x\ny"};
    const net::Request r = llm::buildTranscription(ep, in, "BND");
    CHECK_STR(r.method, "POST");
    CHECK_STR(r.url, "https://api.openai.com/v1/audio/transcriptions");
    CHECK_STR(header(r, "Authorization"), "Bearer sk");
    CHECK_STR(header(r, "Content-Type"), "multipart/form-data; boundary=BND");
    const std::string &b = r.body;
    CHECK(
        has(b, "--BND\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\ngpt-transcribe\r\n")
    );
    CHECK(has(b, "name=\"response_format\"\r\n\r\njson\r\n"));
    CHECK(has(b, "name=\"prompt\"\r\n\r\nClean.\r\n"));
    // Sanitised keywords: duplicates (any case), '<'/'>' and empties dropped.
    CHECK(count(b, "name=\"keywords[]\"") == 2);
    CHECK(has(b, "name=\"keywords[]\"\r\n\r\nmsga\r\n"));
    CHECK(has(b, "name=\"keywords[]\"\r\n\r\nKubernetes\r\n"));
    CHECK(count(b, "name=\"languages[]\"") == 2);
    CHECK(has(b, "name=\"languages[]\"\r\n\r\nsv\r\n"));
    CHECK(has(b, "filename=\"F1_x.wav\"\r\nContent-Type: audio/wav\r\n\r\n"));
    CHECK(has(b, std::string("RIFF\0\x01", 6) + "\r\n--BND--\r\n"));

    // Whisper-style models get neither keywords nor languages; no key, no auth.
    in.model             = "whisper-1";
    ep.apiKey            = "";
    in.prompt            = "  ";
    const net::Request w = llm::buildTranscription(ep, in);
    CHECK_STR(header(w, "Authorization"), "<none>");
    CHECK_FALSE(has(w.body, "keywords[]"));
    CHECK_FALSE(has(w.body, "languages[]"));
    CHECK_FALSE(has(w.body, "name=\"prompt\""));
    CHECK(llm::supportsTranscription(llm::Format::OpenAiChat));
    CHECK_FALSE(llm::supportsTranscription(llm::Format::AnthropicMessages));
}

TEST("stt wire: answers, errors, mime types") {
    llm::TranscriptionResult r = llm::parseTranscription(answer(200, R"({"text":"  hello  "})"));
    CHECK(r.ok);
    CHECK_STR(r.text, "hello");
    r = llm::parseTranscription(answer(200, "plain words\n")); // ignores response_format
    CHECK(r.ok);
    CHECK_STR(r.text, "plain words");
    r = llm::parseTranscription(answer(404, "not found"));
    CHECK_FALSE(r.ok);
    CHECK_STR(r.error, "This server has no speech-to-text endpoint (HTTP 404)");
    r = llm::parseTranscription(answer(200, R"({"model":"x"})"));
    CHECK_FALSE(r.ok);
    CHECK_STR(r.error, "Unexpected response from server (no text)");
    r = llm::parseTranscription(answer(400, R"({"error":{"message":"bad audio"}})"));
    CHECK_FALSE(r.ok);
    CHECK_STR(r.error, "bad audio");
    CHECK_STR(llm::audioMimeForExtension("MP3"), "audio/mpeg");
    CHECK_STR(llm::audioMimeForExtension("m4a"), "audio/mp4");
    CHECK_STR(llm::audioMimeForExtension("xyz"), "");
}

// ── Service routing ────────────────────────────────────────────────────────

TEST("stt service: routing past a provider without speech-to-text") {
    llm::Service s(app());
    CHECK(s.sttProvider() == nullptr);
    llm::Provider anthropic = llm::fromSettings("anthropic", "Anthropic", "", "k", "", "");
    llm::Provider openai    = llm::fromSettings("openai", "OpenAI", "", "k", "", "");
    CHECK_STR(openai.sttModel, "gpt-transcribe");
    s.setProviders({anthropic}, "anthropic");
    CHECK(s.sttProvider() == nullptr);
    CHECK_FALSE(s.voice().available());
    llm::TranscriptionResult got;
    bool                     done = false;
    s.transcribe({}, [&](llm::TranscriptionResult r) {
        got  = std::move(r);
        done = true;
    });
    CHECK_FALSE(done); // later, never inside the call
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(got.ok);
    CHECK(has(got.error, "Anthropic does not support speech-to-text"));
    // The Anthropic provider asked directly refuses too.
    done = false;
    s.transcribe(anthropic, {}, [&](llm::TranscriptionResult r) {
        got  = std::move(r);
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_STR(got.error, "Anthropic does not support speech-to-text");
    // An Anthropic default still transcribes through a connected OpenAI.
    int            notified = 0;
    const uint32_t obs      = s.observeProviders([&] { ++notified; });
    s.setProviders({anthropic, openai}, "anthropic");
    CHECK(notified == 1);
    s.unobserveProviders(obs);
    REQUIRE(s.sttProvider());
    CHECK_STR(s.sttProvider()->id, "openai");
    CHECK_STR(s.active()->id, "anthropic");
    CHECK(s.voice().available());
    // The stand-in answers chats only.
    llm::Service standIn(app());
    standIn.setStandIn(
        llm::fromSettings("local", "Local AI", "local:", "", "local-1", ""),
        [](std::string_view) { return std::string("x"); }
    );
    CHECK(standIn.available());
    CHECK(standIn.sttProvider() == nullptr);
}

TEST("stt service: a transcript from the fake server, one request per file") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake provider\n");
        return;
    }
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    llm::Service s(app());
    s.setProviders({fakeStt(base, "")}, "custom-1"); // "" → the custom default, whisper-1
    llm::AudioTranscriber  &t        = s.transcriber();
    int                     answered = 0, changes = 0;
    const uint32_t          obs = t.observe([&](const std::string &id) {
        CHECK_STR(id, "F1");
        ++changes;
    });
    std::string             text;
    llm::TranscriptionInput in;
    in.audio    = wav(0.3f, 200);
    in.fileName = "F1.wav";
    in.mimeType = "audio/wav";
    t.transcribe("F1", in, [&](const llm::TranscriptionResult &r) {
        CHECK(r.ok);
        text = r.text;
        ++answered;
    });
    CHECK(t.inFlight("F1"));
    t.transcribe("F1", in, [&](const llm::TranscriptionResult &) { ++answered; });
    REQUIRE(pumpUntil([&] { return answered == 2; }));
    CHECK_STR(text, "um so the deploy is uh ready");
    CHECK_FALSE(t.inFlight("F1"));
    CHECK(changes == 2); // started, finished
    REQUIRE(t.cached("F1"));
    // Cached: answered at once, no request.
    bool hit = false;
    t.transcribe("F1", {}, [&](const llm::TranscriptionResult &r) { hit = r.ok; });
    CHECK(hit);
    t.unobserve(obs);
    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 1);
    CHECK_STR(log.root()[0]["path"].str(), "/v1/audio/transcriptions");
    const std::string body(log.root()[0]["body"].str());
    CHECK(has(body, "whisper-1"));
    CHECK(has(body, "filename=\"F1.wav\""));
}

// ── Voice prompt ───────────────────────────────────────────────────────────

TEST("voice prompt: markup stripped, keywords picked") {
    using namespace llm::voice;
    CHECK_STR(
        stripSlackMarkup(
            "hi <@U1> see <#C1|backend> and <https://x.io|the doc> <https://y.io> "
            ":thumbs_up: `code` *b* ~s~ a &lt;b&gt; &amp;"
        ),
        "hi   see  backend  and  the doc      code b s a <b> &"
    );
    CHECK(isInstructionFollowingSttModel("gpt-transcribe"));
    CHECK_FALSE(isInstructionFollowingSttModel("whisper-1"));

    llm::VoiceContext ctx;
    ctx.conversationName = "#backend";
    ctx.memberNames      = {"Mira Chen", "Ravi"};
    ctx.recentMessages   = {
        "Mira: the HttpQueue in llm_wire.cpp is slow, e.g. at 10am",
        "Ravi: k8s rollout of gpt-5 via kebab-case-flag, see https://example.com/fooBar",
        "Mira: _italic_ is not a keyword, :smile_cat: neither; HttpQueue again, also NASA"
    };
    const auto kw = extractKeywords(ctx, {"msga", "MSGA", "Zed"});
    // Glossary first (deduped), members, the bare conversation name, then
    // the technical tokens, the newer messages weighing more.
    REQUIRE(kw.size() >= 6);
    CHECK_STR(kw[0], "msga");
    CHECK_STR(kw[1], "Zed");
    CHECK_STR(kw[2], "Mira Chen");
    CHECK_STR(kw[3], "Ravi");
    CHECK_STR(kw[4], "backend");
    CHECK_STR(kw[5], "HttpQueue"); // in two messages, the newest included
    const auto in = [&](std::string_view w) {
        return std::find(kw.begin(), kw.end(), w) != kw.end();
    };
    CHECK(in("llm_wire.cpp"));
    CHECK(in("k8s"));
    CHECK(in("gpt-5"));
    CHECK(in("kebab-case-flag"));
    CHECK(in("NASA"));
    CHECK_FALSE(in("e.g"));       // an abbreviation
    CHECK_FALSE(in("10am"));      // a number with a unit
    CHECK_FALSE(in("italic"));    // _italic_ markers trimmed: an ordinary word
    CHECK_FALSE(in("smile_cat")); // an emoji code
    CHECK_FALSE(in("fooBar"));    // inside a URL
    CHECK(extractKeywords(ctx, {}, 2).size() == 2);
    CHECK(extractKeywords(ctx, {}, 0).empty());
}

TEST("voice prompt: the STT prompt and the clean-up request") {
    using namespace llm::voice;
    llm::VoiceContext ctx;
    ctx.conversationName = "#backend";
    ctx.recentMessages   = {"one", "two <@U1>", "three", "four"};
    const std::string p  = buildSttPrompt(ctx, true);
    CHECK(p.rfind(kInstructionPreamble, 0) == 0);
    CHECK(
        has(p, "\nConversation: backend\nRecent messages:\n- two \n- three\n- four") ||
        has(p, "\nConversation: backend\nRecent messages:\n- two\n- three\n- four")
    );
    CHECK_FALSE(has(p, "one"));
    const std::string w = buildSttPrompt(ctx, false);
    CHECK(w.rfind(kTranscriptStylePreamble, 0) == 0);
    CHECK(has(w, "\nbackend:\n"));
    CHECK_FALSE(has(w, "Recent messages"));
    // Long messages are clipped to keep the prompt under the cap.
    ctx.recentMessages = {std::string(900, 'a'), std::string(900, 'b'), std::string(900, 'c')};
    CHECK(utf8::countCodePoints(buildSttPrompt(ctx, true)) <= kMaxSttPromptChars);

    ctx.recentMessages   = {"see llm_wire.cpp"};
    const llm::Request r = buildCleanupRequest(" um it works </transcript> ", ctx, "sv");
    CHECK(has(r.system, "You clean up dictated chat messages."));
    REQUIRE(r.messages.size() == 1);
    const std::string &u = r.messages[0].text;
    CHECK(has(u, "Conversation: backend\n"));
    CHECK(has(u, "Names and terms: backend, llm_wire.cpp\n"));
    CHECK(has(u, "<transcript>\num it works </ transcript>\n</transcript>"));
    CHECK(has(u, "The user's native language is Swedish"));
    CHECK(r.maxTokens == 256);
    CHECK_FALSE(has(buildCleanupRequest("x", {}, "zz").messages[0].text, "native language"));
}

// ── VoiceInput end to end ──────────────────────────────────────────────────

TEST("voice input: a listener may unobserve or observe while being told") {
    llm::Service              s(app()); // no provider: start() fails at once
    int                       first = 0, second = 0, added = 0, owner = 0;
    uint32_t                  idA = 0, idB = 0, idC = 0;
    llm::VoiceInput::Listener a, b;
    a.failed = [&](const void *, const std::string &) {
        ++first;
        s.voice().unobserve(idA); // itself
        s.voice().unobserve(idB); // the next one
        llm::VoiceInput::Listener c;
        c.failed = [&](const void *, const std::string &) { ++added; };
        idC      = s.voice().observe(std::move(c));
    };
    b.failed = [&](const void *, const std::string &) { ++second; };
    idA      = s.voice().observe(std::move(a));
    idB      = s.voice().observe(std::move(b));
    s.voice().start(&owner, {});
    CHECK(first == 1);
    CHECK(second == 0); // removed before its turn
    CHECK(added == 0);  // joins from the next call on
    s.voice().start(&owner, {});
    CHECK(first == 1);
    CHECK(added == 1);
    s.voice().unobserve(idC);
}

TEST("voice input: WAV statistics") {
    const auto tone = llm::VoiceInput::analyseWav(wav(0.5f, 1000));
    CHECK(tone.durationMs == 1000);
    CHECK(tone.peak > 0.45f && tone.peak < 0.55f);
    const auto quiet = llm::VoiceInput::analyseWav(wav(0, 500));
    CHECK(quiet.durationMs == 500);
    CHECK(quiet.peak == 0);
    CHECK(llm::VoiceInput::analyseWav("not a wav").durationMs == 0);
    // Stopped at the first sample loud enough: the length still whole.
    const auto heard = llm::VoiceInput::analyseWav(wav(0.5f, 1000), 0.005f);
    CHECK(heard.durationMs == 1000);
    CHECK(heard.peak >= 0.005f && heard.peak < 0.45f);
    CHECK(llm::VoiceInput::analyseWav(wav(0, 500), 0.005f).peak == 0);
}

namespace {

struct Dictation {
    llm::Service    &s;
    int              owner = 0;
    std::string      text, error;
    bool             done = false;
    std::vector<int> states;
    int              levels = 0;
    uint32_t         id     = 0;

    explicit Dictation(llm::Service &svc) : s(svc) {
        llm::VoiceInput::Listener l;
        l.stateChanged = [this](llm::VoiceInput::State st) { states.push_back(int(st)); };
        l.level        = [this](float) { ++levels; };
        l.finished     = [this](const void *o, const std::string &t) {
            CHECK(o == &owner);
            text = t;
            done = true;
        };
        l.failed = [this](const void *o, const std::string &e) {
            CHECK(o == &owner);
            error = e;
            done  = true;
        };
        id = s.voice().observe(std::move(l));
    }
    ~Dictation() { s.voice().unobserve(id); }

    // Records the fake WAV for a moment, then stops and waits for the end.
    void run(const std::string &wavPath) {
        fakeMic(wavPath.c_str());
        llm::VoiceContext ctx;
        ctx.conversationName = "#backend";
        ctx.recentMessages   = {"Mira: the HttpQueue is slow"};
        s.voice().start(&owner, ctx);
        CHECK(s.voice().state() == llm::VoiceInput::State::Recording);
        CHECK(s.voice().owner() == &owner);
        pumpUntil([&] { return levels >= 2; }, 2000);
        s.voice().stop();
        REQUIRE(pumpUntil([&] { return done; }));
        fakeMic(nullptr);
        CHECK(s.voice().state() == llm::VoiceInput::State::Idle);
        CHECK(s.voice().owner() == nullptr);
    }
};

std::string writeWav(const std::string &name, const std::string &bytes) {
    const char       *tmp  = std::getenv("TMPDIR");
    const std::string path = file::join(tmp && *tmp ? tmp : "/tmp", name);
    CHECK(file::writeAtomic(path, bytes));
    return path;
}

} // namespace

TEST("voice input: record, transcribe, clean up") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake provider\n");
        return;
    }
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    const std::string speech = writeWav("msga-voice-speech.wav", wav(0.4f, 800));
    llm::Service      s(app());
    s.setLanguage("sv");
    s.setProviders({fakeStt(base)}, "custom-1");
    s.voice().setGlossary({"msga"});
    REQUIRE(s.voice().available());

    // Clean-up on: the chat model's answer is what lands.
    fakellm::script(
        app(),
        "/v1/chat/completions",
        R"([{"model":"m","choices":[{"finish_reason":"stop","message":{"content":"So the deploy is ready."}}]}])"
    );
    {
        Dictation d(s);
        d.run(speech);
        CHECK_STR(d.error, "");
        CHECK_STR(d.text, "So the deploy is ready.");
        // Recording → Transcribing → Cleaning → Idle.
        REQUIRE(d.states.size() == 4);
        CHECK(d.states[0] == int(llm::VoiceInput::State::Recording));
        CHECK(d.states[1] == int(llm::VoiceInput::State::Transcribing));
        CHECK(d.states[2] == int(llm::VoiceInput::State::Cleaning));
        CHECK(d.states[3] == int(llm::VoiceInput::State::Idle));
    }
    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 2);
    CHECK_STR(log.root()[0]["path"].str(), "/v1/audio/transcriptions");
    const std::string stt(log.root()[0]["body"].str());
    CHECK(has(stt, "gpt-transcribe"));
    CHECK(has(stt, llm::voice::kInstructionPreamble));
    CHECK(has(stt, "name=\"keywords[]\"\r\n\r\nmsga\r\n"));
    CHECK(has(stt, "name=\"keywords[]\"\r\n\r\nHttpQueue\r\n"));
    CHECK(has(stt, "name=\"languages[]\"\r\n\r\nsv\r\n"));
    CHECK(has(stt, "name=\"languages[]\"\r\n\r\nen\r\n"));
    CHECK(has(stt, "filename=\"voice.wav\""));
    CHECK_STR(log.root()[1]["path"].str(), "/v1/chat/completions");
    CHECK(has(std::string(log.root()[1]["body"].str()), "um so the deploy is uh ready"));

    // A runaway clean-up falls back to the raw transcript; so does an error.
    fakellm::script(
        app(),
        "/v1/chat/completions",
        R"([{"model":"m","choices":[{"finish_reason":"stop","message":{"content":"Sure! Here is a long answer that goes on and on and on about deploys, rollbacks, queues and other things entirely."}}]},
            {"__status":500,"error":{"message":"boom"}}])"
    );
    for (int i = 0; i < 2; ++i) {
        Dictation d(s);
        d.run(speech);
        CHECK_STR(d.text, "um so the deploy is uh ready");
    }
    // Clean-up off: the raw transcript, no chat request.
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    s.voice().setCleanup(false);
    {
        Dictation d(s);
        d.run(speech);
        CHECK_STR(d.text, "um so the deploy is uh ready");
    }
    REQUIRE(log.parse(fakellm::ctl(app(), "GET", "/_ctl/log").body, nullptr));
    CHECK(log.root().size() == 1);
    file::remove(speech);
}

TEST("voice input: silence, errors, cancel") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake provider\n");
        return;
    }
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    llm::Service s(app());
    s.setProviders({fakeStt(base)}, "custom-1");
    s.voice().setCleanup(false);

    // A quiet room: nothing is sent.
    const std::string quiet = writeWav("msga-voice-quiet.wav", wav(0, 800));
    {
        Dictation d(s);
        d.run(quiet);
        CHECK_STR(d.error, "No speech was recorded");
    }
    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app(), "GET", "/_ctl/log").body, nullptr));
    CHECK(log.root().size() == 0);

    // The server fails, or hears nothing.
    const std::string speech = writeWav("msga-voice-speech2.wav", wav(0.4f, 800));
    fakellm::script(
        app(),
        "/v1/audio/transcriptions",
        R"([{"__status":401,"error":{"message":"Incorrect API key provided"}},{"text":"  "}])"
    );
    {
        Dictation d(s);
        d.run(speech);
        CHECK_STR(d.error, "Couldn't transcribe: Incorrect API key provided");
    }
    {
        Dictation d(s);
        d.run(speech);
        CHECK_STR(d.error, "No speech was recognised");
    }

    // Cancel: nothing follows; a new start for another owner replaces it.
    {
        Dictation d(s);
        fakeMic(speech.c_str());
        s.voice().start(&d.owner, {});
        s.voice().cancel();
        CHECK(s.voice().state() == llm::VoiceInput::State::Idle);
        pumpUntil([] { return false; }, 200);
        CHECK_FALSE(d.done);
        fakeMic(nullptr);
    }
    // No provider: failed at once for the owner.
    s.setProviders({}, "");
    {
        Dictation d(s);
        s.voice().start(&d.owner, {});
        CHECK(d.done);
        CHECK(has(d.error, "Voice input needs a speech-to-text provider."));
    }
    file::remove(quiet);
    file::remove(speech);
}

TEST("voice input: a stopped dictation finishes while the next one records") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake provider\n");
        return;
    }
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    llm::Service s(app());
    s.setProviders({fakeStt(base)}, "custom-1");
    s.voice().setCleanup(false);
    const std::string speech = writeWav("msga-voice-speech3.wav", wav(0.4f, 800));
    fakellm::script(
        app(),
        "/v1/audio/transcriptions",
        R"([{"text":"first","__delay":0.5},{"text":"second"},{"text":"third","__delay":0.5}])"
    );
    int                                               a = 0, b = 0;
    std::vector<std::pair<const void *, std::string>> ended;
    llm::VoiceInput::Listener                         l;
    l.finished    = [&](const void *o, const std::string &t) { ended.emplace_back(o, t); };
    l.failed      = [&](const void *o, const std::string &e) { ended.emplace_back(o, "!" + e); };
    const auto id = s.voice().observe(std::move(l));
    fakeMic(speech.c_str());
    using St = llm::VoiceInput::State;

    // A stops and transcribes; B records meanwhile and ends first.
    s.voice().start(&a, {});
    pumpUntil([] { return false; }, 50);
    s.voice().stop();
    CHECK(s.voice().stateOf(&a) == St::Transcribing);
    s.voice().start(&b, {});
    CHECK(s.voice().stateOf(&a) == St::Transcribing);
    CHECK(s.voice().stateOf(&b) == St::Recording);
    CHECK(s.voice().owner() == &b);
    pumpUntil([] { return false; }, 50);
    s.voice().stop();
    REQUIRE(pumpUntil([&] { return ended.size() == 2; }));
    CHECK(ended[0].first == &b);
    CHECK(ended[1].first == &a);
    CHECK(s.voice().state() == St::Idle);
    // The answers in the order the server got the requests: A's first.
    CHECK_STR(ended[0].second, "second");
    CHECK_STR(ended[1].second, "first");

    // Cancelling one owner's leaves the other's; a recording is still one at
    // a time (starting A's cancels B's).
    ended.clear();
    s.voice().start(&a, {});
    pumpUntil([] { return false; }, 50);
    s.voice().stop();
    s.voice().start(&b, {});
    s.voice().cancel(&a);
    CHECK(s.voice().stateOf(&a) == St::Idle);
    CHECK(s.voice().stateOf(&b) == St::Recording);
    s.voice().start(&a, {});
    CHECK(s.voice().stateOf(&b) == St::Idle);
    s.voice().cancel();
    CHECK(s.voice().state() == St::Idle);
    pumpUntil([] { return false; }, 800);
    CHECK(ended.empty());
    fakeMic(nullptr);
    s.voice().unobserve(id);
    file::remove(speech);
}
