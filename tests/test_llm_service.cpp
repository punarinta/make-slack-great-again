// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// LlmService registry + LlmProvider HTTP round trips against FakeHttpServer.
// QSettings is redirected to a temp dir so the real user config (and its
// API keys) is never touched; SecretStore on Linux is the QSettings fallback,
// so keys land there too.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fake_http_server.h"
#include "llm/llm_service.h"
#include "llm/llm_token_store.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

static QTemporaryDir *gSettingsDir = nullptr;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    gSettingsDir = new QTemporaryDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, gSettingsDir->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, gSettingsDir->path());
    // Pre-seed a legacy layout: an Anthropic key stored under the old
    // per-vendor path, plus stale OAuth fields — the registry must pick the
    // key up unchanged and scrub the rest.
    {
        QSettings s("msga", "msga");
        s.setValue("llm/anthropic/apiKey", "sk-ant-legacy-0000-tail");
        s.setValue("llm/anthropic/accessToken", "stale");
        s.setValue("llm/anthropic/refreshToken", "stale");
        s.setValue("llm/anthropic/expiresAt", 123);
        s.sync();
    }
    const int rc = Catch::Session().run(argc, argv);
    delete gSettingsDir;
    return rc;
}

namespace {

// Spin the event loop until `done` or the timeout.
void waitFor(const bool &done, int ms = 3000) {
    QEventLoop loop;
    QTimer     t;
    t.setSingleShot(true);
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(ms);
    while (!done && t.isActive())
        loop.processEvents(QEventLoop::AllEvents, 20);
}

LlmProvider *customFor(FakeHttpServer &srv, const QString &key = {}) {
    LlmProviderConfig cfg = LlmProviderConfig::newCustom();
    cfg.name              = "Fake vLLM";
    cfg.baseUrl           = srv.baseUrl().chopped(1) + "/v1"; // "http://127.0.0.1:P/v1"
    cfg.model             = "Qwen3-32B";
    return LlmService::instance().addCustom(cfg, key);
}

QJsonObject lastBody(const FakeHttpServer &srv) {
    return QJsonDocument::fromJson(srv.requestBodies.last()).object();
}

} // namespace

TEST_CASE("Presets are registered first and the legacy key survives without migration") {
    auto &svc = LlmService::instance();
    REQUIRE(svc.providers().size() >= 2);
    CHECK(svc.providers()[0]->id() == "anthropic");
    CHECK(svc.providers()[1]->id() == "openai");

    auto *anthropic = svc.provider("anthropic");
    REQUIRE(anthropic);
    CHECK(anthropic->isPreset());
    CHECK(anthropic->isConnected());
    CHECK(anthropic->apiKey() == "sk-ant-legacy-0000-tail");
    CHECK(anthropic->accountLabel() == QString::fromUtf8("sk-an…tail"));
    CHECK(anthropic->model() == "claude-opus-5");
    CHECK(anthropic->lightModel() == "claude-haiku-4-5");

    // Stale OAuth fields were scrubbed at startup.
    QSettings s("msga", "msga");
    CHECK(!s.contains("llm/anthropic/accessToken"));
    CHECK(!s.contains("llm/anthropic/refreshToken"));
    CHECK(!s.contains("llm/anthropic/expiresAt"));

    auto *openai = svc.provider("openai");
    REQUIRE(openai);
    CHECK(!openai->isConnected());
    CHECK(openai->model() == "gpt-5.6-terra");
    CHECK(openai->lightModel() == "gpt-5.6-luna");
    CHECK(openai->endpoint("gpt-5.6-luna").reasoningEffort == "none");
    CHECK(openai->endpoint("gpt-5.6-terra").reasoningEffort.isEmpty());
    CHECK(openai->endpoint().maxCompletionTokens);
}

