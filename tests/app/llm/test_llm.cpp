// msga_app_llm: the wires against captured payloads, the summary prompt, and
// the Service against a local fake provider (tests/support/fake_llm.py) — never a
// real one.
#include "app/llm/discussion_summary.h"
#include "app/llm/provider.h"
#include "app/llm/service.h"
#include "support/fake_llm_server.h"
#include "app/llm/wire.h"
#include "base/json.h"
#include "support/test.h"
#include "plat/plat.h"

#include <chrono>
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

llm::Request oneQuestion() {
    llm::Request req;
    req.system    = "Be brief.";
    req.model     = "m1";
    req.maxTokens = 100;
    req.messages.push_back({llm::Message::Role::User, "Hi \"there\""});
    return req;
}

} // namespace

// ── Wire: requests ─────────────────────────────────────────────────────────

TEST("wire: Anthropic chat request") {
    llm::Endpoint ep;
    ep.format            = llm::Format::AnthropicMessages;
    ep.baseUrl           = "https://api.anthropic.com/";
    ep.apiKey            = "sk-ant";
    llm::Request req     = oneQuestion();
    req.temperature      = 1.7; // clamped to Anthropic's 0…1
    const net::Request r = llm::buildChat(ep, req);
    CHECK_STR(r.method, "POST");
    CHECK_STR(r.url, "https://api.anthropic.com/v1/messages");
    CHECK_STR(header(r, "x-api-key"), "sk-ant");
    CHECK_STR(header(r, "anthropic-version"), "2023-06-01");
    CHECK_STR(header(r, "Content-Type"), "application/json");
    CHECK_STR(header(r, "Authorization"), "<none>");
    json::Document d;
    REQUIRE(d.parse(std::string_view(r.body), nullptr));
    CHECK_STR(d.root()["model"].str(), "m1");
    CHECK_STR(d.root()["system"].str(), "Be brief."); // top level, not a message
    CHECK(d.root()["messages"].size() == 1);
    CHECK_STR(d.root()["messages"][0]["role"].str(), "user");
    CHECK_STR(d.root()["messages"][0]["content"].str(), "Hi \"there\"");
    CHECK(d.root()["max_tokens"].integer() == 100);
    CHECK(d.root()["temperature"].number() == 1.0);
}

TEST("wire: OpenAI-compatible chat request") {
    llm::Endpoint ep;
    ep.baseUrl       = "http://localhost:8000/v1";
    llm::Request req = oneQuestion();
    req.temperature  = 0.2;
    net::Request   r = llm::buildChat(ep, req);
    json::Document d;
    CHECK_STR(r.url, "http://localhost:8000/v1/chat/completions");
    CHECK_STR(header(r, "Authorization"), "<none>"); // a keyless local server
    REQUIRE(d.parse(std::string_view(r.body), nullptr));
    CHECK(d.root()["messages"].size() == 2); // the system prompt is a message
    CHECK_STR(d.root()["messages"][0]["role"].str(), "system");
    CHECK(d.root()["max_tokens"].integer() == 100);
    CHECK_FALSE(d.root().has("max_completion_tokens"));
    CHECK_FALSE(d.root().has("reasoning_effort"));
    CHECK(d.root()["temperature"].number() == 0.2);

    // The OpenAI preset: max_completion_tokens, a reasoning effort, and with
    // reasoning on no temperature.
    ep.apiKey              = "sk-oa";
    ep.maxCompletionTokens = true;
    ep.reasoningEffort     = "medium";
    r                      = llm::buildChat(ep, req);
    REQUIRE(d.parse(std::string_view(r.body), nullptr));
    CHECK_STR(header(r, "Authorization"), "Bearer sk-oa");
    CHECK(d.root()["max_completion_tokens"].integer() == 100);
    CHECK_FALSE(d.root().has("max_tokens"));
    CHECK_STR(d.root()["reasoning_effort"].str(), "medium");
    CHECK_FALSE(d.root().has("temperature"));
    ep.reasoningEffort = "none"; // reasoning off: the temperature is kept
    r                  = llm::buildChat(ep, req);
    REQUIRE(d.parse(std::string_view(r.body), nullptr));
    CHECK_STR(d.root()["reasoning_effort"].str(), "none");
    CHECK(d.root()["temperature"].number() == 0.2);
}

