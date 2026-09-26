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

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"
#include "stub_backend.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>

#include "text/mrkdwn_parser.h"
#include "ui/message_list/message_list.h"
#include "media/audio_player.h"
#include "util/slack_links.h"
#include "session/session.h"
#include "backend/backend.h"
#include "backend/domain.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"
#include "ui/image_cache.h"
#include "ui/theme_manager.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-message-list");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

namespace {

// ── StubBackend ───────────────────────────────────────────────────────────────
// Minimal controllable backend. loadHistory returns _historyPage synchronously
// (rpl::variable fires on subscription), modelling the no-cursor history fetch.

struct StubBackend : msga_test::StubBackendBase {
    // The page returned by a no-cursor loadHistory (the recent tail).
    std::vector<Message>   _historyPage;
    std::vector<Message>   _threadPage;
    std::optional<QString> _olderCursor;

    // When set, a no-cursor loadHistory answers nothing until deliverHistory()
    // is called — the "conversation opened, its first page still in flight"
    // window that a plain rpl::variable (which fires on subscription) can't model.
    bool                           _deferHistory = false;
    rpl::event_stream<MessagePage> _historyStream;

    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString> cursor) override {
        // A cursored (older) fetch returns nothing here — the test models the
        // older messages as already loaded into the view, not the cache.
        if (cursor.has_value())
            return rpl::variable<MessagePage>(MessagePage{}).value();
        if (_deferHistory)
            return _historyStream.events();
        return rpl::variable<MessagePage>(MessagePage{_historyPage, _olderCursor}).value();
    }
    void deliverHistory() { _historyStream.fire(MessagePage{_historyPage, _olderCursor}); }
    bool _deferThread = false;
    rpl::event_stream<MessagePage> _threadStream;
    rpl::producer<MessagePage>     loadThread(ConversationId, Ts, std::optional<QString>) override {
        if (_deferThread)
            return _threadStream.events();
        return rpl::variable<MessagePage>(MessagePage{_threadPage, std::nullopt}).value();
    }

    // deleteAttachment calls with their outcome callbacks: the test plays the
    // server, so the list's optimistic hide and the confirmation can be told apart.
    struct AttachmentDelete {
        ConversationId                     conv;
        Ts                                 ts;
        int                                id;
        std::function<void(bool, QString)> done;
    };
    std::vector<AttachmentDelete> attachmentDeletes;
    void                          deleteAttachment(
        ConversationId c, Ts ts, int id, std::function<void(bool, QString)> done
    ) override {
        attachmentDeletes.push_back({c, ts, id, std::move(done)});
    }
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

} // namespace

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

