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
#include "backend/public_backend/public_backend.h"
#include "network/web_api_client.h"
#include "rpl/producer.h"

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

// ── FakeHttpServer ────────────────────────────────────────────────────────────
// Minimal local HTTP/1.1 server. Enqueue JSON bodies with enqueue(); each
// incoming request consumes one entry and gets that body as the response.
// Handles both GET and POST (reads until full Content-Length received).

class FakeHttpServer {
public:
    FakeHttpServer() {
        QObject::connect(&_server, &QTcpServer::newConnection, &_server, [this] {
            onNewConnection();
        });
        _server.listen(QHostAddress::LocalHost);
    }

    QString baseUrl() const { return QString("http://127.0.0.1:%1/").arg(_server.serverPort()); }

    void enqueue(const QByteArray &json) { _pending.append(json); }

    int requestCount = 0;

private:
    void onNewConnection() {
        auto *sock = _server.nextPendingConnection();
        auto *buf  = new QByteArray;
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
            buf->append(sock->readAll());

            // Wait for complete headers.
            int headerEnd = buf->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;

            // Parse Content-Length so we wait for the full POST body.
            QByteArray headers       = buf->left(headerEnd);
            int        contentLength = 0;
            for (const QByteArray &line : headers.split('\n')) {
                if (line.trimmed().toLower().startsWith("content-length:"))
                    contentLength = line.trimmed().mid(15).trimmed().toInt();
            }
            if (buf->size() < headerEnd + 4 + contentLength)
                return; // body not fully received yet

            ++requestCount;
            QByteArray body = _pending.isEmpty() ? R"({"ok":false,"error":"no_response_queued"})"
                                                 : _pending.takeFirst();

            QByteArray resp;
            resp += "HTTP/1.1 200 OK\r\n";
            resp += "Content-Type: application/json\r\n";
            resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            resp += "Connection: close\r\n\r\n";
            resp += body;
            sock->write(resp);
            sock->flush();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, [sock, buf] {
            delete buf;
            sock->deleteLater();
        });
    }

    QTcpServer        _server;
    QList<QByteArray> _pending;
};

// ── TestablePublicBackend ─────────────────────────────────────────────────────
// Overrides doRefresh so tests control success/failure without real HTTP.
// Updates _tokenExpiresAt to a far future value on success so that
// scheduleProactiveRefresh re-arms the timer for hours from now, preventing
// an immediate re-fire that would create an infinite processEvents loop.

class TestablePublicBackend : public PublicBackend {
public:
    int  doRefreshCallCount = 0;
    bool doRefreshSucceeds  = true;

    using PublicBackend::PublicBackend;

protected:
    void doRefresh(std::function<void(bool)> done) override {
        ++doRefreshCallCount;
        if (doRefreshSucceeds)
            _tokenExpiresAt = QDateTime::currentSecsSinceEpoch() + 43200;
        done(doRefreshSucceeds);
    }
};

// ── Per-test credentials fixture ──────────────────────────────────────────────

static const TokenStore::AppConfig kTestApp{"test-client-id", "test-client-secret", ""};

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