TEST("wire: list models request") {
    llm::Endpoint ep;
    ep.baseUrl = "https://api.openai.com/v1";
    ep.apiKey  = "k";
    CHECK_STR(llm::buildListModels(ep).url, "https://api.openai.com/v1/models");
    CHECK_STR(llm::buildListModels(ep).method, "GET");
    ep.format  = llm::Format::AnthropicMessages;
    ep.baseUrl = "https://api.anthropic.com";
    CHECK_STR(llm::buildListModels(ep).url, "https://api.anthropic.com/v1/models");
    CHECK_STR(header(llm::buildListModels(ep), "x-api-key"), "k");
}

// ── Wire: answers ──────────────────────────────────────────────────────────

TEST("wire: parse chat answers") {
    using llm::Format;
    llm::ChatResult r = llm::parseChat(
        Format::AnthropicMessages,
        answer(
            200,
            R"({"model":"claude-x","stop_reason":"end_turn","content":[{"type":"text","text":"A"},{"type":"tool_use"},{"type":"text","text":"B"}]})"
        )
    );
    CHECK(r.ok);
    CHECK_STR(r.response.text, "AB");
    CHECK_STR(r.response.model, "claude-x");
    CHECK_STR(r.response.stopReason, "end_turn");

    r = llm::parseChat(
        Format::AnthropicMessages, answer(200, R"({"stop_reason":"refusal","content":[]})")
    );
    CHECK_FALSE(r.ok);
    CHECK_STR(r.error, "refusal");

    r = llm::parseChat(
        Format::OpenAiChat,
        answer(
            200, R"({"model":"g","choices":[{"finish_reason":"stop","message":{"content":"Hi"}}]})"
        )
    );
    CHECK(r.ok);
    CHECK_STR(r.response.text, "Hi");
    CHECK_STR(r.response.stopReason, "stop");
    // Content as an array of text parts (some compat servers).
    r = llm::parseChat(
        Format::OpenAiChat,
        answer(
            200,
            R"({"choices":[{"message":{"content":[{"type":"text","text":"x"},{"text":"y"}]}}]})"
        )
    );
    CHECK_STR(r.response.text, "xy");
    r = llm::parseChat(Format::OpenAiChat, answer(200, R"({"choices":[]})"));
    CHECK_FALSE(r.ok);
    CHECK_STR(r.error, "Unexpected response from server (no choices)");
}

TEST("wire: parse errors") {
    using llm::Format;
    auto err = [](int status, std::string body) {
        return llm::parseChat(Format::OpenAiChat, answer(status, std::move(body))).error;
    };
    CHECK_STR(err(401, R"({"error":{"message":"Invalid key"}})"), "Invalid key");
    CHECK_STR(err(400, R"({"error":{"code":1}})"), "unknown error");
    CHECK_STR(err(502, R"({"error":"gateway down"})"), "gateway down");
    CHECK_STR(err(200, R"({"object":"error","message":"model not loaded"})"), "model not loaded");
    CHECK_STR(err(500, R"({"message":"boom"})"), "boom");
    CHECK_STR(err(503, "Service   Unavailable\n"), "HTTP 503: Service Unavailable");
    CHECK_STR(err(500, ""), "HTTP 500");
    CHECK_STR(
        err(404, "nope"),
        "No chat endpoint at this URL (HTTP 404) \xE2\x80\x94 most servers expect it to end in /v1"
    );
    CHECK_STR(err(200, "<html>"), "Unexpected response from server (not JSON)");
    // A long HTML error page is cut to 200 characters.
    const std::string e = err(500, std::string(300, 'x'));
    CHECK_STR(e, "HTTP 500: " + std::string(200, 'x') + "\xE2\x80\xA6");
    // No HTTP answer: the transport's reason, as a sentence.
    net::Response r;
    r.error = "connect: refused";
    CHECK_STR(llm::parseChat(Format::OpenAiChat, r).error, "Couldn't connect to the server");
    r.error = "timeout: 120 s";
    CHECK_STR(llm::parseModels(r).error, "The server didn't answer in time");
    r.error = "dns";
    CHECK_STR(llm::parseChat(Format::OpenAiChat, r).error, "Server not found");
    r.error.clear();
    CHECK_STR(llm::parseChat(Format::OpenAiChat, r).error, "network error");
}

