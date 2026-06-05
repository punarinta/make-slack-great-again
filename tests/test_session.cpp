// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "session/session.h"
#include "backend/backend.h"
#include "rpl/producer.h"
#include "rpl/variable.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── StubBackend ───────────────────────────────────────────────────────────────
// Minimal controllable Backend for session tests.
// All producers use rpl::variable so they fire synchronously on subscription.

struct StubBackend : Backend {
    rpl::variable<AuthState>                  _authState{AuthState::LoggedIn};
    rpl::variable<UserId>                     _meId;
    rpl::variable<std::vector<Conversation>>  _convs;
    rpl::variable<std::vector<User>>          _users;
    rpl::event_stream<Event>                  _events;

    bool   presenceResult = false;
    struct SentMessage { ConversationId conv; OutgoingMessage msg; };
    std::vector<SentMessage> sentMessages;

    rpl::producer<AuthState> authState() const override { return _authState.value(); }
    Capabilities capabilities()          const override { return {}; }
    void connectRealtime()    override {}
    void disconnectRealtime() override {}

    rpl::producer<UserId> loadMe() override { return _meId.value(); }

    rpl::producer<std::vector<Conversation>> loadConversations() override {
        return _convs.value();
    }
    rpl::producer<std::vector<User>> loadUsers() override { return _users.value(); }

    rpl::producer<bool> loadPresence(UserId) override {
        return rpl::variable<bool>(presenceResult).value();
    }
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }

    void sendMessage(ConversationId c, OutgoingMessage m) override {
        sentMessages.push_back({c, std::move(m)});
    }
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
    void deleteMessage(ConversationId, Ts)                 override {}
    void addReaction(ConversationId, Ts, QString)          override {}
    void removeReaction(ConversationId, Ts, QString)       override {}
    void markRead(ConversationId, Ts)                      override {}

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString,QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString,QString>>({}).value();
    }
    void uploadFile(ConversationId, const QString &) override {}
    void downloadFile(const QString &,
                      std::function<void(QByteArray)>,
                      std::function<void(QString)>) override {}

    rpl::producer<Event> events() const override { return _events.events(); }

    void fireEvent(Event e) { _events.fire(std::move(e)); }
};

// ── SessionFixture ────────────────────────────────────────────────────────────

static const User kAlice = User{
    UserId{"U1"}, "alice", "Alice Wonder", "https://av/alice.png",
    /*isBot=*/false, /*isActive=*/false, /*isDeactivated=*/false
};
static const User kBob = User{
    UserId{"U2"}, "bob", "Bob Builder", "",
    /*isBot=*/false, /*isActive=*/true, /*isDeactivated=*/false
};
static const Conversation kGeneral = Conversation{
    ConversationId{"C1"}, ConvKind::PublicChannel, "general",
    /*desc=*/"", /*isMember=*/true, /*lastRead=*/"0", /*unread=*/2
};
static const Conversation kRandom = Conversation{
    ConversationId{"C2"}, ConvKind::PublicChannel, "random",
    /*desc=*/"", /*isMember=*/true, /*lastRead=*/"0", /*unread=*/0
};

struct SessionFixture {
    const QString teamId = "T_SESSION_TEST";
    QString       baseDir;
    StubBackend  *stub;               // non-owning; owned by session
    std::unique_ptr<Session> session;

    SessionFixture() {
        baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + "/cache/" + teamId;

        auto backend = std::make_unique<StubBackend>();
        stub = backend.get();
        stub->_meId  = UserId{"U1"};
        stub->_convs = std::vector<Conversation>{kGeneral, kRandom};
        stub->_users = std::vector<User>{kAlice, kBob};

        session = std::make_unique<Session>(std::move(backend), teamId);
        session->start();
    }
    ~SessionFixture() {
        session.reset();
        QDir(baseDir).removeRecursively();
    }

    // Collect events fired after this helper is called.
    // Usage: auto [events, lt] = collectEvents();  stub->fireEvent(...);
    struct EventCollector {
        std::vector<Event> events;
        rpl::lifetime      lt;
    };
    EventCollector collectEvents() {
        EventCollector c;
        session->events() | rpl::on_next([&c](Event e) {
            c.events.push_back(std::move(e));
        }, c.lt);
        return c;
    }
};

// ── start() — initial state ───────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "start() populates conversations from backend", "[session]") {
    REQUIRE(session->findConversation(ConversationId{"C1"}) != nullptr);
    REQUIRE(session->findConversation(ConversationId{"C2"}) != nullptr);
    CHECK(session->findConversation(ConversationId{"C1"})->name == "general");
    CHECK(session->findConversation(ConversationId{"C1"})->unread == 2);
}

TEST_CASE_METHOD(SessionFixture, "start() populates users from backend", "[session]") {
    REQUIRE(session->findUser(UserId{"U1"}) != nullptr);
    REQUIRE(session->findUser(UserId{"U2"}) != nullptr);
    CHECK(session->findUser(UserId{"U1"})->displayName == "Alice Wonder");
    CHECK(session->findUser(UserId{"U2"})->isActive    == true);
}

TEST_CASE_METHOD(SessionFixture, "start() sets meUserId from loadMe", "[session]") {
    CHECK(session->meUserId() == UserId{"U1"});
}

// ── findUser / findConversation ───────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "findUser returns null for unknown id", "[session]") {
    CHECK(session->findUser(UserId{"U_GHOST"}) == nullptr);
}

TEST_CASE_METHOD(SessionFixture, "findConversation returns null for unknown id", "[session]") {
    CHECK(session->findConversation(ConversationId{"C_GHOST"}) == nullptr);
}

