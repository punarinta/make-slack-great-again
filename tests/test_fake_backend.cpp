// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>
#include "backend/fake_backend/fake_backend.h"
#include "rpl/producer.h"
#include "rpl/variable.h"

// ── collect helper ────────────────────────────────────────────────────────────
// Subscribes to a producer and returns the first value it fires synchronously.

template <typename T>
static T collect(auto producer) {
    T             out{};
    rpl::lifetime lt;
    std::move(producer) | rpl::on_next([&out](T v) { out = std::move(v); }, lt);
    return out;
}

// ── authState / capabilities ──────────────────────────────────────────────────

TEST_CASE("authState returns LoggedIn", "[fake_backend]") {
    FakeBackend fb;
    CHECK(collect<AuthState>(fb.authState()) == AuthState::LoggedIn);
}

TEST_CASE("capabilities all false", "[fake_backend]") {
    FakeBackend fb;
    auto        caps = fb.capabilities();
    CHECK(!caps.typing);
    CHECK(!caps.livePresence);
    CHECK(!caps.huddles);
}

// ── loadMe ────────────────────────────────────────────────────────────────────

TEST_CASE("loadMe returns U001", "[fake_backend]") {
    FakeBackend fb;
    CHECK(collect<UserId>(fb.loadMe()) == UserId{"U001"});
}

// ── loadConversations ─────────────────────────────────────────────────────────

TEST_CASE("loadConversations returns 5 conversations", "[fake_backend][convs]") {
    FakeBackend fb;
    auto        convs = collect<std::vector<Conversation>>(fb.loadConversations());
    CHECK(convs.size() == 5);
}

TEST_CASE("loadConversations includes #general as public channel", "[fake_backend][convs]") {
    FakeBackend fb;
    auto        convs = collect<std::vector<Conversation>>(fb.loadConversations());
    auto        it    = std::find_if(convs.begin(), convs.end(), [](const Conversation &c) {
        return c.id == ConversationId{"C001"};
    });
    REQUIRE(it != convs.end());
    CHECK(it->kind == ConvKind::PublicChannel);
    CHECK(it->name == "general");
    CHECK(it->isMember == true);
    CHECK(it->unread == 0);
}

TEST_CASE("loadConversations #random has unread badge", "[fake_backend][convs]") {
    FakeBackend fb;
    auto        convs = collect<std::vector<Conversation>>(fb.loadConversations());
    auto        it    = std::find_if(convs.begin(), convs.end(), [](const Conversation &c) {
        return c.id == ConversationId{"C002"};
    });
    REQUIRE(it != convs.end());
    CHECK(it->unread == 2);
}

TEST_CASE("loadConversations includes #secret as private channel", "[fake_backend][convs]") {
    FakeBackend fb;
    auto        convs = collect<std::vector<Conversation>>(fb.loadConversations());
    auto        it    = std::find_if(convs.begin(), convs.end(), [](const Conversation &c) {
        return c.id == ConversationId{"C003"};
    });
    REQUIRE(it != convs.end());
    CHECK(it->kind == ConvKind::PrivateChannel);
}

TEST_CASE("loadConversations includes DM with dmUser set", "[fake_backend][convs]") {
    FakeBackend fb;
    auto        convs = collect<std::vector<Conversation>>(fb.loadConversations());
    auto        it    = std::find_if(convs.begin(), convs.end(), [](const Conversation &c) {
        return c.id == ConversationId{"D001"};
    });
    REQUIRE(it != convs.end());
    CHECK(it->kind == ConvKind::Im);
    REQUIRE(it->dmUser.has_value());
    CHECK(*it->dmUser == UserId{"U002"});
}

// ── loadUsers ─────────────────────────────────────────────────────────────────

TEST_CASE("loadUsers returns 4 users", "[fake_backend][users]") {
    FakeBackend fb;
    auto        users = collect<std::vector<User>>(fb.loadUsers());
    CHECK(users.size() == 4);
}