// =============================================================================
// PublicBackend — proactive refresh scheduling (via TestablePublicBackend)
// =============================================================================

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: no refresh token → doRefresh never called",
    "[token_refresh][backend]"
) {
    // Credentials without a refresh token (rotation not enabled for this install)
    TokenStore::Credentials creds{"xoxp-t", "T001", "Team", "", "", 0};
    TestablePublicBackend   backend(creds, kTestApp);
    QCoreApplication::processEvents();
    CHECK(backend.doRefreshCallCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: refresh token present but expiresAt=0 → no proactive refresh",
    "[token_refresh][backend]"
) {
    // expiresAt=0 means we don't know when the token expires; skip proactive timer
    TokenStore::Credentials creds{"xoxp-t", "T001", "Team", "", "refresh-tok", 0};
    TestablePublicBackend   backend(creds, kTestApp);
    QCoreApplication::processEvents();
    CHECK(backend.doRefreshCallCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: token expires within 1h → doRefresh called immediately on startup",
    "[token_refresh][backend]"
) {
    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300; // 5 min
    TokenStore::Credentials creds{"xoxp-t", "T001", "Team", "", "refresh-tok", soonExpiry};
    TestablePublicBackend   backend(creds, kTestApp);

    REQUIRE(waitFor([&] { return backend.doRefreshCallCount > 0; }));
    CHECK(backend.doRefreshCallCount == 1);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: token expires in > 1h → no immediate doRefresh",
    "[token_refresh][backend]"
) {
    qint64                  farExpiry = QDateTime::currentSecsSinceEpoch() + 7200; // 2h
    TokenStore::Credentials creds{"xoxp-t", "T001", "Team", "", "refresh-tok", farExpiry};
    TestablePublicBackend   backend(creds, kTestApp);
    QCoreApplication::processEvents();
    CHECK(backend.doRefreshCallCount == 0);
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: proactive doRefresh failure → authState becomes NotLoggedIn",
    "[token_refresh][backend]"
) {
    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    TokenStore::Credentials creds{"xoxp-t", "T001", "Team", "", "refresh-tok", soonExpiry};
    TestablePublicBackend   backend(creds, kTestApp);
    backend.doRefreshSucceeds = false;

    AuthState     lastState = AuthState::LoggedIn;
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { lastState = s; }, lt);

    REQUIRE(waitFor([&] { return lastState == AuthState::NotLoggedIn; }));
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

    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    TokenStore::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-old", soonExpiry};
    TokenStore::saveWorkspace(creds);

    // Pass refreshUrl so doRefresh posts to our local server instead of Slack.
    PublicBackend backend(creds, kTestApp, {}, server.baseUrl() + "oauth.v2.exchange");

    REQUIRE(waitFor([&] { return TokenStore::loadWorkspace("T001").xoxp == "xoxp-new"; }));
    CHECK(TokenStore::loadWorkspace("T001").xoxp == "xoxp-new");
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

    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    TokenStore::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-old", soonExpiry};
    TokenStore::saveWorkspace(creds);

    PublicBackend backend(creds, kTestApp, {}, server.baseUrl() + "oauth.v2.exchange");

    REQUIRE(waitFor([&] {
        return TokenStore::loadWorkspace("T001").refreshToken == "refresh-new";
    }));
    CHECK(TokenStore::loadWorkspace("T001").refreshToken == "refresh-new");
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

    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    TokenStore::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-old", soonExpiry};
    TokenStore::saveWorkspace(creds);

    const qint64  before = QDateTime::currentSecsSinceEpoch();
    PublicBackend backend(creds, kTestApp, {}, server.baseUrl() + "oauth.v2.exchange");

    REQUIRE(waitFor([&] { return TokenStore::loadWorkspace("T001").expiresAt > soonExpiry; }));
    // expiresAt should be approximately now + 43200 (within a few seconds of tolerance)
    const qint64 saved = TokenStore::loadWorkspace("T001").expiresAt;
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

    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    TokenStore::Credentials creds{"xoxp-old", "T001", "Team", "", "refresh-bad", soonExpiry};
    TokenStore::saveWorkspace(creds);

    PublicBackend backend(creds, kTestApp, {}, server.baseUrl() + "oauth.v2.exchange");

    AuthState     lastState = AuthState::LoggedIn;
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { lastState = s; }, lt);

    REQUIRE(waitFor([&] { return lastState == AuthState::NotLoggedIn; }));
}

TEST_CASE_METHOD(
    RefreshFixture,
    "PublicBackend: near-expiry without refresh token → no HTTP request, auth stays LoggedIn",
    "[token_refresh][backend][http]"
) {
    // setupTokenRefresh guards on _refreshToken.isEmpty(); no timer is created when it's absent,
    // so the backend makes no refresh attempt even if expiresAt is near.
    qint64                  soonExpiry = QDateTime::currentSecsSinceEpoch() + 300;
    TokenStore::Credentials creds{"xoxp-old", "T001", "Team", "", "", soonExpiry};

    // Use a URL that would cause a connection error if doRefresh were called.
    PublicBackend backend(creds, kTestApp, {}, "http://127.0.0.1:1/should-not-reach");
    QCoreApplication::processEvents();

    AuthState     currentState = AuthState::NotLoggedIn; // start wrong — subscribe corrects it
    rpl::lifetime lt;
    backend.authState() | rpl::on_next([&](AuthState s) { currentState = s; }, lt);
    CHECK(currentState == AuthState::LoggedIn);
}