TEST_CASE("threadRoots lists only loaded roots, newest first", "[message_list][move]") {
    Fixture f;

    Message root1        = makeMessage("1000.000001", "first thread");
    root1.replyCount     = 2;
    Message reply        = makeMessage("1000.000002", "a reply");
    reply.threadRoot     = Ts{"1000.000001"};
    Message plain        = makeMessage("1000.000003", "no thread");
    Message broadcast    = makeMessage("1000.000004", "also sent to channel");
    broadcast.threadRoot = Ts{"1000.000001"};
    broadcast.subtype    = "thread_broadcast";
    Message root2        = makeMessage("1000.000005", "second thread");
    root2.replyCount     = 1;

    f.stub->_historyPage = {root1, reply, plain, broadcast, root2};
    MessageListWidget list(f.session.get(), /*imgCache*/ nullptr);
    list.openConversation(kConv.id);

    const auto roots = list.threadRoots();
    REQUIRE(roots.size() == 2);
    CHECK(roots[0].ts == "1000.000005"); // newest first
    CHECK(roots[1].ts == "1000.000001");
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

// ── Message links ─────────────────────────────────────────────────────────────

// Permalink to the message posted at `ts` in the fixture's channel.
static QString permalinkTo(const QString &ts) {
    QString digits = ts;
    digits.remove('.');
    return "https://cityteam.slack.com/archives/" + kConv.id.value + "/p" + digits;
}

static Message makeMrkdwnMessage(const QString &ts, const QString &mrkdwn) {
    Message m = makeMessage(ts, {});
    m.text    = MrkdwnParser::parse(mrkdwn);
    return m;
}

// Let queued work (the post-load QTimer::singleShot) and the 220 ms scroll
// animation run to completion.
static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Press every point of a coarse grid over the viewport until `hit` turns true.
// The chip's rectangle depends on font metrics and wrapping, so it can't be
// computed here — but if no point in the whole message area triggers it, the
// chip isn't clickable, which is exactly the regression to catch.
static void pressUntil(MessageListWidget &list, const bool &hit) {
    QWidget *vp = list.viewport();
    for (int y = 0; y < vp->height() && !hit; y += 3) {
        for (int x = 0; x < vp->width() && !hit; x += 3) {
            const QPointF p(x, y);
            QMouseEvent   ev(
                QEvent::MouseButtonPress,
                p,
                p,
                vp->mapToGlobal(p),
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier
            );
            QApplication::sendEvent(vp, &ev);
        }
    }
}

TEST_CASE("clicking a link to a message asks the host to focus it", "[message_list][msglink]") {
    Fixture f;

    // The last message is nothing but a permalink to the first one — Slack shows
    // that as a chip, and clicking it must navigate rather than open a browser.
    f.stub->_historyPage = {
        makeMessage("1000.000001", "the message being linked to"),
        makeMessage("1000.000002", "something else"),
        makeMrkdwnMessage("1000.000003", "look: <" + permalinkTo("1000.000001") + ">"),
    };

    MessageListWidget list(f.session.get(), nullptr);
    list.resize(500, 240);
    list.openConversation(kConv.id);

    bool           hit = false;
    ConversationId gotConv;
    Ts             gotTs, gotThread;
    QObject::connect(
        &list,
        &MessageListWidget::messageLinkRequested,
        [&](ConversationId conv, Ts ts, Ts threadTs) {
            hit       = true;
            gotConv   = conv;
            gotTs     = ts;
            gotThread = threadTs;
        }
    );

    pressUntil(list, hit);

    REQUIRE(hit);
    CHECK(gotConv == kConv.id);
    CHECK(gotTs == "1000.000001");
    CHECK(gotThread.isEmpty());
}

TEST_CASE("a link to a thread reply carries its thread root", "[message_list][msglink]") {
    Fixture f;

    const QString reply =
        permalinkTo("1000.000009") + "?thread_ts=1000.000001&cid=" + kConv.id.value;
    f.stub->_historyPage = {
        makeMessage("1000.000001", "thread root"),
        makeMrkdwnMessage("1000.000003", "<" + reply + ">"),
    };

    MessageListWidget list(f.session.get(), nullptr);
    list.resize(500, 240);
    list.openConversation(kConv.id);

    bool hit = false;
    Ts   gotTs, gotThread;
    QObject::connect(
        &list, &MessageListWidget::messageLinkRequested, [&](ConversationId, Ts ts, Ts threadTs) {
            hit       = true;
            gotTs     = ts;
            gotThread = threadTs;
        }
    );

    pressUntil(list, hit);

    REQUIRE(hit);
    CHECK(gotTs == "1000.000009");
    // Without the root the host would open the channel, where a reply isn't
    // shown at all (conversations.history omits replies).
    CHECK(gotThread == "1000.000001");
}

TEST_CASE("a jump issued before the history arrives still lands", "[message_list][msglink]") {
    Fixture f;

    // 30 messages so the conversation is taller than the viewport.
    std::vector<Message> msgs;
    for (int i = 1; i <= 30; ++i)
        msgs.push_back(
            makeMessage(QString("1000.0000%1").arg(i, 2, 10, QChar('0')), QString("m%1").arg(i))
        );
    f.stub->_historyPage  = msgs;
    f.stub->_deferHistory = true;

    // Control: no jump — the conversation opens at the bottom.
    MessageListWidget plain(f.session.get(), nullptr);
    plain.resize(500, 200);
    plain.openConversation(kConv.id);
    f.stub->deliverHistory();
    spin(400);
    REQUIRE(plain.verticalScrollBar()->maximum() > 0); // content does overflow
    CHECK(plain.verticalScrollBar()->value() == plain.verticalScrollBar()->maximum());

    // The click case: the conversation was opened by the click itself, so the
    // jump target isn't loaded yet. It must be honoured once the page lands.
    MessageListWidget list(f.session.get(), nullptr);
    list.resize(500, 200);
    list.openConversation(kConv.id);
    list.jumpToTs("1000.000010");
    f.stub->deliverHistory();
    spin(400);

    const int v = list.verticalScrollBar()->value();
    CHECK(v > 0);                                   // not the top
    CHECK(v < list.verticalScrollBar()->maximum()); // and not the bottom
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

// ── Inline audio player ───────────────────────────────────────────────────────

TEST_CASE(
    "clicking an audio chip starts fetching it into the player, not the browser",
    "[message_list][audio]"
) {
    Fixture f;
    Message m = makeMessage("1000.000001", "listen to this");
    File    audio;
    audio.id         = "F_AUDIO";
    audio.name       = "sample-5s.mp3";
    audio.mimeType   = "audio/mpeg";
    audio.prettyType = "MP3";
    audio.size       = 80000;
    audio.durationMs = 5000;
    audio.urlPrivate = "https://files.slack.com/files-pri/T1-F_AUDIO/sample-5s.mp3";
    m.files.push_back(audio);
    f.stub->_historyPage = {m};

    auto &player = Media::AudioPlayer::instance();
    player.stop();

    MessageListWidget list(f.session.get(), nullptr);
    list.resize(500, 240);
    list.openConversation(kConv.id);

    // Sweep presses across the viewport until the player picks the file up. The
    // stub backend's downloadFile never answers, so it stays in Loading — which
    // is exactly the state a click on a not-yet-cached file must produce.
    bool hit = false;
    QObject::connect(&player, &Media::AudioPlayer::statusChanged, &list, [&](const QString &k) {
        if (k == "F_AUDIO")
            hit = true;
    });
    pressUntil(list, hit);

    REQUIRE(hit);
    CHECK(player.status().key == "F_AUDIO");
    CHECK(player.status().state == Media::AudioPlayer::State::Loading);
    CHECK(player.status().durationMs == 5000); // Slack's duration shown while fetching

    // The chip's paint state mirrors the player.
    const auto st = list.audioChipState(audio);
    REQUIRE(st.has_value());
    CHECK(st->phase == MsgRender::AudioChipState::Phase::Loading);
    CHECK(st->durationMs == 5000);
    player.stop();
}

// ── Text selection ────────────────────────────────────────────────────────────
// A selection spanning several messages used to set the end of every fully
// covered row to characterCount(), one past the last valid cursor position.
// QTextCursor rejected it ("Position 'N' out of range"), left the cursor at 0,
// and the row painted no highlight and copied as nothing — only the last row of
// a multi-message selection ever reached the clipboard.

static int  g_outOfRangeWarnings = 0;
static void countOutOfRange(QtMsgType, const QMessageLogContext &, const QString &msg) {
    if (msg.contains("out of range"))
        ++g_outOfRangeWarnings;
}

static void sendMouse(QWidget *w, QEvent::Type type, const QPointF &p, Qt::MouseButton btn) {
    const Qt::MouseButtons held =
        (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent ev(type, p, p, w->mapToGlobal(p), btn, held, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

TEST_CASE("a selection across messages copies and paints every row", "[message_list][selection]") {
    Fixture f;
    f.stub->_historyPage = {
        makeMessage("1000.000001", "first alpha"),
        makeMessage("1000.000002", "second bravo"),
        makeMessage("1000.000003", "third charlie"),
    };

    MessageListWidget list(f.session.get(), nullptr);
    list.resize(500, 300);
    list.openConversation(kConv.id);
    spin(50);

    QWidget  *vp = list.viewport();
    const int w = vp->width(), h = vp->height();

    // Press on the first text band from the top (x=0 clamps to the start of the
    // line), then sweep the pointer down the right edge: moves off text are
    // ignored, so the focus ends at the end of the last line of the last message.
    QApplication::clipboard()->setText("sentinel");
    for (int ay = 0; ay < h && QApplication::clipboard()->text() == "sentinel"; ay += 2) {
        sendMouse(vp, QEvent::MouseButtonPress, QPointF(0, ay), Qt::LeftButton);
        for (int fy = 0; fy < h; fy += 2)
            sendMouse(vp, QEvent::MouseMove, QPointF(w - 1, fy), Qt::NoButton);
        sendMouse(vp, QEvent::MouseButtonRelease, QPointF(w - 1, h - 1), Qt::LeftButton);
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, "c");
        QApplication::sendEvent(&list, &copy);
    }

    const QString copied = QApplication::clipboard()->text();
    REQUIRE(copied != "sentinel");
    CHECK(copied.contains("first alpha"));
    CHECK(copied.contains("second bravo"));
    CHECK(copied.contains("third charlie"));

    // Painting the highlighted rows must not trip QTextCursor either.
    g_outOfRangeWarnings = 0;
    auto *prev           = qInstallMessageHandler(countOutOfRange);
    (void)list.grab();
    qInstallMessageHandler(prev);
    CHECK(g_outOfRangeWarnings == 0);
}

TEST_CASE(
    "disabled link previews skip image loading and restore live", "[message_list][previews]"
) {
    Fixture     f;
    QStringList requested;
    QImage      image(40, 30, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    QByteArray png;
    QBuffer    buffer(&png);
    REQUIRE(buffer.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&buffer, "PNG"));
    ImageCache cache;
    // All cache fetches, including sizeOf() during layout, consult this loader
    // before the network. Supply bytes so this test needs no external service.
    cache.setDiskCache(
        [&](const QString &url) {
            requested.append(url);
            return png;
        },
        {}
    );

    auto message        = makeMrkdwnMessage("1000.000001", "<https://example.com/article|Article>");
    message.attachments = {
        Attachment{
            .title      = "Web preview",
            .titleLink  = "https://example.com/article",
            .text       = TextWithEntities{QString("Preview details\n").repeated(12), {}},
            .imageUrl   = "https://example.com/preview.png",
            .faviconUrl = "https://example.com/favicon.png",
            .footerIcon = "https://example.com/footer.png",
            .blocks     = {Block{.typeStr = "image", .imageUrl = "https://example.com/block.png"}},
            .isLinkPreview = true,
        },
        Attachment{
            .title         = "Linear issue",
            .imageUrl      = "https://example.com/linear.png",
            .isLinkPreview = true
        },
        Attachment{
            .fallback      = "Dancing GIF",
            .imageUrl      = "https://media.giphy.com/media/abc123/giphy.gif",
            .isLinkPreview = true,
        },
        Attachment{
            .fallback      = "Photo",
            .imageUrl      = "https://images.example.com/opaque-cdn-resource",
            .isLinkPreview = true,
        },
        Attachment{.title = "Bot content", .imageUrl = "https://example.com/bot.png"},
        Attachment{
            .text        = TextWithEntities{"Shared Slack message", {}},
            .isMsgUnfurl = true,
            .authorIcon  = "https://example.com/author.png",
        },
    };
    message.files        = {File{.name = "notes.txt", .mimeType = "text/plain"}};
    f.stub->_historyPage = {message};
    f.stub->_threadPage  = {message};

    MessageListWidget list(f.session.get(), &cache);
    list.resize(500, 160);
    list.setLinkPreviewsEnabled(false);
    SECTION("conversation") {
        list.openConversation(kConv.id);
    }
    SECTION("standalone thread") {
        list.openThread(kConv.id, message.ts);
    }
    list.show();
    spin(300);
    list.viewport()->grab();

    const auto checkNoPreviews = [&] {
        CHECK_FALSE(requested.contains("https://example.com/preview.png"));
        CHECK_FALSE(requested.contains("https://example.com/favicon.png"));
        CHECK_FALSE(requested.contains("https://example.com/footer.png"));
        CHECK_FALSE(requested.contains("https://example.com/block.png"));
        CHECK_FALSE(requested.contains("https://example.com/linear.png"));
        CHECK_FALSE(requested.contains("https://example.com/author.png"));
    };
    checkNoPreviews();
    CHECK(requested.contains("https://media.giphy.com/media/abc123/giphy.gif"));
    CHECK(requested.contains("https://images.example.com/opaque-cdn-resource"));
    CHECK(requested.contains("https://example.com/bot.png"));
    CHECK_FALSE(requested.contains("https://example.com/author.png"));
    const int hiddenHeight = list.verticalScrollBar()->maximum();
    list.setLinkPreviewsEnabled(true);
    spin(300);
    list.verticalScrollBar()->setValue(0);
    list.viewport()->grab();
    CHECK(requested.contains("https://example.com/preview.png"));
    CHECK(requested.contains("https://example.com/favicon.png"));
    CHECK(requested.contains("https://example.com/footer.png"));
    CHECK(requested.contains("https://example.com/block.png"));
    CHECK(list.verticalScrollBar()->maximum() > hiddenHeight);

    list.setLinkPreviewsEnabled(false);
    spin(300);
    list.viewport()->grab();
    CHECK(list.verticalScrollBar()->maximum() == hiddenHeight);
    // Hiding previews must never strip them or uploaded files from the message
    // model: re-enabling works without fetching history again.
    const auto stored = list.lastOwnMessage(UserId{"U1"});
    REQUIRE(stored.has_value());
    CHECK(stored->attachments == message.attachments);
    CHECK(stored->files == message.files);
    CHECK(stored->text == message.text);
}

TEST_CASE("first history load merges racing live messages exactly once", "[message_list][race]") {
    Fixture f;
    f.stub->_deferHistory = true;
    auto root             = makeMessage("1000.000001", "same text");
    auto newer            = makeMessage("1000.000002", "same text");
    SECTION("without cache") {}
    SECTION("with duplicate cached rows") {
        f.session->cacheMessages(kConv.id, {root, root});
    }
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    f.stub->_events.fire(EvMessageNew{kConv.id, root});
    f.stub->_events.fire(EvMessageNew{kConv.id, newer});
    root.replyCount      = 3;
    root.latestReply     = "1000.000004";
    f.stub->_historyPage = {root, root}; // duplicate page, newer live row absent
    f.stub->deliverHistory();
    auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 2);
    CHECK(view[0].ts == root.ts);
    CHECK(view[0].replyCount == 3);
    CHECK(view[1].ts == newer.ts); // identical text is still a distinct message
}

TEST_CASE("history does not undo concurrent edits or deletions", "[message_list][race]") {
    Fixture f;
    auto    message = makeMessage("1000.000001", "old text");
    f.session->cacheMessages(kConv.id, {message});
    f.stub->_deferHistory = true;
    f.stub->_historyPage  = {message};
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    bool deleted = false;
    SECTION("edited while loading") {
        message.text.text = "edited text";
        f.stub->_events.fire(EvMessageChanged{kConv.id, message});
    }
    SECTION("deleted while loading") {
        deleted = true;
        f.stub->_events.fire(EvMessageDeleted{kConv.id, message.ts});
    }
    f.stub->deliverHistory();
    auto view = liveView(list, f.session.get(), kConv.id);
    if (deleted) {
        CHECK(view.empty());
    } else {
        REQUIRE(view.size() == 1);
        CHECK(view[0].text.text == "edited text");
    }
}

TEST_CASE("an empty first history response preserves concurrent messages", "[message_list][race]") {
    Fixture f;
    f.stub->_deferHistory = true;
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    const auto message = makeMessage("1000.000001", "arrived after request");
    f.stub->_events.fire(EvMessageNew{kConv.id, message});
    f.stub->deliverHistory();
    auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 1);
    CHECK(view[0].ts == message.ts);
}

TEST_CASE("thread first load merges overlapping replies and live edits", "[message_list][race]") {
    Fixture f;
    f.stub->_deferThread = true;
    auto root            = makeMessage("1000.000001", "root");
    auto reply           = makeMessage("1000.000002", "reply");
    reply.threadRoot     = root.ts;
    MessageListWidget list(f.session.get(), nullptr);
    list.openThread(kConv.id, root.ts);
    f.stub->_events.fire(EvMessageNew{kConv.id, reply});
    f.stub->_threadStream.fire(MessagePage{{root, reply, reply}, std::nullopt});
    // Deleting once must remove the only copy, not expose a duplicate underneath.
    f.stub->_events.fire(EvMessageDeleted{kConv.id, reply.ts, root.ts});
    const auto last = list.lastOwnMessage(UserId{"U1"});
    REQUIRE(last.has_value());
    CHECK(last->ts == root.ts);
}

TEST_CASE("live reply before channel history does not hide its root", "[message_list][race]") {
    Fixture f;
    f.stub->_deferHistory = true;
    auto root             = makeMessage("1000.000001", "root");
    root.replyCount       = 1;
    auto reply            = makeMessage("1000.000002", "reply");
    reply.threadRoot      = root.ts;
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    f.stub->_events.fire(EvMessageNew{kConv.id, reply});
    f.stub->_historyPage = {root};
    f.stub->deliverHistory();
    auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 1);
    CHECK(view[0].ts == root.ts);
    CHECK(view[0].replyCount == 1);
}

TEST_CASE("first history response retains richer live thread metadata", "[message_list][race]") {
    Fixture f;
    f.stub->_deferHistory = true;
    auto root             = makeMessage("1000.000001", "root");
    f.stub->_historyPage  = {root};
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    root.replyCount  = 3;
    root.latestReply = "1000.000004";
    f.stub->_events.fire(EvMessageNew{kConv.id, root});
    f.stub->deliverHistory();
    auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 1);
    CHECK(view[0].replyCount == 3);
    CHECK(view[0].latestReply == root.latestReply);
}