TEST_CASE("Custom provider: add, chat round trip, persistence, remove") {
    auto          &svc = LlmService::instance();
    FakeHttpServer srv;
    auto          *p = customFor(srv, "secret-key");
    REQUIRE(p);
    CHECK(!p->isPreset());
    CHECK(p->isConnected());
    CHECK(p->lightModel() == "Qwen3-32B"); // no cheaper tier → same model
    CHECK(svc.provider(p->id()) == p);

    // Persisted layout.
    {
        QSettings s("msga", "msga");
        CHECK(s.value("llm/customProviders").toStringList().contains(p->id()));
        CHECK(s.value(QString("llm/providers/%1/baseUrl").arg(p->id())).toString() == p->baseUrl());
        CHECK(s.value(QString("llm/providers/%1/model").arg(p->id())).toString() == "Qwen3-32B");
        CHECK(LlmTokenStore::loadApiKey(p->id()) == "secret-key");
    }

    // Route a chat through the service with the custom provider as default.
    svc.setDefaultProviderId(p->id());
    CHECK(svc.activeProvider() == p);

    srv.enqueue(R"({"model":"Qwen3-32B","choices":[{"finish_reason":"stop",
                   "message":{"role":"assistant","content":"Summary here"}}]})");
    Llm::Request req;
    req.system    = "sys";
    req.messages  = {{Llm::Message::Role::User, "hello"}};
    req.maxTokens = 64;

    bool          done = false;
    Llm::Response got;
    QString       err;
    svc.chat(
        req,
        [&](Llm::Response r) {
            got  = std::move(r);
            done = true;
        },
        [&](QString e) {
            err  = std::move(e);
            done = true;
        }
    );
    waitFor(done);
    REQUIRE(done);
    CHECK(err.isEmpty());
    CHECK(got.text == "Summary here");

    REQUIRE(srv.requestPaths.size() == 1);
    CHECK(srv.requestPaths[0] == "/v1/chat/completions");
    CHECK(srv.requestHeaders[0].contains("Authorization: Bearer secret-key"));
    const auto body = lastBody(srv);
    CHECK(body["model"].toString() == "Qwen3-32B"); // provider default filled in
    CHECK(body["max_tokens"].toInt() == 64);
    CHECK(!body.contains("max_completion_tokens"));
    CHECK(!body.contains("reasoning_effort"));

    // Remove: forgets config, key and default.
    const QString id = p->id();
    svc.removeCustom(id);
    CHECK(svc.provider(id) == nullptr);
    CHECK(svc.defaultProviderId().isEmpty());
    QSettings s("msga", "msga");
    CHECK(!s.value("llm/customProviders").toStringList().contains(id));
    CHECK(!s.contains(QString("llm/providers/%1/baseUrl").arg(id)));
    CHECK(LlmTokenStore::loadApiKey(id).isEmpty());
}

TEST_CASE("Custom provider without a key sends no Authorization header") {
    auto          &svc = LlmService::instance();
    FakeHttpServer srv;
    auto          *p = customFor(srv);
    REQUIRE(p->isConnected()); // key is optional for a custom endpoint

    srv.enqueue(R"({"choices":[{"finish_reason":"stop","message":{"content":"ok"}}]})");
    bool done = false;
    p->chat(
        {{}, {{Llm::Message::Role::User, "x"}}},
        [&](Llm::Response) { done = true; },
        [&](QString) { done = true; }
    );
    waitFor(done);
    REQUIRE(done);
    CHECK(!srv.requestHeaders[0].toLower().contains("authorization:"));
    svc.removeCustom(p->id());
}

TEST_CASE("Server errors reach onError as the server's message") {
    auto          &svc = LlmService::instance();
    FakeHttpServer srv;
    auto          *p = customFor(srv, "k");

    srv.enqueueStatus(
        401,
        "Unauthorized",
        "application/json",
        R"({"error":{"message":"Invalid API key","type":"invalid_request_error"}})"
    );
    bool    done = false;
    QString err;
    p->chat(
        {{}, {{Llm::Message::Role::User, "x"}}},
        [&](Llm::Response) { done = true; },
        [&](QString e) {
            err  = std::move(e);
            done = true;
        }
    );
    waitFor(done);
    REQUIRE(done);
    CHECK(err == "Invalid API key");

    // Flat vLLM shape on a 400.
    srv.enqueueStatus(
        400,
        "Bad Request",
        "application/json",
        R"({"object":"error","message":"maximum context length exceeded","type":"BadRequestError"})"
    );
    done = false;
    p->chat(
        {{}, {{Llm::Message::Role::User, "x"}}},
        [&](Llm::Response) { done = true; },
        [&](QString e) {
            err  = std::move(e);
            done = true;
        }
    );
    waitFor(done);
    REQUIRE(done);
    CHECK(err == "maximum context length exceeded");
    svc.removeCustom(p->id());
}

