// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "auth/token_store.h"
#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"
#include "backend/slack/web_api_client.h"
#include "rpl/producer.h"

using namespace slack;

// Decode the Slack credentials stored for a team id (the test workspaces are
// all Slack), so assertions can read xoxp/refreshToken/expiresAt back out of the
// opaque WorkspaceRecord::auth blob.
static slack::Credentials loadCreds(const QString &id) {
    const auto rec = TokenStore::loadWorkspace(WorkspaceKey{Service::Slack, id});
    return rec ? slack::fromRecord(*rec) : slack::Credentials{};
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static void clearSettings() {
    QSettings s("msga", "msga");
    s.clear();
    s.sync();
}

// Pumps the Qt event loop until pred() returns true or timeoutMs elapses.
static bool waitFor(std::function<bool()> pred, int timeoutMs = 2000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

#include "fake_http_server.h"

// ── TestablePublicBackend ─────────────────────────────────────────────────────
// Overrides doRefresh so tests control success/failure without real HTTP.
// Updates _tokenExpiresAt to a far future value on success so that the
// periodic proactive check stays outside the refresh window, preventing
// an immediate re-fire that would create an infinite processEvents loop.

class TestablePublicBackend : public PublicBackend {
public:
    int           doRefreshCallCount = 0;
    RefreshResult doRefreshResult    = RefreshResult::Success;

    using PublicBackend::PublicBackend;
    using PublicBackend::RefreshResult;

protected:
    void doRefresh(std::function<void(RefreshResult)> done) override {
        ++doRefreshCallCount;
        if (doRefreshResult == RefreshResult::Success)
            _tokenExpiresAt = QDateTime::currentSecsSinceEpoch() + 43200;
        done(doRefreshResult);
    }
};

// ── Per-test credentials fixture ──────────────────────────────────────────────

static const slack::AppConfig kTestApp{"test-client-id", "test-client-secret", ""};

struct RefreshFixture {
    RefreshFixture() { clearSettings(); }
    ~RefreshFixture() { clearSettings(); }
};

// =============================================================================
// WebApiClient — reactive token_expired handling
// =============================================================================

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: token_expired triggers OnTokenExpired handler",
    "[token_refresh][webclient]"
) {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"token_expired"})");
    server.enqueue(R"({"ok":true})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("old-token");

    bool handlerCalled = false;
    bool successCalled = false;

    client.setOnTokenExpired([&](std::function<void(bool)> done) {
        handlerCalled = true;
        client.setToken("new-token");
        done(true);
    });

    client.call("some.method", QUrlQuery{}, [&](QJsonObject) { successCalled = true; }, {});

    REQUIRE(waitFor([&] { return successCalled; }));
    CHECK(handlerCalled);
    CHECK(server.requestCount == 2); // initial + retry
}

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: done(true) retries the call and fires success callback",
    "[token_refresh][webclient]"
) {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"token_expired"})");
    server.enqueue(R"({"ok":true,"value":"hello"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("old-token");

    QString resultValue;
    client.setOnTokenExpired([&](std::function<void(bool)> done) {
        client.setToken("refreshed-token");
        done(true);
    });
    client.call(
        "api.method",
        QUrlQuery{},
        [&](QJsonObject r) { resultValue = r.value("value").toString(); },
        {}
    );

    REQUIRE(waitFor([&] { return !resultValue.isEmpty(); }));
    CHECK(resultValue == "hello");
}

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: done(false) drains all pending calls with errors",
    "[token_refresh][webclient]"
) {
    FakeHttpServer server;
    // Only the first call reaches the server; the second is drained without HTTP.
    server.enqueue(R"({"ok":false,"error":"token_expired"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("old-token");

    int errorCount   = 0;
    int successCount = 0;
    client.setOnTokenExpired([&](std::function<void(bool)> done) { done(false); });

    client.call(
        "method.one", {}, [&](QJsonObject) { successCount++; }, [&](QString) { errorCount++; }
    );
    client.call(
        "method.two", {}, [&](QJsonObject) { successCount++; }, [&](QString) { errorCount++; }
    );

    REQUIRE(waitFor([&] { return errorCount == 2; }));
    CHECK(successCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: failAllPending errors queued calls and leaves the in-flight one",
    "[token_refresh][webclient]"
) {
    // Regression: a proactive refresh that ends in AuthError has only a no-op
    // waiter, so it never drains the queue via the token_expired path. Queued
    // calls — and the self-referential paginate Ctx they hold — would leak
    // unless the backend fails them explicitly. failAllPending is that drain.
    FakeHttpServer server;
    server.enqueue(R"({"ok":true})"); // response for the in-flight call

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("token");

    int ok1 = 0, err1 = 0, ok2 = 0, err2 = 0;
    // Single in-flight slot: method.one starts executing, method.two queues
    // behind it. No event-loop pump yet, so method.one's reply hasn't arrived.
    client.call("method.one", {}, [&](QJsonObject) { ok1++; }, [&](QString) { err1++; });
    client.call("method.two", {}, [&](QJsonObject) { ok2++; }, [&](QString) { err2++; });

    client.failAllPending("token_expired");

    // The queued call errors synchronously; the in-flight call is untouched.
    CHECK(err2 == 1);
    CHECK(ok2 == 0);
    CHECK(err1 == 0);

    // The in-flight call still completes normally afterwards.
    REQUIRE(waitFor([&] { return ok1 == 1; }));
    CHECK(err1 == 0);
    CHECK(err2 == 1); // not drained twice
}

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: token_expired without handler is treated as a regular error",
    "[token_refresh][webclient]"
) {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"token_expired"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    // No setOnTokenExpired — handler is null

    bool    errorCalled = false;
    QString errorMsg;
    client.call("method", {}, {}, [&](QString err) {
        errorCalled = true;
        errorMsg    = err;
    });

    REQUIRE(waitFor([&] { return errorCalled; }));
    CHECK(errorMsg == "token_expired");
}

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: stale-connection failure is retried once on a fresh connection",
    "[token_refresh][webclient]"
) {
    FakeHttpServer server;
    server.dropConnections = 1; // first request: connection killed without a response
    server.enqueue(R"({"ok":true,"value":"hello"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("token");

    QString resultValue;
    int     errorCount = 0;
    client.call(
        "api.method",
        QUrlQuery{},
        [&](QJsonObject r) { resultValue = r.value("value").toString(); },
        [&](QString) { errorCount++; }
    );

    REQUIRE(waitFor([&] { return !resultValue.isEmpty(); }));
    CHECK(resultValue == "hello");
    CHECK(errorCount == 0);
    CHECK(server.requestCount == 2); // initial (dropped) + retry
}

TEST_CASE_METHOD(
    RefreshFixture,
    "WebApiClient: persistent connection failure surfaces an error after one retry",
    "[token_refresh][webclient]"
) {
    FakeHttpServer server;
    server.dropConnections = 2; // both the initial attempt and the retry die

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("token");

    int successCount = 0;
    int errorCount   = 0;
    client.call(
        "api.method",
        QUrlQuery{},
        [&](QJsonObject) { successCount++; },
        [&](QString) { errorCount++; }
    );

    REQUIRE(waitFor([&] { return errorCount == 1; }));
    CHECK(successCount == 0);
    // Our single retry plus possibly one QNAM-internal reconnect attempt —
    // bounded either way, no endless retry loop.
    CHECK(server.requestCount >= 2);
    CHECK(server.requestCount <= 4);
}

// =============================================================================
// PublicBackend — proactive refresh scheduling (via TestablePublicBackend)
// =============================================================================

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: no refresh token → doRefresh never called",
    "[token_refresh][backend]"
) {
    // Credentials without a refresh token (rotation not enabled for this install)
    slack::Credentials    creds{"xoxp-t", "T001", "Team", "", "", 0};
    TestablePublicBackend backend(creds, kTestApp);
    QCoreApplication::processEvents();
    CHECK(backend.doRefreshCallCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: refresh token present but expiresAt=0 → no proactive refresh",
    "[token_refresh][backend]"
) {
    // expiresAt=0 means we don't know when the token expires; skip proactive timer
    slack::Credentials    creds{"xoxp-t", "T001", "Team", "", "refresh-tok", 0};
    TestablePublicBackend backend(creds, kTestApp);
    QCoreApplication::processEvents();
    CHECK(backend.doRefreshCallCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: token expires within 1h → doRefresh called immediately on startup",
    "[token_refresh][backend]"
) {
    qint64                soonExpiry = QDateTime::currentSecsSinceEpoch() + 300; // 5 min
    slack::Credentials    creds{"xoxp-t", "T001", "Team", "", "refresh-tok", soonExpiry};
    TestablePublicBackend backend(creds, kTestApp);

    REQUIRE(waitFor([&] { return backend.doRefreshCallCount > 0; }));
    CHECK(backend.doRefreshCallCount == 1);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: token expires in > 1h → no immediate doRefresh",
    "[token_refresh][backend]"
) {
    qint64                farExpiry = QDateTime::currentSecsSinceEpoch() + 7200; // 2h
    slack::Credentials    creds{"xoxp-t", "T001", "Team", "", "refresh-tok", farExpiry};
    TestablePublicBackend backend(creds, kTestApp);
    QCoreApplication::processEvents();
    CHECK(backend.doRefreshCallCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: proactive doRefresh auth error → authState becomes NotLoggedIn",
    "[token_refresh][backend]"
) {
    qint64                soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials    creds{"xoxp-t", "T001", "Team", "", "refresh-tok", soonExpiry};
    TestablePublicBackend backend(creds, kTestApp);
    backend.doRefreshResult = TestablePublicBackend::RefreshResult::AuthError;

    AuthState     lastState = AuthState::LoggedIn;
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { lastState = s; }, lt);

    REQUIRE(waitFor([&] { return lastState == AuthState::NotLoggedIn; }));
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: proactive doRefresh transient error → stays LoggedIn, retried later",
    "[token_refresh][backend]"
) {
    qint64                soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials    creds{"xoxp-t", "T001", "Team", "", "refresh-tok", soonExpiry};
    TestablePublicBackend backend(creds, kTestApp);
    backend.doRefreshResult = TestablePublicBackend::RefreshResult::TransientError;

    AuthState     lastState = AuthState::NotLoggedIn; // start wrong — subscribe corrects it
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { lastState = s; }, lt);

    REQUIRE(waitFor([&] { return backend.doRefreshCallCount > 0; }));
    waitFor([] { return false; }, 100); // give a wrong NotLoggedIn time to arrive
    CHECK(lastState == AuthState::LoggedIn);
}

// =============================================================================
// PublicBackend — doRefresh HTTP path (real network call to FakeHttpServer)
// =============================================================================

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend doRefresh: success updates access token in TokenStore",
    "[token_refresh][backend][http]"
) {
    FakeHttpServer server;
    server.enqueue(
        R"({"ok":true,"access_token":"xoxp-new","refresh_token":"refresh-new","expires_in":43200})"
    );

    qint64             soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-old", soonExpiry};
    TokenStore::saveWorkspace(slack::toRecord(creds));

    // Pass refreshUrl so doRefresh posts to our local server instead of Slack.
    PublicBackend backend(creds, kTestApp, server.baseUrl() + "oauth.v2.access");

    REQUIRE(waitFor([&] { return loadCreds("T001").xoxp == "xoxp-new"; }));
    CHECK(loadCreds("T001").xoxp == "xoxp-new");
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend doRefresh: success updates refreshToken in TokenStore",
    "[token_refresh][backend][http]"
) {
    FakeHttpServer server;
    server.enqueue(
        R"({"ok":true,"access_token":"xoxp-new","refresh_token":"refresh-new","expires_in":43200})"
    );

    qint64             soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-old", soonExpiry};
    TokenStore::saveWorkspace(slack::toRecord(creds));

    PublicBackend backend(creds, kTestApp, server.baseUrl() + "oauth.v2.access");

    REQUIRE(waitFor([&] { return loadCreds("T001").refreshToken == "refresh-new"; }));
    CHECK(loadCreds("T001").refreshToken == "refresh-new");
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend doRefresh: success updates expiresAt in TokenStore",
    "[token_refresh][backend][http]"
) {
    FakeHttpServer server;
    server.enqueue(
        R"({"ok":true,"access_token":"xoxp-new","refresh_token":"refresh-new","expires_in":43200})"
    );

    qint64             soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-old", soonExpiry};
    TokenStore::saveWorkspace(slack::toRecord(creds));

    const qint64  before = QDateTime::currentSecsSinceEpoch();
    PublicBackend backend(creds, kTestApp, server.baseUrl() + "oauth.v2.access");

    REQUIRE(waitFor([&] { return loadCreds("T001").expiresAt > soonExpiry; }));
    // expiresAt should be approximately now + 43200 (within a few seconds of tolerance)
    const qint64 saved = loadCreds("T001").expiresAt;
    CHECK(saved >= before + 43200);
    CHECK(saved <= before + 43200 + 5);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend doRefresh: Slack API error → authState becomes NotLoggedIn",
    "[token_refresh][backend][http]"
) {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"invalid_refresh_token"})");

    qint64             soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-bad", soonExpiry};
    TokenStore::saveWorkspace(slack::toRecord(creds));

    PublicBackend backend(creds, kTestApp, server.baseUrl() + "oauth.v2.access");

    AuthState     lastState = AuthState::LoggedIn;
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { lastState = s; }, lt);

    REQUIRE(waitFor([&] { return lastState == AuthState::NotLoggedIn; }));
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend doRefresh: network error → stays LoggedIn (transient, retried later)",
    "[token_refresh][backend][http]"
) {
    // Nothing listens on port 1 — the refresh POST fails with a connection
    // error, which must NOT log the user out (e.g. waking from suspend before
    // the network is back).
    qint64             soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-tok", soonExpiry};
    TokenStore::saveWorkspace(slack::toRecord(creds));

    PublicBackend backend(creds, kTestApp, "http://127.0.0.1:1/oauth.v2.access");

    AuthState     lastState = AuthState::NotLoggedIn; // start wrong — subscribe corrects it
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { lastState = s; }, lt);

    waitFor([] { return false; }, 300); // let the failed refresh attempt complete
    CHECK(lastState == AuthState::LoggedIn);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: near-expiry without refresh token → no HTTP request, auth stays LoggedIn",
    "[token_refresh][backend][http]"
) {
    // setupTokenRefresh guards on _refreshToken.isEmpty(); no timer is created when it's absent,
    // so the backend makes no refresh attempt even if expiresAt is near.
    qint64             soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    slack::Credentials creds{"xoxp-old", "T001", "Team", "", "", soonExpiry};

    // Use a URL that would cause a connection error if doRefresh were called.
    PublicBackend backend(creds, kTestApp, "http://127.0.0.1:1/should-not-reach");
    QCoreApplication::processEvents();

    AuthState     currentState = AuthState::NotLoggedIn; // start wrong — subscribe corrects it
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { currentState = s; }, lt);
    CHECK(currentState == AuthState::LoggedIn);
}