TEST_CASE("loadUsers bob is active and not a bot", "[fake_backend][users]") {
    FakeBackend fb;
    auto        users = collect<std::vector<User>>(fb.loadUsers());
    auto        it    = std::find_if(users.begin(), users.end(), [](const User &u) {
        return u.id == UserId{"U001"};
    });
    REQUIRE(it != users.end());
    CHECK(it->name == "bob");
    CHECK(it->displayName == "Bob Builder");
    CHECK(it->isActive == true);
    CHECK(it->isBot == false);
}

TEST_CASE("loadUsers alice is inactive and not a bot", "[fake_backend][users]") {
    FakeBackend fb;
    auto        users = collect<std::vector<User>>(fb.loadUsers());
    auto        it    = std::find_if(users.begin(), users.end(), [](const User &u) {
        return u.id == UserId{"U002"};
    });
    REQUIRE(it != users.end());
    CHECK(it->displayName == "Alice Wonder");
    CHECK(it->isActive == false);
    CHECK(it->isBot == false);
}

TEST_CASE("loadUsers bot has isBot set", "[fake_backend][users]") {
    FakeBackend fb;
    auto        users = collect<std::vector<User>>(fb.loadUsers());
    auto        it    = std::find_if(users.begin(), users.end(), [](const User &u) {
        return u.id == UserId{"U003"};
    });
    REQUIRE(it != users.end());
    CHECK(it->isBot == true);
}

// ── loadHistory ───────────────────────────────────────────────────────────────

TEST_CASE("loadHistory C001 returns 3 messages", "[fake_backend][history]") {
    FakeBackend fb;
    auto        page = collect<MessagePage>(fb.loadHistory(ConversationId{"C001"}, {}));
    CHECK(page.messages.size() == 3);
    CHECK(!page.olderCursor.has_value());
}

TEST_CASE("loadHistory C001 messages are in fixture order", "[fake_backend][history]") {
    FakeBackend fb;
    auto        page = collect<MessagePage>(fb.loadHistory(ConversationId{"C001"}, {}));
    REQUIRE(page.messages.size() == 3);
    CHECK(page.messages[0].author == UserId{"U002"});
    CHECK(page.messages[0].text.text == "Hey everyone, welcome!");
    CHECK(page.messages[1].author == UserId{"U001"});
    CHECK(page.messages[1].text.text == "Thanks! Excited to be here.");
    CHECK(page.messages[2].author == UserId{"U003"});
    CHECK(page.messages[2].text.text == "I am a bot. Beep boop.");
}

TEST_CASE("loadHistory C001 timestamps are correct", "[fake_backend][history]") {
    FakeBackend fb;
    auto        page = collect<MessagePage>(fb.loadHistory(ConversationId{"C001"}, {}));
    REQUIRE(page.messages.size() == 3);
    CHECK(page.messages[0].ts == "1000000000.000001");
    CHECK(page.messages[1].ts == "1000000000.000002");
    CHECK(page.messages[2].ts == "1000000000.000003");
}

TEST_CASE("loadHistory C002 returns 2 messages", "[fake_backend][history]") {
    FakeBackend fb;
    auto        page = collect<MessagePage>(fb.loadHistory(ConversationId{"C002"}, {}));
    REQUIRE(page.messages.size() == 2);
    CHECK(page.messages[0].text.text == "Anyone up for a coffee break?");
    CHECK(page.messages[1].text.text == "Always :coffee:");
}

TEST_CASE("loadHistory unknown conv returns empty page", "[fake_backend][history]") {
    FakeBackend fb;
    auto        page = collect<MessagePage>(fb.loadHistory(ConversationId{"C_UNKNOWN"}, {}));
    CHECK(page.messages.empty());
    CHECK(!page.olderCursor.has_value());
}

TEST_CASE("loadHistory cursor parameter is ignored", "[fake_backend][history]") {
    FakeBackend fb;
    auto withCursor    = collect<MessagePage>(fb.loadHistory(ConversationId{"C001"}, {"abc123"}));
    auto withoutCursor = collect<MessagePage>(fb.loadHistory(ConversationId{"C001"}, {}));
    CHECK(withCursor.messages.size() == withoutCursor.messages.size());
}