TEST("wire: custom server URL normalized") {
    using llm::normalizeOpenAiBaseUrl;
    CHECK_STR(normalizeOpenAiBaseUrl("  http://localhost:11434  "), "http://localhost:11434/v1");
    CHECK_STR(normalizeOpenAiBaseUrl("localhost:8000/v1"), "http://localhost:8000/v1");
    CHECK_STR(normalizeOpenAiBaseUrl("https://x.example/v1/"), "https://x.example/v1");
    CHECK_STR(
        normalizeOpenAiBaseUrl("https://x.example/api/v1/chat/completions/"),
        "https://x.example/api/v1"
    );
    CHECK_STR(normalizeOpenAiBaseUrl("https://x.example//"), "https://x.example/v1");
    CHECK_STR(normalizeOpenAiBaseUrl(""), "");
    CHECK_STR(normalizeOpenAiBaseUrl("   "), "");
    CHECK_STR(normalizeOpenAiBaseUrl("ftp://x.example/v1"), "");
    CHECK_STR(normalizeOpenAiBaseUrl("http:///v1"), "");
}

TEST("wire: cleartext only to remote hosts") {
    using llm::isCleartextRemote;
    CHECK(isCleartextRemote("http://api.example.com/v1"));
    CHECK(isCleartextRemote("HTTP://8.8.8.8/v1"));
    CHECK(isCleartextRemote("http://[2001:db8::1]:8000/v1"));
    CHECK(isCleartextRemote("http://172.32.0.1/v1"));
    CHECK(!isCleartextRemote("https://api.example.com/v1"));
    for (const char *local : {
             "http://localhost:11434/v1",
             "http://LOCALHOST/v1",
             "http://llm.localhost/v1",
             "http://box.local:8000/v1",
             "http://127.0.0.5/v1",
             "http://169.254.1.2/v1",
             "http://10.1.2.3/v1",
             "http://172.16.0.1/v1",
             "http://172.31.255.1/v1",
             "http://192.168.1.10:8000/v1",
             "http://[::1]:8000/v1",
             "http://[fe80::1]/v1",
             "http://[fd12:3456::7]/v1",
             "http://[::ffff:192.168.0.2]/v1",
         })
        CHECK(!isCleartextRemote(local));
    // A host typed without a scheme is checked once normalized.
    CHECK(isCleartextRemote(llm::normalizeOpenAiBaseUrl("api.example.com:8000")));
}

TEST("wire: parse models") {
    llm::ModelsResult m =
        llm::parseModels(answer(200, R"({"data":[{"id":"a"},{"id":""},{"id":"b"}]})"));
    CHECK(m.ok);
    REQUIRE(m.models.size() == 2);
    CHECK_STR(m.models[0], "a");
    CHECK_STR(m.models[1], "b");
    m = llm::parseModels(answer(401, R"({"error":{"message":"bad key"}})"));
    CHECK_FALSE(m.ok);
    CHECK_STR(m.error, "bad key");
}

// ── Providers ──────────────────────────────────────────────────────────────

TEST("provider: presets and custom servers from the settings") {
    llm::Provider a = llm::fromSettings("anthropic", "Anthropic", "", "", "", "");
    CHECK(a.isPreset);
    CHECK_FALSE(a.connected()); // a preset needs a key
    CHECK_STR(a.model, "claude-opus-5");
    CHECK_STR(a.summaryModel(), "claude-haiku-4-5");
    CHECK_STR(a.baseUrl, "https://api.anthropic.com");
    CHECK(a.format == llm::Format::AnthropicMessages);
    a.apiKey = "k";
    CHECK(a.connected());

    llm::Provider o = llm::fromSettings("openai", "OpenAI", "", "k", "gpt-6-astra", "");
    CHECK_STR(o.model, "gpt-6-astra");
    CHECK_STR(o.sttModel, "gpt-transcribe");
    CHECK(o.endpoint(o.summaryModel()).reasoningEffort == "none"); // the light tier
    CHECK(o.endpoint(o.model).reasoningEffort.empty());
    CHECK(o.endpoint().maxCompletionTokens);

    llm::Provider c =
        llm::fromSettings("custom-1", "vLLM", "http://localhost:8000/v1", "", "qwen", "");
    CHECK_FALSE(c.isPreset);
    CHECK(c.connected()); // the key is optional
    CHECK_STR(c.summaryModel(), "qwen");
    CHECK_STR(c.sttModel, "whisper-1");
    CHECK(llm::preset("custom-1") == nullptr);
    REQUIRE(llm::preset("openai") != nullptr);
    CHECK_STR(llm::preset("openai")->keyUrl, "https://platform.openai.com/api-keys");
}

