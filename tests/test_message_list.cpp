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
        std::optional<Ts>                  = std::nullopt,
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

// Snapshot the live view back through the cache: setSession(nullptr) writes
// _items to the cache, so what comes back is exactly what the widget is showing.
static std::vector<Message>
liveView(MessageListWidget &list, Session *session, const ConversationId &conv) {
    list.setSession(nullptr);
    return session->cachedMessages(conv);
}

TEST_CASE("reopen clears a middle message deleted from another client", "[message_list][delete]") {
    Fixture f;

    // The cache (shown instantly on open) still holds a message that was deleted
    // from another client; the realtime message_deleted never arrived. The
    // authoritative head fetch is the same set MINUS the deleted one.
    const std::vector<Message> cached = {
        makeMessage("1000.000001", "one"),
        makeMessage("1000.000002", "two (deleted elsewhere)"),
        makeMessage("1000.000003", "three"),
    };
    f.session->cacheMessages(kConv.id, cached);
    f.stub->_historyPage = {cached[0], cached[2]}; // m2 gone

    MessageListWidget list(f.session.get(), /*imgCache*/ nullptr);
    list.openConversation(kConv.id);

    const auto view = liveView(list, f.session.get(), kConv.id);
    CHECK(view.size() == 2);
    CHECK(containsTs(view, "1000.000001"));
    CHECK_FALSE(containsTs(view, "1000.000002")); // deleted-elsewhere row cleared
    CHECK(containsTs(view, "1000.000003"));
}

TEST_CASE(
    "reopen clears the NEWEST message deleted from another client", "[message_list][delete]"
) {
    Fixture f;

    // The decisive case: you delete the most recent message in another client.
    // Its ts is newer than everything that remains, so an upper-bounded merge
    // window left it stuck forever. The head page is authoritative above its
    // newest, so the stale newest row must be dropped.
    const std::vector<Message> cached = {
        makeMessage("1000.000001", "one"),
        makeMessage("1000.000002", "two"),
        makeMessage("1000.000003", "three (newest, deleted elsewhere)"),
    };
    f.session->cacheMessages(kConv.id, cached);
    f.stub->_historyPage = {cached[0], cached[1]}; // newest gone

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    const auto view = liveView(list, f.session.get(), kConv.id);
    CHECK(view.size() == 2);
    CHECK(containsTs(view, "1000.000001"));
    CHECK(containsTs(view, "1000.000002"));
    CHECK_FALSE(containsTs(view, "1000.000003")); // deleted newest row cleared
}

TEST_CASE(
    "reopen clears the only message when it was deleted from another client",
    "[message_list][delete]"
) {
    Fixture f;

    // Deleting the sole message empties the channel. The head fetch returns an
    // empty page (a successful fetch, not an error — those don't fire on_next),
    // which must clear the stale cached row rather than leave it on screen.
    f.session->cacheMessages(kConv.id, {makeMessage("1000.000001", "the only message")});
    f.stub->_historyPage = {}; // server now has nothing

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    const auto view = liveView(list, f.session.get(), kConv.id);
    CHECK(view.empty());
}

TEST_CASE(
    "textOnly edit echo replaces stale rich_text blocks and keeps reactions", "[message_list][edit]"
) {
    Fixture f;

    // The row as delivered by the echo of the original send: Slack attaches a
    // server-generated rich_text block, which the renderer prefers over the
    // text field — a merge that only swaps text/rawText keeps showing the
    // stale block's old text (while "(edited)" updates, painted separately).
    Message orig = makeMessage("1000.000001", "old text");
    Block   rt;
    rt.typeStr = "rich_text";
    rt.text    = TextWithEntities{"old text", {}};
    orig.blocks.push_back(rt);
    orig.reactions.push_back({"thumbsup", 1, {UserId{"U2"}}});
    f.session->cacheMessages(kConv.id, {orig});
    f.stub->_historyPage = {orig};

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    // chat.update response echo: sparse message (new text, no blocks), textOnly.
    Message sparse = makeMessage("1000.000001", "new text");
    sparse.rawText = "new text";
    sparse.edited  = true;
    f.stub->_events.fire(Event{EvMessageChanged{kConv.id, sparse, /*textOnly=*/true}});

    const auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 1);
    CHECK(view[0].text.text == "new text");
    CHECK(view[0].blocks.empty()); // stale rich_text dropped → doc renders the new text
    CHECK(view[0].edited);
    REQUIRE(view[0].reactions.size() == 1); // merge keeps the row's reactions
    CHECK(view[0].reactions[0].name == "thumbsup");
}

