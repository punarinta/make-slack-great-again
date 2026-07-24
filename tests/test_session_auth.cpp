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
