// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// PublicBackend::loadMembers: conversations.members, which pages by cursor. The
// header's member list shows the whole answer, so every page has to be read
// before it is handed over.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"
#include "test_support.h"

#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"
#include "fake_http_server.h"

using namespace slack;

MSGA_TEST_MAIN(argc, argv) {
    return msga_test::runCoreAppWithTempSettings(argc, argv);
}

namespace {

using msga_test::waitFor;

const AppConfig   kTestApp{"id", "secret", ""};
const Credentials kOAuth{"xoxp-test", "T1", "Test", "", "", 0};

struct MembersLoad {
    std::vector<UserId> members;
    QString             err;
    bool                done = false;

    void run(PublicBackend &backend, const ConversationId &conv) {
        backend.loadMembers(conv, [this](std::vector<UserId> ids, QString e) {
            members = std::move(ids);
            err     = std::move(e);
            done    = true;
        });
    }
};

} // namespace

TEST_CASE("conversations.members is read to the last page", "[members]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true,"members":["U1","U2"],
                       "response_metadata":{"next_cursor":"page2"}})");
    server.enqueue(R"({"ok":true,"members":["U3"],"response_metadata":{"next_cursor":""}})");

    PublicBackend backend{kOAuth, kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    MembersLoad load;
    load.run(backend, ConversationId{"C1"});
    REQUIRE(waitFor([&] { return load.done; }));
    CHECK(load.err.isEmpty());
    CHECK(load.members == std::vector<UserId>{UserId{"U1"}, UserId{"U2"}, UserId{"U3"}});
    REQUIRE(server.requestTargets.size() == 2);
    CHECK(server.requestPaths[0] == "/conversations.members");
    CHECK(server.requestTargets[0].contains("channel=C1"));
    CHECK(server.requestTargets[1].contains("cursor=page2"));
}

TEST_CASE("a refused conversations.members reports its error", "[members]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"channel_not_found"})");

    PublicBackend backend{kOAuth, kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    MembersLoad load;
    load.run(backend, ConversationId{"C404"});
    REQUIRE(waitFor([&] { return load.done; }));
    CHECK(load.err == "channel_not_found");
    CHECK(load.members.empty());
}