TEST_CASE(
    "live realtime delete removes the row from the open conversation", "[message_list][delete]"
) {
    Fixture f;

    const std::vector<Message> msgs = {
        makeMessage("1000.000001", "one"),
        makeMessage("1000.000002", "two"),
        makeMessage("1000.000003", "three (deleted live)"),
    };
    f.stub->_historyPage = msgs;

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    // The socket delivers message_deleted for the newest message; it flows through
    // the session to the list and the row must vanish immediately.
    f.stub->_events.fire(Event{EvMessageDeleted{kConv.id, "1000.000003", std::nullopt}});

    const auto view = liveView(list, f.session.get(), kConv.id);
    CHECK(view.size() == 2);
    CHECK_FALSE(containsTs(view, "1000.000003"));
}

TEST_CASE(
    "mergeNetworkMessages keeps paginated-older messages below the page window",
    "[message_list][delete]"
) {
    Fixture f;

    // Same five-message scrolled-up scenario as the anchor test: m1..m3 were
    // paginated in (below the no-cursor tail). The tail fetch returning only
    // m4,m5 must NOT be read as "m1..m3 were deleted" — they're simply older
    // than the window this page covers.
    const std::vector<Message> all = {
        makeMessage("1000.000001", "one"),
        makeMessage("1000.000002", "two"),
        makeMessage("1000.000003", "three"),
        makeMessage("1000.000004", "four"),
        makeMessage("1000.000005", "five"),
    };
    f.session->cacheMessages(kConv.id, all);
    f.stub->_historyPage = {all[3], all[4]}; // recent tail only

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    list.setSession(nullptr);
    const auto view = f.session->cachedMessages(kConv.id);
    CHECK(view.size() == 5); // nothing erased — the older three are out of window
    CHECK(containsTs(view, "1000.000001"));
    CHECK(containsTs(view, "1000.000005"));
}

static const Reaction *
findReaction(const std::vector<Message> &msgs, const QString &ts, const QString &name) {
    for (const auto &m : msgs)
        if (m.ts == ts)
            for (const auto &r : m.reactions)
                if (r.name == name)
                    return &r;
    return nullptr;
}

TEST_CASE(
    "duplicate reaction_added echo for the same user is not counted twice",
    "[message_list][reactions]"
) {
    Fixture f;

    // The optimistic local add and the Socket Mode echo (or an envelope
    // redelivery) both report the same (user, emoji) — the pill must show 1,
    // not briefly 2.
    f.stub->_historyPage = {makeMessage("1000.000001", "nice")};

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    f.stub->_events.fire(Event{EvReactionAdded{kConv.id, "1000.000001", "+1", UserId{"U1"}}});
    f.stub->_events.fire(Event{EvReactionAdded{kConv.id, "1000.000001", "+1", UserId{"U1"}}});

    const auto  view = liveView(list, f.session.get(), kConv.id);
    const auto *r    = findReaction(view, "1000.000001", "+1");
    REQUIRE(r != nullptr);
    CHECK(r->count == 1);
    CHECK(r->users == std::vector<UserId>{UserId{"U1"}});
}

TEST_CASE(
    "duplicate reaction_removed echo does not double-decrement others' reactions",
    "[message_list][reactions]"
) {
    Fixture f;

    auto msg             = makeMessage("1000.000001", "nice");
    msg.reactions        = {{"+1", 2, {UserId{"U1"}, UserId{"U2"}}}};
    f.stub->_historyPage = {msg};

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    // U1 un-reacts; the echo arrives twice (optimistic remove + socket echo).
    // U2's like must survive.
    f.stub->_events.fire(Event{EvReactionRemoved{kConv.id, "1000.000001", "+1", UserId{"U1"}}});
    f.stub->_events.fire(Event{EvReactionRemoved{kConv.id, "1000.000001", "+1", UserId{"U1"}}});

    const auto  view = liveView(list, f.session.get(), kConv.id);
    const auto *r    = findReaction(view, "1000.000001", "+1");
    REQUIRE(r != nullptr);
    CHECK(r->count == 1);
    CHECK(r->users == std::vector<UserId>{UserId{"U2"}});
}

TEST_CASE(
    "reaction_removed still decrements when the user list is truncated", "[message_list][reactions]"
) {
    Fixture f;

    // History reactions can carry count > users.size() (Slack truncates the
    // users array). A removal by an unlisted user must still drop the count.
    auto msg             = makeMessage("1000.000001", "popular");
    msg.reactions        = {{"+1", 3, {UserId{"U1"}}}};
    f.stub->_historyPage = {msg};

    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    f.stub->_events.fire(Event{EvReactionRemoved{kConv.id, "1000.000001", "+1", UserId{"U9"}}});

    const auto  view = liveView(list, f.session.get(), kConv.id);
    const auto *r    = findReaction(view, "1000.000001", "+1");
    REQUIRE(r != nullptr);
    CHECK(r->count == 2);
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