TEST("service: the active provider") {
    llm::Service s(app());
    CHECK_FALSE(s.available());
    std::vector<llm::Provider> list;
    list.push_back(llm::fromSettings("anthropic", "Anthropic", "", "", "", ""));
    list.push_back(llm::fromSettings("openai", "OpenAI", "", "k", "", ""));
    list.push_back(llm::fromSettings("custom-1", "Box", "http://box/v1", "", "m", ""));
    s.setProviders(list, "anthropic"); // the default is not connected: the first that is
    REQUIRE(s.active());
    CHECK_STR(s.active()->id, "openai");
    s.setProviders(list, "custom-1");
    CHECK_STR(s.active()->id, "custom-1");
    list.pop_back();
    list[1].apiKey.clear();
    s.setProviders(list, "custom-1");
    CHECK(s.active() == nullptr);
}

TEST("service: no provider fails later, not inside the call") {
    llm::Service    s(app());
    bool            called = false, inside = true;
    llm::ChatResult got;
    s.chat(oneQuestion(), [&](llm::ChatResult r) {
        called = true;
        CHECK_FALSE(inside);
        got = std::move(r);
    });
    inside = false;
    CHECK_FALSE(called);
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_FALSE(got.ok);
    CHECK_STR(
        got.error,
        "No AI provider connected \xE2\x80\x94 connect one in Settings \xE2\x86\x92 AI assistance"
    );
    // A destroyed service never calls back.
    bool late = false;
    {
        llm::Service gone(app());
        gone.chat(oneQuestion(), [&](llm::ChatResult) { late = true; });
    }
    pumpUntil([] { return false; }, 100);
    CHECK_FALSE(late);
}

TEST("service: the stand-in answers in-process, only while nothing is connected") {
    llm::Service s(app());
    std::string  asked;
    s.setStandIn(
        llm::fromSettings("local", "Local AI", "local:", "", "local-1", ""),
        [&](std::string_view req) {
            asked = std::string(req);
            return std::string("canned");
        }
    );
    REQUIRE(s.active());
    CHECK_STR(s.active()->name, "Local AI");
    llm::ChatResult got;
    bool            done = false;
    s.chat(oneQuestion(), [&](llm::ChatResult r) {
        got  = std::move(r);
        done = true;
    });
    CHECK_FALSE(done);
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(got.ok);
    CHECK_STR(got.response.text, "canned");
    CHECK(asked.find("Hi \"there\"") != std::string::npos);
    s.setProviders({llm::fromSettings("openai", "OpenAI", "", "k", "", "")}, "");
    CHECK_STR(s.active()->id, "openai");
}

// ── Against the fake provider ──────────────────────────────────────────────