TEST_CASE(
    "a live thread broadcast gets a channel row as well as a count", "[message_list][thread]"
) {
    Fixture f;
    auto    root         = makeMessage("1000.000001", "root");
    f.stub->_historyPage = {root};
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    auto reply           = makeMessage("1000.000002", "thread only");
    reply.threadRoot     = root.ts;
    auto broadcast       = makeMessage("1000.000003", "also sent to channel");
    broadcast.threadRoot = root.ts;
    broadcast.subtype    = "thread_broadcast";
    f.stub->_events.fire(EvMessageNew{kConv.id, reply});
    f.stub->_events.fire(EvMessageNew{kConv.id, broadcast});

    auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 2);
    CHECK(view[0].ts == root.ts);
    CHECK(view[0].replyCount == 2);
    CHECK(view[1].ts == broadcast.ts);
}

TEST_CASE("a thread broadcast already loaded is not counted again", "[message_list][thread]") {
    // The history page's root count already includes the broadcast it carries;
    // a live copy racing that load refreshes the row and leaves the count be.
    Fixture f;
    auto    root         = makeMessage("1000.000001", "root");
    root.replyCount      = 1;
    auto broadcast       = makeMessage("1000.000002", "also sent to channel");
    broadcast.threadRoot = root.ts;
    broadcast.subtype    = "thread_broadcast";
    f.stub->_historyPage = {root, broadcast};
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);

    f.stub->_events.fire(EvMessageNew{kConv.id, broadcast});

    auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 2);
    CHECK(view[0].replyCount == 1);
    CHECK(view[1].ts == broadcast.ts);
}

