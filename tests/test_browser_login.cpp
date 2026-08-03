// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Browser sign-in (docs/BROWSER_LOGIN_PLAN.md): the pure parts of the flow — reading
// the DevTools endpoint, turning the web client's localConfig_v2 into workspaces, and
// the host-harvesting fallback used when the client never booted. The CDP round trip
// itself needs a real browser, so it isn't covered here.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include "backend/slack/session_import/browser_login.h"

using namespace slack::session;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test-browser-login");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

TEST_CASE("the DevTools websocket endpoint is read from /json/version", "[browser_login]") {
    const QByteArray body = R"({
        "Browser": "Chrome/126.0.6478.126",
        "Protocol-Version": "1.3",
        "webSocketDebuggerUrl": "ws://127.0.0.1:41235/devtools/browser/9f2c-4d1a"
    })";
    CHECK(
        parseDebuggerUrl(body) == QStringLiteral("ws://127.0.0.1:41235/devtools/browser/9f2c-4d1a")
    );

    // Not listening yet / not a DevTools endpoint → empty, and the caller retries.
    CHECK(parseDebuggerUrl(QByteArray("<html>nope</html>")).isEmpty());
    CHECK(parseDebuggerUrl(QByteArray("{}")).isEmpty());
}

TEST_CASE("localConfig_v2 yields workspaces with their xoxc tokens", "[browser_login]") {
    const QString blob = R"({
        "teams": {
            "T111": {
                "id": "T111",
                "name": "Acme",
                "domain": "acme",
                "url": "https://acme.slack.com/",
                "token": "xoxc-111-abc",
                "icon": {"image_68": "https://a.slack-edge.com/68.png",
                         "image_88": "https://a.slack-edge.com/88.png"}
            },
            "T222": {
                "id": "T222",
                "name": "Side Project",
                "domain": "sideproj",
                "token": "not-a-token"
            }
        },
        "unrelated": 1
    })";

    const QList<TeamSession> teams = parseLocalConfig(blob);
    REQUIRE(teams.size() == 2);

    const TeamSession *acme = nullptr;
    const TeamSession *side = nullptr;
    for (const TeamSession &t : teams) {
        if (t.teamId == QLatin1String("T111"))
            acme = &t;
        if (t.teamId == QLatin1String("T222"))
            side = &t;
    }
    REQUIRE(acme != nullptr);
    REQUIRE(side != nullptr);

    CHECK(acme->teamName == QStringLiteral("Acme"));
    CHECK(acme->token == QStringLiteral("xoxc-111-abc"));
    // Trailing slash trimmed — the deriver GETs this URL directly.
    CHECK(acme->workspaceUrl == QStringLiteral("https://acme.slack.com"));
    CHECK(acme->iconUrl == QStringLiteral("https://a.slack-edge.com/88.png"));

    // No url → synthesised from the domain; a non-xoxc token is dropped so the
    // deriver scrapes a real one instead of validating garbage.
    CHECK(side->workspaceUrl == QStringLiteral("https://sideproj.slack.com"));
    CHECK(side->token.isEmpty());
}

TEST_CASE("localConfig_v2 parsing survives junk", "[browser_login]") {
    CHECK(parseLocalConfig(QString()).isEmpty());
    CHECK(parseLocalConfig(QStringLiteral("null")).isEmpty());
    CHECK(parseLocalConfig(QStringLiteral(R"({"teams":{}})")).isEmpty());
    // A team with neither a token nor any way to build a host is unusable.
    CHECK(parseLocalConfig(QStringLiteral(R"({"teams":{"T1":{"name":"x"}}})")).isEmpty());
}

TEST_CASE("host harvesting skips Slack's own infrastructure", "[browser_login]") {
    const QStringList seen{
        QStringLiteral("https://app.slack.com/client/T111/C222"), // infra
        QStringLiteral(".slack.com"),                             // no subdomain
        QStringLiteral("acme.slack.com"),                         // cookie domain
        QStringLiteral("https://acme.slack.com/ssb/redirect"),    // duplicate
        QStringLiteral("https://Other.Slack.com/messages"),       // case-insensitive
        QStringLiteral("https://edgeapi.slack.com/cache/v1"),     // infra
        QStringLiteral("https://example.com/"),                   // unrelated
    };

    const QList<TeamSession> teams = teamsFromHosts(seen);
    REQUIRE(teams.size() == 2);
    CHECK(teams.at(0).workspaceUrl == QStringLiteral("https://acme.slack.com"));
    CHECK(teams.at(1).workspaceUrl == QStringLiteral("https://other.slack.com"));
    // Tokens are left for the deriver to scrape off the boot page.
    CHECK(teams.at(0).token.isEmpty());
}

TEST_CASE("MSGA_BROWSER_LOGIN=0 turns the feature off", "[browser_login]") {
    const QByteArray prev  = qgetenv("MSGA_BROWSER_LOGIN");
    const bool       hadIt = qEnvironmentVariableIsSet("MSGA_BROWSER_LOGIN");
    qputenv("MSGA_BROWSER_LOGIN", "0");
    CHECK_FALSE(browserLoginSupported());
    CHECK(browserLoginName().isEmpty());
    if (hadIt)
        qputenv("MSGA_BROWSER_LOGIN", prev);
    else
        qunsetenv("MSGA_BROWSER_LOGIN");
}