TEST_CASE("Connection refused surfaces a readable error, not a hang") {
    auto             &svc = LlmService::instance();
    LlmProviderConfig cfg = LlmProviderConfig::newCustom();
    cfg.name              = "Dead";
    cfg.baseUrl           = "http://127.0.0.1:1/v1"; // nothing listens on port 1
    cfg.model             = "m";
    auto *p               = svc.addCustom(cfg, {});

    bool    done = false;
    QString err;
    p->chat(
        {{}, {{Llm::Message::Role::User, "x"}}},
        [&](Llm::Response) { done = true; },
        [&](QString e) {
            err  = std::move(e);
            done = true;
        }
    );
    waitFor(done, 5000);
    REQUIRE(done);
    CHECK(!err.isEmpty());
    svc.removeCustom(p->id());
}

TEST_CASE("listModels and probe hit GET /models") {
    auto          &svc = LlmService::instance();
    FakeHttpServer srv;
    auto          *p = customFor(srv, "k");

    srv.enqueue(R"({"object":"list","data":[{"id":"Qwen3-32B"},{"id":"gemma-3"}]})");
    bool        done = false;
    QStringList models;
    p->listModels(
        [&](QStringList m) {
            models = std::move(m);
            done   = true;
        },
        [&](QString) { done = true; }
    );
    waitFor(done);
    REQUIRE(done);
    CHECK(models == QStringList{"Qwen3-32B", "gemma-3"});
    CHECK(srv.requestPaths.last() == "/v1/models");
    CHECK(srv.requestBodies.last().isEmpty()); // GET

    // probe(): the not-yet-saved editor flow, with an explicit key.
    srv.enqueue(R"({"data":[{"id":"only-one"}]})");
    LlmProviderConfig cfg = p->config();
    done                  = false;
    models.clear();
    QObject ctx;
    LlmProvider::probe(
        cfg,
        "probe-key",
        [&](QStringList m) {
            models = std::move(m);
            done   = true;
        },
        [&](QString) { done = true; },
        &ctx
    );
    waitFor(done);
    REQUIRE(done);
    CHECK(models == QStringList{"only-one"});
    CHECK(srv.requestHeaders.last().contains("Authorization: Bearer probe-key"));
    svc.removeCustom(p->id());
}

TEST_CASE("updateProvider: custom edits persist; preset takes only the model") {
    auto          &svc = LlmService::instance();
    FakeHttpServer srv;
    auto          *p = customFor(srv, "k1");

    LlmProviderConfig edited = p->config();
    edited.name              = "Renamed";
    edited.model             = "other-model";
    svc.updateProvider(p->id(), edited, {}); // empty key keeps the stored one
    CHECK(p->displayName() == "Renamed");
    CHECK(p->model() == "other-model");
    CHECK(p->apiKey() == "k1");
    svc.updateProvider(p->id(), edited, "k2");
    CHECK(p->apiKey() == "k2");
    {
        QSettings s("msga", "msga");
        CHECK(s.value(QString("llm/providers/%1/name").arg(p->id())).toString() == "Renamed");
    }
    svc.removeCustom(p->id());

    auto             *openai = svc.provider("openai");
    LlmProviderConfig ocfg   = openai->config();
    ocfg.name                = "Hacked"; // ignored on presets
    ocfg.baseUrl             = "http://evil/v1";
    ocfg.model               = "gpt-5.6-sol";
    svc.updateProvider("openai", ocfg, "sk-openai");
    CHECK(openai->displayName() == "OpenAI");
    CHECK(openai->baseUrl() == "https://api.openai.com/v1");
    CHECK(openai->model() == "gpt-5.6-sol");
    CHECK(openai->isConnected());
    CHECK(
        QSettings("msga", "msga").value("llm/providers/openai/model").toString() == "gpt-5.6-sol"
    );

    // Back to the default → the override is dropped from settings.
    ocfg.model = "";
    svc.updateProvider("openai", ocfg, {});
    CHECK(openai->model() == "gpt-5.6-terra");
    CHECK(!QSettings("msga", "msga").contains("llm/providers/openai/model"));

    svc.disconnectProvider("openai");
    CHECK(!openai->isConnected());
    CHECK(LlmTokenStore::loadApiKey("openai").isEmpty());
}

TEST_CASE("activeProvider falls back to any connected provider") {
    auto &svc = LlmService::instance();
    svc.setDefaultProviderId("openai"); // not connected
    CHECK(svc.activeProvider() == svc.provider("anthropic"));

    svc.disconnectProvider("anthropic");
    CHECK(svc.activeProvider() == nullptr);
    CHECK(!svc.isAvailable());

    bool    called = false;
    QString err;
    svc.chat(
        {},
        [&](Llm::Response) { called = true; },
        [&](QString e) {
            err    = e;
            called = true;
        }
    );
    CHECK(called);
    CHECK(err.contains("No AI provider"));
}