TEST_CASE("delayed periodic history preserves live arrivals", "[message_list][race]") {
    Fixture    f;
    const auto old       = makeMessage("1000.000001", "old");
    const auto live      = makeMessage("1000.000002", "new");
    f.stub->_historyPage = {old};
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    const auto revision = f.session->nextMessageRevision(); // periodic request starts
    f.stub->_events.fire(EvMessageNew{kConv.id, live});
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {old}, revision});
    CHECK(f.session->cachedMessages(kConv.id).size() == 2);
    const auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 2);
    CHECK(view.back().ts == live.ts);
}

TEST_CASE("newer history wins over a delayed initial response", "[message_list][race]") {
    Fixture f;
    auto    old           = makeMessage("1000.000001", "old");
    auto    newer         = makeMessage("1000.000002", "new");
    f.stub->_historyPage  = {old};
    f.stub->_deferHistory = true;
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    const auto revision = f.session->nextMessageRevision();
    old.text.text       = "edited by newer history";
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {old, newer}, revision});
    f.stub->deliverHistory();
    auto cached = f.session->cachedMessages(kConv.id);
    REQUIRE(cached.size() == 2);
    CHECK(cached.front().text.text == old.text.text);
    const auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 2);
    CHECK(view.front().text.text == old.text.text);
    CHECK(view.back().ts == newer.ts);
}

