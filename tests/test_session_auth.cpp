// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Session-token (xoxc/xoxd) sign-in: the `d` cookie survives the credentials
// blob round-trip, and the transport emits `Cookie: d=…` only when a cookie is set.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "backend/slack/slack_auth.h"
#include "backend/slack/web_api_client.h"
#include "fake_http_server.h"

using namespace slack;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());
    return Catch::Session().run(argc, argv);
}

static bool waitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// Case-insensitively find a header line's value in a raw header block.
static QByteArray headerValue(const QByteArray &headers, const QByteArray &name) {
    for (const QByteArray &line : headers.split('\n'))
        if (line.trimmed().toLower().startsWith(name.toLower() + ":"))
            return line.mid(line.indexOf(':') + 1).trimmed();
    return {};
}

TEST_CASE("session credentials round-trip through the record blob", "[session_auth]") {
    Credentials in;
    in.xoxp     = "xoxc-abc-123";
    in.teamId   = "T123";
    in.teamName = "Acme";
    in.cookie   = "xoxd-cookievalue";

    const auto rec = toRecord(in);
    const auto out = fromRecord(rec);
    CHECK(out.xoxp == in.xoxp);
    CHECK(out.teamId == "T123");
    CHECK(out.cookie == "xoxd-cookievalue");
    CHECK(out.isSessionAuth());
}

TEST_CASE("OAuth credentials carry no cookie and the blob is unchanged", "[session_auth]") {
    Credentials oauth;
    oauth.xoxp   = "xoxp-oauth";
    oauth.teamId = "T9";

    const auto rec  = toRecord(oauth);
    // No cookie key is written for OAuth records (backward-compatible blob).
    const auto blob = QJsonDocument::fromJson(rec.auth).object();
    CHECK_FALSE(blob.contains("cookie"));

    const auto out = fromRecord(rec);
    CHECK(out.cookie.isEmpty());
    CHECK_FALSE(out.isSessionAuth());
}

TEST_CASE("transport emits the d cookie header only when a cookie is set", "[session_auth]") {
    SECTION("with cookie") {
        FakeHttpServer server;
        server.enqueue(R"({"ok":true})");
        WebApiClient client;
        client.setBaseUrl(server.baseUrl());
        client.setToken("xoxc-tok");
        client.setCookie("xoxd-thecookie");

        bool done = false;
        client.call("auth.test", QUrlQuery{}, [&](QJsonObject) { done = true; });
        REQUIRE(waitFor([&] { return done; }));

        REQUIRE(server.requestHeaders.size() == 1);
        CHECK(headerValue(server.requestHeaders[0], "Cookie") == "d=xoxd-thecookie");
        CHECK(headerValue(server.requestHeaders[0], "Authorization") == "Bearer xoxc-tok");
    }

    SECTION("without cookie") {
        FakeHttpServer server;
        server.enqueue(R"({"ok":true})");
        WebApiClient client;
        client.setBaseUrl(server.baseUrl());
        client.setToken("xoxp-tok");

        bool done = false;
        client.call("auth.test", QUrlQuery{}, [&](QJsonObject) { done = true; });
        REQUIRE(waitFor([&] { return done; }));

        REQUIRE(server.requestHeaders.size() == 1);
        CHECK(headerValue(server.requestHeaders[0], "Cookie").isEmpty());
    }
}

// Slack sets companion cookies (`d-s`, a session-freshness timestamp, and `uc`)
// on every response, and its browser client echoes them back. msga briefly did
// the same, on the theory that looking more browser-like would stop Enterprise
// Grid orgs revoking the session. It did the opposite: a controlled probe
// (scripts/slack-session-probe.py, phase 3) showed the session dying with
// invalid_auth on the SECOND call that replayed them, while a client sending
// nothing but `d` ran indefinitely. Never send back session state we don't own.
TEST_CASE("companion cookies from Set-Cookie are never echoed back", "[session_auth]") {
    FakeHttpServer server;
    server.enqueueWithSetCookie(
        R"({"ok":true})",
        {"d-s=1789211363; path=/; domain=.slack.com; secure; httponly",
         "uc=xoxd-SOMEUSERCOOKIE; path=/; domain=.slack.com"}
    );
    server.enqueue(R"({"ok":true})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("xoxc-tok");
    client.setCookie("xoxd-thecookie");

    int done = 0;
    client.call("auth.test", QUrlQuery{}, [&](QJsonObject) { ++done; });
    REQUIRE(waitFor([&] { return done == 1; }));
    // Second call goes out AFTER the Set-Cookie reply has been seen.
    client.call("conversations.list", QUrlQuery{}, [&](QJsonObject) { ++done; });
    REQUIRE(waitFor([&] { return done == 2; }));

    REQUIRE(server.requestHeaders.size() == 2);
    const QByteArray second = headerValue(server.requestHeaders[1], "Cookie");
    CHECK(second == "d=xoxd-thecookie");
    CHECK_FALSE(second.contains("d-s"));
    CHECK_FALSE(second.contains("uc="));
}

// Minimal sync mode's guarantee lives at the transport: a method that is not on
// the allowlist must never reach the network, whichever call site asked for it,
// and must fail with an error that is NOT an auth error (so it can't trigger a
// token refresh, and the backend latches the method off instead of retrying).
TEST_CASE("the method allowlist refuses calls locally, without network traffic",
          "[session_auth][allowlist]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true})"); // only the allowed call may consume this

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("xoxc-tok");
    client.setCookie("xoxd-c");
    client.setMethodAllowlist({QStringLiteral("auth.test")});

    QString blockedErr;
    bool    blockedDone = false, allowedDone = false;
    client.call(
        "users.list", QUrlQuery{}, [](QJsonObject) {},
        [&](QString e) { blockedErr = e; blockedDone = true; }
    );
    client.call("auth.test", QUrlQuery{}, [&](QJsonObject) { allowedDone = true; });

    REQUIRE(waitFor([&] { return blockedDone && allowedDone; }));
    CHECK(blockedErr == WebApiClient::kBlockedByAllowlist);
    CHECK(server.requestCount == 1);                      // only auth.test went out
    CHECK(server.requestPaths.value(0) == "/auth.test");
}
