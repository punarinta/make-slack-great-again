// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Enterprise Grid support for session (xoxc/cookie) workspaces — issue #49.
//
// Two behaviours are covered here. First, a session workspace addresses its OWN
// slack.com host rather than the shared slack.com/api, because Slack resolves a
// session token in the context of the host the call is made on and bare
// slack.com has no workspace to resolve to inside a Grid org. Second, when
// conversations.list is refused anyway with `enterprise_is_restricted` ("The
// method cannot be called from an Enterprise"), the roster is loaded the way the
// web client loads it: client.userBoot for channels, im.list for DMs.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSettings>
#include <QTemporaryDir>

#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"
#include "backend/slack/web_api_client.h"
#include "fake_http_server.h"
#include "rpl/producer.h"

using namespace slack;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

namespace {

bool waitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

const AppConfig kTestApp{"id", "secret", ""};

Credentials sessionCreds(const QString &workspaceUrl) {
    Credentials c{"xoxc-test", "T1", "Test", "", "", 0};
    c.cookie       = "xoxd-test";
    c.workspaceUrl = workspaceUrl;
    return c;
}

// Drives loadConversations() to completion. `got` stays unset when the producer
// completes without a value — the "load failed, keep the current roster" signal.
struct ConvLoad {
    std::optional<std::vector<Conversation>> got;
    bool                                     done = false;
    rpl::lifetime                            lt;

    void run(PublicBackend &backend) {
        backend.loadConversations() |
            rpl::on_next_done(
                [this](std::vector<Conversation> convs) { got = std::move(convs); },
                [this]() { done = true; },
                lt
            );
    }
};

} // namespace

// =============================================================================
// API host
// =============================================================================

TEST_CASE("session workspace addresses its own /api/ host", "[grid]") {
    PublicBackend backend{sessionCreds("https://acme.enterprise.slack.com/"), kTestApp};
    CHECK(backend.apiBaseUrlForTests() == "https://acme.enterprise.slack.com/api/");
}

TEST_CASE("session workspace without a stored URL keeps the shared host", "[grid]") {
    PublicBackend backend{sessionCreds(""), kTestApp};
    CHECK(backend.apiBaseUrlForTests() == WebApiClient::kBaseUrl);
}

TEST_CASE("a non-https workspace URL is not adopted as the API host", "[grid]") {
    PublicBackend backend{sessionCreds("http://evil.example/"), kTestApp};
    CHECK(backend.apiBaseUrlForTests() == WebApiClient::kBaseUrl);
}

TEST_CASE("OAuth workspace keeps the shared host", "[grid]") {
    Credentials   oauth{"xoxp-test", "T1", "Test", "", "", 0}; // no cookie ⇒ not session auth
    PublicBackend backend{oauth, kTestApp};
    CHECK(backend.apiBaseUrlForTests() == WebApiClient::kBaseUrl);
}

// =============================================================================
// conversations.list
// =============================================================================

TEST_CASE("conversations.list carries team_id", "[grid]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true,"channels":[{"id":"C1","name":"general"}]})");

    PublicBackend backend{sessionCreds(""), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    ConvLoad load;
    load.run(backend);
    REQUIRE(waitFor([&] { return load.done; }));
    REQUIRE(server.requestTargets.size() == 1);
    CHECK(server.requestTargets[0].contains("team_id=T1"));
}

TEST_CASE("enterprise_is_restricted falls back to client.userBoot + im.list", "[grid]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"enterprise_is_restricted"})");
    server.enqueue(R"({"ok":true,"channels":[
        {"id":"C1","name":"general","is_channel":true},
        {"id":"C2","name":"old","is_channel":true,"is_archived":true},
        {"id":"C3","name":"mpdm-a--b-1","is_mpim":true,"is_private":true}
    ]})");
    server.enqueue(R"({"ok":true,"ims":[{"id":"D1","user":"U9","is_im":true}]})");

    PublicBackend backend{sessionCreds(""), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    ConvLoad load;
    load.run(backend);
    REQUIRE(waitFor([&] { return load.done; }));

    REQUIRE(server.requestPaths.size() == 3);
    CHECK(server.requestPaths[0] == "/conversations.list");
    CHECK(server.requestPaths[1] == "/client.userBoot");
    CHECK(server.requestPaths[2] == "/im.list");

    REQUIRE(load.got.has_value());
    // The archived channel is dropped — conversations.list was asked for
    // exclude_archived and userBoot has no equivalent switch.
    REQUIRE(load.got->size() == 3);
    CHECK((*load.got)[0].id == ConversationId{"C1"});
    CHECK((*load.got)[1].id == ConversationId{"C3"});
    CHECK((*load.got)[1].kind == ConvKind::Mpim);
    CHECK((*load.got)[2].id == ConversationId{"D1"});
    CHECK((*load.got)[2].kind == ConvKind::Im);
    CHECK((*load.got)[2].dmUser == UserId{"U9"});
    // userBoot reports no is_member field; every conversation it lists is one
    // the user belongs to, and the sidebar/poll rotation both key off isMember.
    CHECK((*load.got)[0].isMember);
}

TEST_CASE("the Grid fallback is latched after the first rejection", "[grid]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"enterprise_is_restricted"})");
    server.enqueue(R"({"ok":true,"channels":[{"id":"C1","name":"general"}]})");
    server.enqueue(R"({"ok":true,"ims":[]})");

    PublicBackend backend{sessionCreds(""), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    ConvLoad first;
    first.run(backend);
    REQUIRE(waitFor([&] { return first.done; }));

    server.enqueue(R"({"ok":true,"channels":[{"id":"C1","name":"general"}]})");
    server.enqueue(R"({"ok":true,"ims":[]})");
    ConvLoad second;
    second.run(backend);
    REQUIRE(waitFor([&] { return second.done; }));

    // No second conversations.list — the workspace has already said no.
    REQUIRE(server.requestPaths.size() == 5);
    CHECK(server.requestPaths[3] == "/client.userBoot");
    CHECK(server.requestPaths[4] == "/im.list");
    CHECK(second.got.has_value());
}

TEST_CASE("a half-loaded Grid roster is not handed over", "[grid]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"enterprise_is_restricted"})");
    server.enqueue(R"({"ok":true,"channels":[{"id":"C1","name":"general"}]})");
    server.enqueue(R"({"ok":false,"error":"missing_scope"})"); // im.list fails

    PublicBackend backend{sessionCreds(""), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    ConvLoad load;
    load.run(backend);
    REQUIRE(waitFor([&] { return load.done; }));
    // Channels without DMs would REPLACE the roster with half of itself.
    CHECK_FALSE(load.got.has_value());
}

TEST_CASE("an ordinary failure completes without wiping the roster", "[grid]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"missing_scope"})");

    PublicBackend backend{sessionCreds(""), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    ConvLoad load;
    load.run(backend);
    REQUIRE(waitFor([&] { return load.done; }));
    CHECK(server.requestPaths.size() == 1); // no Grid fallback for an unrelated error
    CHECK_FALSE(load.got.has_value());
}