TEST_CASE("stale history cannot resurrect a deletion from newer history", "[message_list][race]") {
    Fixture    f;
    const auto old        = makeMessage("1000.000001", "keep");
    const auto deleted    = makeMessage("1000.000002", "deleted");
    f.stub->_historyPage  = {old, deleted};
    f.stub->_deferHistory = true;
    bool emptyHead        = false;
    SECTION("nonempty head, deletion was not loaded locally") {}
    SECTION("nonempty head, deletion was already cached") {
        f.session->cacheMessages(kConv.id, {old, deleted});
    }
    SECTION("empty newer head") {
        emptyHead = true;
    }
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    const auto                 revision = f.session->nextMessageRevision();
    const std::vector<Message> head =
        emptyHead ? std::vector<Message>{} : std::vector<Message>{old};
    f.stub->_events.fire(EvHeadRefresh{kConv.id, head, revision});
    f.stub->deliverHistory();
    CHECK(f.session->cachedMessages(kConv.id).size() == head.size());
    const auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == head.size());
    CHECK_FALSE(containsTs(view, deleted.ts));
}

TEST_CASE("older responses retain scrollback outside the newer head", "[message_list][race]") {
    Fixture    f;
    const auto older      = makeMessage("1000.000001", "older page");
    const auto newer      = makeMessage("1000.000002", "newer head");
    f.stub->_historyPage  = {older};
    f.stub->_deferHistory = true;
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    const auto revision = f.session->nextMessageRevision();
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {newer}, revision});
    f.stub->deliverHistory();
    const auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 2);
    CHECK(view.front().ts == older.ts);
    CHECK(view.back().ts == newer.ts);
}