// ── EvPresenceChanged ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "EvPresenceChanged updates user isActive", "[session][events]") {
    REQUIRE(!session->findUser(UserId{"U1"})->isActive);
    stub->fireEvent(EvPresenceChanged{UserId{"U1"}, true});
    REQUIRE(session->findUser(UserId{"U1"}) != nullptr);
    CHECK(session->findUser(UserId{"U1"})->isActive == true);
}

TEST_CASE_METHOD(SessionFixture, "EvPresenceChanged is forwarded to subscribers", "[session][events]") {
    auto col = collectEvents();
    stub->fireEvent(EvPresenceChanged{UserId{"U1"}, true});
    REQUIRE(col.events.size() == 1);
    CHECK(std::holds_alternative<EvPresenceChanged>(col.events[0]));
    CHECK(std::get<EvPresenceChanged>(col.events[0]).active == true);
}

// ── EvConvMarked ──────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "EvConvMarked updates conversation unread and lastRead", "[session][events]") {
    stub->fireEvent(EvConvMarked{ConversationId{"C1"}, "999.000", 0});
    auto *c = session->findConversation(ConversationId{"C1"});
    REQUIRE(c != nullptr);
    CHECK(c->unread   == 0);
    CHECK(c->lastRead == "999.000");
}

// ── EvMessageNew unread logic ─────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "EvMessageNew increments unread for other-user message in non-reading conv", "[session][events]") {
    // C2 starts at unread=0; U2 sends a message; unread should become 1.
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
}

TEST_CASE_METHOD(SessionFixture, "EvMessageNew does not increment unread for own message", "[session][events]") {
    // meUserId is U1; U1 sends a message — unread stays at 0.
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U1"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

TEST_CASE_METHOD(SessionFixture, "EvMessageNew does not increment unread for currently reading conv", "[session][events]") {
    session->setReading(ConversationId{"C2"});
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

// ── setReading ────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "setReading zeroes unread badge for conversation", "[session]") {
    // C1 starts at unread=2.
    REQUIRE(session->findConversation(ConversationId{"C1"})->unread == 2);
    session->setReading(ConversationId{"C1"});
    CHECK(session->findConversation(ConversationId{"C1"})->unread == 0);
}

TEST_CASE_METHOD(SessionFixture, "setReading with empty id does not crash", "[session]") {
    CHECK_NOTHROW(session->setReading(ConversationId{""}));
}

// ── sendMessage ───────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "sendMessage fires optimistic EvMessageNew", "[session][send]") {
    auto col = collectEvents();
    session->sendMessage(ConversationId{"C1"}, "hello");
    REQUIRE(col.events.size() == 1);
    REQUIRE(std::holds_alternative<EvMessageNew>(col.events[0]));
    const auto &ev = std::get<EvMessageNew>(col.events[0]);
    CHECK(ev.conv          == ConversationId{"C1"});
    CHECK(ev.msg.text.text == "hello");
    CHECK(ev.msg.author    == UserId{"U1"});
}

TEST_CASE_METHOD(SessionFixture, "sendMessage includes threadRoot in optimistic event", "[session][send]") {
    auto col = collectEvents();
    session->sendMessage(ConversationId{"C1"}, "reply", Ts{"100.000"});
    REQUIRE(col.events.size() == 1);
    const auto &ev = std::get<EvMessageNew>(col.events[0]);
    REQUIRE(ev.msg.threadRoot.has_value());
    CHECK(*ev.msg.threadRoot == "100.000");
}

TEST_CASE_METHOD(SessionFixture, "sendMessage delegates to backend", "[session][send]") {
    session->sendMessage(ConversationId{"C1"}, "hi");
    REQUIRE(stub->sentMessages.size() == 1);
    CHECK(stub->sentMessages[0].conv         == ConversationId{"C1"});
    CHECK(stub->sentMessages[0].msg.text.text == "hi");
}

// ── requestPresence ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "requestPresence updates user and fires EvPresenceChanged", "[session]") {
    stub->presenceResult = true;
    auto col = collectEvents();
    session->requestPresence(UserId{"U1"});
    // Presence updated in-memory
    CHECK(session->findUser(UserId{"U1"})->isActive == true);
    // Event forwarded to subscribers
    REQUIRE(col.events.size() == 1);
    REQUIRE(std::holds_alternative<EvPresenceChanged>(col.events[0]));
    CHECK(std::get<EvPresenceChanged>(col.events[0]).active == true);
}

// ── cache passthrough ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "cacheMessages/cachedMessages round-trip", "[session][cache]") {
    Message m;
    m.ts     = "100.000";
    m.author = UserId{"U1"};
    m.text   = TextWithEntities{"cached msg", {}};
    ConversationId conv{"C1"};
    session->cacheMessages(conv, {m});
    auto loaded = session->cachedMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].ts        == "100.000");
    CHECK(loaded[0].text.text == "cached msg");
}

TEST_CASE_METHOD(SessionFixture, "saveLastConv/loadLastConv round-trip", "[session][cache]") {
    session->saveLastConv(ConversationId{"C1"}, "general");
    auto [conv, name] = session->loadLastConv();
    CHECK(conv == ConversationId{"C1"});
    CHECK(name == "general");
}

TEST_CASE_METHOD(SessionFixture, "cacheImage/cachedImage round-trip", "[session][cache]") {
    const QString url  = "https://example.com/avatar.png";
    const QByteArray data = "PNG_DATA";
    session->cacheImage(url, data);
    CHECK(session->cachedImage(url) == data);
}
