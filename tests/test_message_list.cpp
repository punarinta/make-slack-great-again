// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Regression tests for MessageListWidget scroll-position persistence across
// conversation and workspace switches. Requires QApplication (QWidget subclass).
//
// The decisive bug these guard against: switching workspaces leaves the chat via
// setSession(), which used to clear the list WITHOUT snapshotting the loaded
// messages. When the user had scrolled up, the view held paginated *older*
// messages that aren't in the plain (no-cursor) history page, so on return the
// saved scroll anchor couldn't be found and the list fell back to the bottom.
// setSession() must cache the loaded messages first, exactly like
// openConversation() does — so the anchor message survives the round-trip.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextDocument>

#include "ui/message_list/message_list.h"
#include "session/session.h"
#include "backend/backend.h"
#include "backend/domain.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-message-list");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── StubBackend ───────────────────────────────────────────────────────────────
// Minimal controllable backend. loadHistory returns _historyPage synchronously
// (rpl::variable fires on subscription), modelling the no-cursor history fetch.

struct StubBackend : Backend {
    rpl::variable<AuthState>                 _authState{AuthState::LoggedIn};
    rpl::variable<UserId>                    _meId;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;

    // The page returned by a no-cursor loadHistory (the recent tail).
    std::vector<Message>   _historyPage;
    std::optional<QString> _olderCursor;

    rpl::producer<AuthState> authState() const override { return _authState.value(); }
    Capabilities             capabilities() const override { return {}; }
    void                     connectRealtime() override {}
    void                     disconnectRealtime() override {}

    rpl::producer<UserId>                    loadMe() override { return _meId.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers() override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }

    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString> cursor) override {
        // A cursored (older) fetch returns nothing here — the test models the
        // older messages as already loaded into the view, not the cache.
        if (cursor.has_value())
            return rpl::variable<MessagePage>(MessagePage{}).value();
        return rpl::variable<MessagePage>(MessagePage{_historyPage, _olderCursor}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }

    void sendMessage(ConversationId, OutgoingMessage) override {}
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override {}

    void uploadFiles(
        ConversationId,
        const QStringList &,
        const QString &,
        std::function<void(bool, QString)> = {}
    ) override {}
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)> = {}
    ) override {}

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }

    rpl::producer<Event> events() const override { return _events.events(); }
};

// ── Helpers ─────────────────────────────────────────────────────────────────

static Message makeMessage(const QString &ts, const QString &text) {
    return Message{
        .ts     = ts,
        .date   = decimalTsToMicros(ts),
        .author = UserId{"U1"},
        .text   = TextWithEntities{text, {}},
    };
}

static bool containsTs(const std::vector<Message> &msgs, const QString &ts) {
    for (const auto &m : msgs)
        if (m.ts == ts)
            return true;
    return false;
}

static const Conversation kConv = {
    .id       = ConversationId{"C1"},
    .kind     = ConvKind::PublicChannel,
    .name     = "general",
    .isMember = true,
    .lastRead = "0",
};

struct Fixture {
    QTemporaryDir            tempDir;
    StubBackend             *stub = nullptr;
    std::unique_ptr<Session> session;

    Fixture() {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());
        auto backend = std::make_unique<StubBackend>();
        stub         = backend.get();
        stub->_meId  = UserId{"U1"};
        stub->_convs = std::vector<Conversation>{kConv};
        stub->_users = std::vector<User>{
            {.id          = UserId{"U1"},
             .name        = "me",
             .displayName = "Me",
             .isBot       = false,
             .isActive    = true}
        };
        session = std::make_unique<Session>(std::move(backend), "T_MSGLIST_TEST");
        session->start();
    }
    ~Fixture() {
        session.reset();
        QDir(tempDir.path()).removeRecursively();
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE(
    "setSession snapshots scrolled-up older messages so the anchor survives a workspace switch",
    "[message_list][scroll]"
) {
    Fixture f;

    // Five messages exist; the user scrolled up so all five are loaded into the
    // view, but the no-cursor history fetch only returns the recent tail (m4,m5)
    // — the older three were brought in by pagination, which doesn't cache.
    const std::vector<Message> all = {
        makeMessage("1000.000001", "one"),
        makeMessage("1000.000002", "two"),
        makeMessage("1000.000003", "three"),
        makeMessage("1000.000004", "four"),
        makeMessage("1000.000005", "five"),
    };
    const std::vector<Message> recentTail = {all[3], all[4]};

    // Pre-seed the cache with the full loaded view (as it would be after the
    // openConversation that originally paginated), then point the backend's
    // no-cursor fetch at just the recent tail.
    f.session->cacheMessages(kConv.id, all);
    f.stub->_historyPage = recentTail;

    MessageListWidget list(f.session.get(), /*imgCache*/ nullptr);
    list.openConversation(kConv.id);

    // The authoritative no-cursor fetch replaced the cache with just the tail —
    // this is the precondition that used to lose the scrolled-up anchor.
    const auto afterOpen = f.session->cachedMessages(kConv.id);
    REQUIRE(afterOpen.size() == 2);
    REQUIRE_FALSE(containsTs(afterOpen, "1000.000001"));

    // Leave the workspace: setSession must snapshot the loaded view (all five)
    // back into the cache before clearing.
    list.setSession(nullptr);

    const auto afterSwitch = f.session->cachedMessages(kConv.id);
    CHECK(afterSwitch.size() == 5);
    CHECK(containsTs(afterSwitch, "1000.000001")); // the scrolled-up anchor target
    CHECK(containsTs(afterSwitch, "1000.000003"));
    CHECK(containsTs(afterSwitch, "1000.000005"));
}

TEST_CASE(
    "setSession with no conversation open does not touch the cache", "[message_list][scroll]"
) {
    Fixture                    f;
    const std::vector<Message> seeded = {makeMessage("2000.000001", "hi")};
    f.session->cacheMessages(kConv.id, seeded);

    MessageListWidget list(f.session.get(), nullptr);
    // No openConversation — nothing loaded. setSession must be a safe no-op for
    // the cache (and must not crash on a null session).
    list.setSession(nullptr);

    CHECK(f.session->cachedMessages(kConv.id).size() == 1);
}