TEST_CASE("later requests can refresh rows changed by earlier responses", "[message_list][race]") {
    Fixture f;
    auto    message      = makeMessage("1000.000001", "original");
    f.stub->_historyPage = {message};
    MessageListWidget list(f.session.get(), nullptr);
    list.openConversation(kConv.id);
    const auto first  = f.session->nextMessageRevision();
    const auto second = f.session->nextMessageRevision();
    message.text.text = "first refresh";
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {message}, first});
    message.text.text = "second refresh";
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {message}, second});
    const auto view = liveView(list, f.session.get(), kConv.id);
    REQUIRE(view.size() == 1);
    CHECK(view.front().text.text == "second refresh");
}

// ── "Remove preview" (the × on an attachment) ─────────────────────────────────

namespace {

Message previewMessage(const UserId &author) {
    auto m        = makeMrkdwnMessage("1000.000001", "two links");
    m.author      = author;
    m.attachments = {
        Attachment{
            .id            = 1,
            .title         = "Alpha",
            .titleLink     = "https://example.com/alpha",
            .text          = TextWithEntities{"Alpha preview body", {}},
            .isLinkPreview = true,
        },
        Attachment{
            .id            = 2,
            .title         = "Bravo",
            .titleLink     = "https://example.com/bravo",
            .text          = TextWithEntities{"Bravo preview body", {}},
            .isLinkPreview = true,
        },
    };
    return m;
}

// Press (and release) every point of a grid over the left gutter of the row
// showing `ts` — where the × of a preview sits — top to bottom, until `hit`
// says the press landed. Top to bottom, so the FIRST card's × is met first.
template <typename Hit>
void pressGutterUntil(MessageListWidget &list, const Ts &ts, Hit hit) {
    QWidget    *vp  = list.viewport();
    const QRect row = list.rowViewportRect(ts);
    REQUIRE(!row.isEmpty());
    for (int y = row.top(); y <= row.bottom() && !hit(); y += 2) {
        for (int x = 0; x < 120 && !hit(); x += 2) {
            const QPointF p(x, y);
            sendMouse(vp, QEvent::MouseButtonPress, p, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton);
        }
    }
    REQUIRE(hit());
}

} // namespace