TEST("service: chat over both wires") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake provider\n");
        return;
    }
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    llm::Service  s(app());
    // The Anthropic wire, as the preset speaks it, pointed at the fake.
    llm::Provider a = llm::fromSettings("anthropic", "Anthropic", "", "sk-test", "", "");
    a.baseUrl       = base;
    llm::Provider c = llm::fromSettings("custom-1", "Fake", base + "/v1", "", "fake-large", "");

    llm::ChatResult got;
    bool            done = false;
    s.chat(a, oneQuestion(), [&](llm::ChatResult r) {
        got  = std::move(r);
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(got.ok);
    CHECK_STR(got.response.text, "The team agreed to **ship on Friday**.");
    CHECK_STR(got.response.model, "fake-anthropic");

    done             = false;
    llm::Request req = oneQuestion();
    req.model.clear(); // → the provider's model
    s.chat(c, req, [&](llm::ChatResult r) {
        got  = std::move(r);
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(got.ok);
    CHECK_STR(got.response.model, "fake-openai");

    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 2);
    CHECK_STR(log.root()[0]["path"].str(), "/v1/messages");
    CHECK_STR(log.root()[0]["headers"]["x-api-key"].str(), "sk-test");
    CHECK_STR(log.root()[1]["path"].str(), "/v1/chat/completions");
    CHECK_FALSE(log.root()[1]["headers"].has("authorization"));
    json::Document body;
    REQUIRE(body.parse(std::string(log.root()[1]["body"].str()), nullptr));
    CHECK_STR(body.root()["model"].str(), "fake-large");
}

TEST("service: errors and the model list from the server") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake provider\n");
        return;
    }
    fakellm::ctl(app(), "POST", "/_ctl/reset");
    fakellm::script(
        app(),
        "/v1/chat/completions",
        R"([{"__status":401,"error":{"message":"Incorrect API key provided"}}])"
    );
    llm::Service  s(app());
    llm::Provider c = llm::fromSettings("custom-1", "Fake", base + "/v1", "bad", "m", "");
    s.setProviders({c}, "custom-1");
    llm::ChatResult got;
    bool            done = false;
    s.chat(oneQuestion(), [&](llm::ChatResult r) {
        got  = std::move(r);
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(got.ok);
    CHECK_STR(got.error, "Incorrect API key provided");

    llm::ModelsResult models;
    done = false;
    s.listModels(c, [&](llm::ModelsResult m) {
        models = std::move(m);
        done   = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(models.ok);
    REQUIRE(models.models.size() == 2);
    CHECK_STR(models.models[1], "fake-large");

    // Nothing listens on the old port of a stopped server: a transport error.
    llm::Provider dead =
        llm::fromSettings("custom-2", "Dead", "http://127.0.0.1:9/v1", "", "m", "");
    done = false;
    s.chat(dead, oneQuestion(), [&](llm::ChatResult r) {
        got  = std::move(r);
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK_FALSE(got.ok);
    CHECK_FALSE(got.error.empty());
}

// ── The summary prompt ─────────────────────────────────────────────────────

TEST("summary: transcript and language") {
    std::vector<llm::SummaryEntry> e;
    e.push_back({"Mira", "Ship it\n  on   Friday?", false});
    e.push_back({"Jonas", "Yes", true});
    e.push_back({{}, "[replies of 2 more threads not included]", false});
    const llm::Request req = llm::summaryRequest(e, "ja");
    CHECK(req.maxTokens == 512);
    CHECK(req.system.find("Write in Japanese only") != std::string::npos);
    REQUIRE(req.messages.size() == 1);
    const std::string &u = req.messages[0].text;
    CHECK(
        u.find("Mira: Ship it on Friday?\n    \xE2\x86\xB3 Jonas: Yes\n[replies of 2 more") !=
        std::string::npos
    );
    // The language comes again after the transcript (light models drift).
    CHECK(u.find("Write the summary in Japanese, even if") != std::string::npos);
    CHECK(u.find("Write the summary in Japanese") > u.find("Jonas: Yes"));

    CHECK_STR(llm::languageName("sv"), "Swedish");
    CHECK_STR(llm::languageName("pt-BR"), "Portuguese");
    CHECK_STR(llm::languageName("xx"), "English");
    CHECK_STR(llm::languageName(""), "English");
}

TEST("summary: long messages and long spans are bounded") {
    // One 1000-character message is cut to 600 (599 + "…").
    std::vector<llm::SummaryEntry> one{{"A", std::string(1000, 'z'), false}};
    std::string                    u = llm::summaryRequest(one, "en").messages[0].text;
    CHECK(u.find("A: " + std::string(599, 'z') + "\xE2\x80\xA6\n") != std::string::npos);
    CHECK(u.find(std::string(600, 'z')) == std::string::npos);
    // Exactly 600 stays whole.
    one[0].text = std::string(600, 'y');
    u           = llm::summaryRequest(one, "en").messages[0].text;
    CHECK(u.find("A: " + std::string(600, 'y') + "\n") != std::string::npos);

    // 100 messages of ~500 characters: the middle is dropped, head and tail kept.
    std::vector<llm::SummaryEntry> many;
    for (int i = 0; i < 100; ++i)
        many.push_back({"U" + std::to_string(i), std::string(500, 'm'), false});
    u = llm::summaryRequest(many, "en").messages[0].text;
    CHECK(u.find("U0: ") != std::string::npos);
    CHECK(u.find("U99: ") != std::string::npos);
    CHECK(u.find("U50: ") == std::string::npos);
    const size_t omitted = u.find(" messages omitted \xE2\x80\xA6]");
    REQUIRE(omitted != std::string::npos);
    CHECK(u.find("[\xE2\x80\xA6 ") < omitted);
    CHECK(u.size() < 26000);
}