// ── loadThread ────────────────────────────────────────────────────────────────

TEST_CASE("loadThread returns empty page", "[fake_backend]") {
    FakeBackend fb;
    auto        page =
        collect<MessagePage>(fb.loadThread(ConversationId{"C001"}, "1000000000.000001", {}));
    CHECK(page.messages.empty());
}

// ── loadPresence ──────────────────────────────────────────────────────────────

TEST_CASE("loadPresence always returns false", "[fake_backend]") {
    FakeBackend fb;
    CHECK(collect<bool>(fb.loadPresence(UserId{"U001"})) == false);
    CHECK(collect<bool>(fb.loadPresence(UserId{"U002"})) == false);
}

// ── searchMessages / loadEmojiList ────────────────────────────────────────────

TEST_CASE("searchMessages returns empty results", "[fake_backend]") {
    FakeBackend fb;
    auto        results = collect<std::vector<SearchResult>>(fb.searchMessages("anything"));
    CHECK(results.empty());
}

TEST_CASE("loadEmojiList returns empty map", "[fake_backend]") {
    FakeBackend fb;
    auto        map = collect<QHash<QString, QString>>(fb.loadEmojiList());
    CHECK(map.isEmpty());
}

// ── events / fireEvent ────────────────────────────────────────────────────────

TEST_CASE("fireEvent delivers event to subscriber", "[fake_backend][events]") {
    FakeBackend        fb;
    rpl::lifetime      lt;
    std::vector<Event> received;
    fb.events() | rpl::on_next([&](Event e) { received.push_back(std::move(e)); }, lt);

    fb.fireEvent(EvTyping{ConversationId{"C001"}, UserId{"U001"}});

    REQUIRE(received.size() == 1);
    REQUIRE(std::holds_alternative<EvTyping>(received[0]));
    CHECK(std::get<EvTyping>(received[0]).conv == ConversationId{"C001"});
    CHECK(std::get<EvTyping>(received[0]).user == UserId{"U001"});
}

TEST_CASE("fireEvent delivers multiple events in order", "[fake_backend][events]") {
    FakeBackend        fb;
    rpl::lifetime      lt;
    std::vector<Event> received;
    fb.events() | rpl::on_next([&](Event e) { received.push_back(std::move(e)); }, lt);

    fb.fireEvent(EvTyping{ConversationId{"C001"}, UserId{"U001"}});
    fb.fireEvent(EvPresenceChanged{UserId{"U002"}, true});

    REQUIRE(received.size() == 2);
    CHECK(std::holds_alternative<EvTyping>(received[0]));
    CHECK(std::holds_alternative<EvPresenceChanged>(received[1]));
    CHECK(std::get<EvPresenceChanged>(received[1]).active == true);
}

TEST_CASE("fireEvent before subscription is not received", "[fake_backend][events]") {
    FakeBackend fb;
    fb.fireEvent(EvTyping{ConversationId{"C001"}, UserId{"U001"}});

    rpl::lifetime      lt;
    std::vector<Event> received;
    fb.events() | rpl::on_next([&](Event e) { received.push_back(std::move(e)); }, lt);

    CHECK(received.empty()); // hot stream — past events not replayed
}

TEST_CASE("cancelling lifetime stops event delivery", "[fake_backend][events]") {
    FakeBackend        fb;
    std::vector<Event> received;
    {
        rpl::lifetime lt;
        fb.events() | rpl::on_next([&](Event e) { received.push_back(std::move(e)); }, lt);
        fb.fireEvent(EvTyping{ConversationId{"C001"}, UserId{"U001"}});
        CHECK(received.size() == 1);
    } // lt destroyed — subscription cancelled
    fb.fireEvent(EvTyping{ConversationId{"C002"}, UserId{"U002"}});
    CHECK(received.size() == 1); // second event not delivered
}