TEST_CASE(
    "the × on an own preview hides it at once and removes it server-side on confirmation",
    "[message_list][preview]"
) {
    Fixture f;
    f.stub->caps.removePreview = true;
    const auto message         = previewMessage(UserId{"U1"}); // me
    f.stub->_historyPage       = {message};

    MessageListWidget list(f.session.get(), nullptr);
    list.resize(600, 400);
    list.openConversation(kConv.id);
    list.show();
    spin(300);
    list.viewport()->grab();
    const int fullH = list.rowViewportRect(message.ts).height();

    pressGutterUntil(list, message.ts, [&] { return !f.stub->attachmentDeletes.empty(); });
    REQUIRE(f.stub->attachmentDeletes.size() == 1);
    CHECK(f.stub->attachmentDeletes[0].conv == kConv.id);
    CHECK(f.stub->attachmentDeletes[0].ts == message.ts);
    CHECK(f.stub->attachmentDeletes[0].id == 1); // the first card, Slack's positional id
    // Hidden right away — but only hidden: the model keeps both cards until the
    // server confirms, so a failure can't lose anything.
    list.viewport()->grab();
    const int oneCardH = list.rowViewportRect(message.ts).height();
    CHECK(oneCardH < fullH);
    REQUIRE(list.lastOwnMessage(UserId{"U1"})->attachments.size() == 2);

    SECTION("confirmed: the card is dropped and the rest renumbered, nothing else hidden") {
        f.stub->attachmentDeletes[0].done(true, {});
        list.viewport()->grab();
        const auto stored = list.lastOwnMessage(UserId{"U1"});
        REQUIRE(stored->attachments.size() == 1);
        CHECK(stored->attachments[0].title == "Bravo");
        CHECK(stored->attachments[0].id == 1); // mirrors Slack's renumbering for the next click
        // Bravo now sits at index 0 — the index the hide was keyed on. It must
        // still be visible: the row shows exactly one card, as right after the press.
        CHECK(list.rowViewportRect(message.ts).height() == oneCardH);
    }

    SECTION("the realtime echo landing first doesn't cost a second card") {
        auto echoed              = message;
        echoed.attachments       = {message.attachments[1]};
        echoed.attachments[0].id = 1;
        f.stub->_events.fire(Event{EvMessageChanged{kConv.id, echoed, /*textOnly=*/false}});
        f.stub->attachmentDeletes[0].done(true, {});
        list.viewport()->grab();
        const auto stored = list.lastOwnMessage(UserId{"U1"});
        REQUIRE(stored->attachments.size() == 1);
        CHECK(stored->attachments[0].title == "Bravo");
        CHECK(list.rowViewportRect(message.ts).height() == oneCardH);
    }

    SECTION("refused: the card stays hidden here, the model keeps it, the user is told") {
        QString       err;
        rpl::lifetime lt;
        f.session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
        f.stub->attachmentDeletes[0].done(false, "cant_delete_message");
        list.viewport()->grab();
        CHECK(list.lastOwnMessage(UserId{"U1"})->attachments.size() == 2);
        CHECK(list.rowViewportRect(message.ts).height() == oneCardH);
        CHECK(err.contains("cant_delete_message"));
    }
}

TEST_CASE(
    "the × only hides a preview locally when it can't be removed for everyone",
    "[message_list][preview]"
) {
    Fixture f;
    Message message;
    SECTION("someone else's message") {
        f.stub->caps.removePreview = true;
        message                    = previewMessage(UserId{"U2"});
    }
    SECTION("a backend without removePreview") {
        message = previewMessage(UserId{"U1"});
    }
    f.stub->_historyPage = {message};

    MessageListWidget list(f.session.get(), nullptr);
    list.resize(600, 400);
    list.openConversation(kConv.id);
    list.show();
    spin(300);
    list.viewport()->grab();
    const int fullH = list.rowViewportRect(message.ts).height();

    pressGutterUntil(list, message.ts, [&] {
        list.viewport()->grab();
        return list.rowViewportRect(message.ts).height() < fullH;
    });
    CHECK(f.stub->attachmentDeletes.empty());
}

// Regression: after a runtime font-size switch the sender name, timestamp and
// reaction chips of an already painted list stayed at the startup size (static
// fonts + string-keyed shaping caches) until a restart.
TEST_CASE("a painted list's header follows a runtime font-size change", "[message_list][fonts]") {
    Fixture       f;
    QTemporaryDir themeDir; // keep the font-size setting out of the real config
    qputenv("MSGA_THEME_SETTINGS_FILE", themeDir.filePath("msga.ini").toUtf8());
    auto &mgr = ThemeManager::instance();
    mgr.setFontSizeId("medium");

    Message m = makeMessage("1000.000001", "hello");
    m.reactions.push_back({"thumbsup", 12, {UserId{"U1"}}});
    f.stub->_historyPage = {m};

    MessageListWidget painted(f.session.get(), nullptr);
    painted.resize(500, 200);
    painted.openConversation(kConv.id);
    const QImage atMedium = painted.grab().toImage();

    mgr.setFontSizeId("large");
    const QImage afterSwitch = painted.grab().toImage();

    // Rendered from scratch at the new size: what the switched list must match.
    MessageListWidget fresh(f.session.get(), nullptr);
    fresh.resize(500, 200);
    fresh.openConversation(kConv.id);
    const QImage freshLarge = fresh.grab().toImage();

    mgr.setFontSizeId("medium");
    qunsetenv("MSGA_THEME_SETTINGS_FILE");

    CHECK(afterSwitch != atMedium);
    CHECK(afterSwitch == freshLarge);
}
