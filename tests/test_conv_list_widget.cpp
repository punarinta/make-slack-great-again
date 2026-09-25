// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for ConvListWidget selection — the mechanism that keeps the conversation
// header (title + avatar) and the conv-list highlight in sync with the open
// message list.
//
// Regression context: clicking a desktop notification used to call
// MainWindow::openConversation(row) directly, which switched the message list
// but left the header title and the list highlight on the previously-open
// conversation, because those are only updated when ConvListWidget emits
// conversationSelected. The fix routes the notification (and search-result)
// open through selectConversation(), so the signal fires and everything lands
// on the notified conversation together. selectConversation() must therefore:
//   - select the target row and emit conversationSelected with that row, and
//   - open even a conversation the relevance filter has hidden (a notification
//     can target a DM with no recent activity — rowForId() returns -1 for it,
//     which is exactly why the old direct-open path silently did nothing).
//
// These assertions go through public accessors (selectConversation / rowForId /
// conversationId / selectedIndex) and a captured conversationSelected signal —
// none depend on widget geometry, so they run deterministically headless.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QObject>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <vector>

#include "backend/domain.h"
#include "ui/conv_list/conv_list_widget.h"
#include "ui/theme.h"
#include "ui/context_menu/context_menu.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-conv-list");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();

    // Isolate QSettings (ConvListWidget persists/loads its visited-at history)
    // so the relevance filter starts from a clean slate every run.
    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── Test data ──────────────────────────────────────────────────────────────────

static Conversation channel(const char *id, const char *name) {
    Conversation c;
    c.id       = ConversationId{id};
    c.kind     = ConvKind::PublicChannel;
    c.name     = name;
    c.isMember = true;
    return c;
}

// A DM with no unread, no activity cursors and no visit stamp — the relevance
// filter hides it (DMs start hidden until activity surfaces). This is the kind
// of conversation a notification may target while it is invisible in the list.
static Conversation hiddenDm(const char *id, const char *userId) {
    Conversation c;
    c.id       = ConversationId{id};
    c.kind     = ConvKind::Im;
    c.isMember = true;
    c.dmUser   = UserId{userId};
    return c;
}

// Captures conversationSelected emissions without pulling in Qt6::Test.
struct SelectionSpy {
    int count   = 0;
    int lastRow = -1;
    explicit SelectionSpy(ConvListWidget *w) {
        QObject::connect(w, &ConvListWidget::conversationSelected, [this](int row) {
            ++count;
            lastRow = row;
        });
    }
};

TEST_CASE("selectConversation selects a visible channel and emits the matching row") {
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general"), channel("C2", "random")});
    SelectionSpy spy(&list);

    REQUIRE(list.selectConversation(ConversationId{"C2"}));

    // The signal fired exactly once, carrying a row that resolves to C2.
    REQUIRE(spy.count == 1);
    REQUIRE(list.conversationId(spy.lastRow) == ConversationId{"C2"});
    // The list's own highlight landed on C2 too.
    REQUIRE(list.selectedIndex() >= 0);
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"C2"});
}

TEST_CASE("selectConversation opens a relevance-filtered DM that rowForId cannot find") {
    ConvListWidget       list(nullptr);
    // Native macOS QSettings ignores setPath; use a fresh id so visits from
    // earlier test runs cannot make this supposedly hidden DM visible.
    const ConversationId dmId{QUuid::createUuid().toString()};
    auto                 dm = hiddenDm("", "U1");
    dm.id                   = dmId;
    list.setConversations({channel("C1", "general"), dm});
    SelectionSpy spy(&list);

    // Precondition mirroring the bug: the DM is hidden, so the old notification
    // path (rowForId + openConversation) would find no row and do nothing —
    // leaving the header/selection stale on the previously-open conversation.
    REQUIRE(list.rowForId(dmId) < 0);

    REQUIRE(list.selectConversation(dmId));

    // It is now visible, selected, and the signal carried its row — so the
    // coordinated header + highlight update can run.
    REQUIRE(list.rowForId(dmId) >= 0);
    REQUIRE(spy.count == 1);
    REQUIRE(list.conversationId(spy.lastRow) == dmId);
    REQUIRE(list.conversationId(list.selectedIndex()) == dmId);
}

// An app/bot DM: isAppConv() keys off the roster's isBot flag, so the matching
// User must be registered via setUsers() for the conversation to land in the
// "Agents & apps" section rather than the human DM section.
static Conversation appDm(const char *id, const char *userId) {
    Conversation c;
    c.id       = ConversationId{id};
    c.kind     = ConvKind::Im;
    c.isMember = true;
    c.dmUser   = UserId{userId};
    return c;
}

static User botUser(const char *userId) {
    User u;
    u.id          = UserId{userId};
    u.name        = "ci-bot";
    u.displayName = "CI bot";
    u.isBot       = true;
    return u;
}

TEST_CASE("selectConversation opens an app DM while the Agents & apps section is hidden") {
    ConvListWidget list(nullptr);
    list.setUsers({botUser("B1")});
    list.setConversations({channel("C1", "general"), appDm("A1", "B1")});
    SelectionSpy spy(&list);

    // Visible by default: the section is on, so the app DM has a row.
    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);

    // Hide the section (Settings -> Appearance).
    list.setShowAgentsApps(false);
    REQUIRE(list.rowForId(ConversationId{"A1"}) < 0);

    // A notification or a search result for this app DM routes through
    // selectConversation(). It must still produce a row: the callers in
    // MainWindow open via `rowForId(conv) >= 0`, so a -1 here means clicking
    // the notification silently does nothing.
    REQUIRE(list.selectConversation(ConversationId{"A1"}));
    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);
    REQUIRE(spy.count == 1);
    REQUIRE(list.conversationId(spy.lastRow) == ConversationId{"A1"});
}

TEST_CASE("hiding the section keeps the app DM that is currently open") {
    ConvListWidget list(nullptr);
    list.setUsers({botUser("B1")});
    list.setConversations({channel("C1", "general"), appDm("A1", "B1")});

    REQUIRE(list.selectConversation(ConversationId{"A1"}));

    // Turning the section off must not yank the row out from under the
    // conversation the message list is showing.
    list.setShowAgentsApps(false);
    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"A1"});
}

TEST_CASE("the revealed app DM is retired once the selection moves away") {
    ConvListWidget list(nullptr);
    list.setUsers({botUser("B1")});
    list.setConversations({channel("C1", "general"), appDm("A1", "B1")});

    list.setShowAgentsApps(false);
    REQUIRE(list.selectConversation(ConversationId{"A1"}));
    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);

    // Back to a channel: the section returns to fully hidden.
    REQUIRE(list.selectConversation(ConversationId{"C1"}));
    REQUIRE(list.rowForId(ConversationId{"A1"}) < 0);
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"C1"});
}

// A group DM is titled by its members — unless the user named it ("Name
// conversation…"), in which case the alias is the title on every surface the
// list feeds (header, quick switcher, notifications).
TEST_CASE("a group DM's local name replaces the member list as its title", "[groupdm][name]") {
    ConvListWidget list(nullptr);
    User           bob;
    bob.id          = UserId{"U2"};
    bob.name        = "bob";
    bob.displayName = "Bob Builder";
    User carol;
    carol.id          = UserId{"U3"};
    carol.name        = "carol";
    carol.displayName = "Carol Danvers";
    list.setUsers({bob, carol});

    Conversation g;
    g.id       = ConversationId{"G1"};
    g.kind     = ConvKind::Mpim;
    g.name     = "mpdm-bob--carol-1";
    g.isMember = true;
    g.members  = {UserId{"U2"}, UserId{"U3"}};
    g.unread   = 1; // keeps it out of the relevance filter
    list.setConversations({channel("C1", "general"), g});
    const int row = list.rowForId(ConversationId{"G1"});
    REQUIRE(row >= 0);
    CHECK(list.resolvedName(row) == "Bob Builder, Carol Danvers");

    g.localName = "Launch crew";
    list.setConversations({channel("C1", "general"), g});
    CHECK(list.resolvedName(list.rowForId(ConversationId{"G1"})) == "Launch crew");
    // The host asks for the member-list title to use as the dialog placeholder.
    Conversation bare = g;
    bare.localName.clear();
    CHECK(list.resolvedConvName(bare) == "Bob Builder, Carol Danvers");
}

TEST_CASE("selectConversation is a no-op for an unknown id") {
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general")});
    SelectionSpy spy(&list);

    REQUIRE_FALSE(list.selectConversation(ConversationId{"NOPE"}));
    REQUIRE(spy.count == 0);
    REQUIRE(list.selectedIndex() < 0);
}

TEST_CASE("re-selecting the current conversation does not re-emit") {
    // Documents why the notification/search open keeps a `_currentConvId != conv`
    // fallback: selecting an already-selected row suppresses the signal, so the
    // caller must drive the open directly if the view somehow lags the highlight.
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general"), channel("C2", "random")});
    SelectionSpy spy(&list);

    REQUIRE(list.selectConversation(ConversationId{"C2"}));
    REQUIRE(spy.count == 1);

    // Same conversation again: still selected, but no fresh emission.
    REQUIRE(list.selectConversation(ConversationId{"C2"}));
    REQUIRE(spy.count == 1);
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"C2"});
}

// ── Starred section (issue #48) ────────────────────────────────────────────────

static Conversation starred(Conversation c) {
    c.isStarred = true;
    return c;
}

TEST_CASE("a starred conversation is listed once, in the Starred section") {
    ConvListWidget list(nullptr);
    list.setConversations({starred(channel("C1", "general")), channel("C2", "random")});

    // It has a row…
    const int row = list.rowForId(ConversationId{"C1"});
    REQUIRE(row >= 0);
    // …above the unstarred channel, because the Starred section comes first…
    REQUIRE(row < list.rowForId(ConversationId{"C2"}));
    // …and only one, so a star never duplicates the chat into two sections.
    int hits = 0;
    for (int r = 0; r < list.rowCount(); ++r)
        if (list.conversationId(r) == ConversationId{"C1"})
            ++hits;
    REQUIRE(hits == 1);
}

TEST_CASE("starring a relevance-filtered DM makes it visible") {
    // A fresh id: _visitedAt is process-wide (QSettings-backed), so a DM another
    // test opened is permanently "relevant" and would fail the precondition.
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general"), hiddenDm("D_STAR", "U9")});
    REQUIRE(list.rowForId(ConversationId{"D_STAR"}) < 0);

    // Starring is an explicit "keep this in front of me" — the relevance filter
    // must not be able to hide it again.
    list.setConversations({channel("C1", "general"), starred(hiddenDm("D_STAR", "U9"))});
    REQUIRE(list.rowForId(ConversationId{"D_STAR"}) >= 0);
}

TEST_CASE("a starred app DM shows even while Agents & apps is hidden") {
    ConvListWidget list(nullptr);
    list.setUsers({botUser("B1")});
    list.setShowAgentsApps(false);
    list.setConversations({channel("C1", "general"), starred(appDm("A1", "B1"))});

    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);
    REQUIRE(list.selectConversation(ConversationId{"A1"}));
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"A1"});
}

TEST_CASE("no Starred section header exists while nothing is starred") {
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general"), channel("C2", "random")});
    const int before = list.rowCount();

    list.setConversations({starred(channel("C1", "general")), channel("C2", "random")});
    // Exactly one extra row: the "Starred" header. The channel itself just moved.
    REQUIRE(list.rowCount() == before + 1);
}

// ── "Show only unread conversations" ──────────────────────────────────────────

static Conversation unread(Conversation c, int count = 1) {
    c.unread = count;
    return c;
}

TEST_CASE("unreads-only lists unread conversations and hides read ones", "[unreads-only]") {
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general"), unread(channel("C2", "random"))});
    // Both listed by default (channels with no data are shown until they earn a stamp).
    REQUIRE(list.rowForId(ConversationId{"C1"}) >= 0);
    REQUIRE(list.rowForId(ConversationId{"C2"}) >= 0);

    list.setUnreadsOnly(true);
    REQUIRE(list.rowForId(ConversationId{"C1"}) < 0);
    REQUIRE(list.rowForId(ConversationId{"C2"}) >= 0);

    // Reading C2 (its count drops to 0) removes it too.
    list.setConversations({channel("C1", "general"), channel("C2", "random")});
    REQUIRE(list.rowForId(ConversationId{"C2"}) < 0);

    list.setUnreadsOnly(false);
    REQUIRE(list.rowForId(ConversationId{"C1"}) >= 0);
    REQUIRE(list.rowForId(ConversationId{"C2"}) >= 0);
}

TEST_CASE(
    "unreads-only keeps the open conversation listed until the selection moves", "[unreads-only]"
) {
    ConvListWidget list(nullptr);
    list.setUnreadsOnly(true);
    list.setConversations({unread(channel("C1", "general")), unread(channel("C2", "random"))});
    REQUIRE(list.selectConversation(ConversationId{"C1"}));

    // C1 is read now (count 0) but still on screen — it must not vanish from
    // under the message list.
    list.setConversations({channel("C1", "general"), unread(channel("C2", "random"))});
    REQUIRE(list.rowForId(ConversationId{"C1"}) >= 0);
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"C1"});

    // Moving on to C2 retires it.
    REQUIRE(list.selectConversation(ConversationId{"C2"}));
    REQUIRE(list.rowForId(ConversationId{"C1"}) < 0);
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"C2"});
}

TEST_CASE(
    "unreads-only still opens a read conversation from a notification or search", "[unreads-only]"
) {
    ConvListWidget list(nullptr);
    list.setUnreadsOnly(true);
    list.setConversations(
        {unread(channel("C1", "general")), channel("C2", "random"), hiddenDm("D1", "U9")}
    );
    REQUIRE(list.rowForId(ConversationId{"C2"}) < 0);
    REQUIRE(list.rowForId(ConversationId{"D1"}) < 0);
    SelectionSpy spy(&list);

    // rowForId finds nothing for a read chat, exactly as for a relevance-hidden
    // one — selectConversation must still be able to land on it.
    REQUIRE(list.selectConversation(ConversationId{"C2"}));
    REQUIRE(spy.count == 1);
    REQUIRE(list.conversationId(spy.lastRow) == ConversationId{"C2"});

    REQUIRE(list.selectConversation(ConversationId{"D1"}));
    REQUIRE(spy.count == 2);
    REQUIRE(list.conversationId(spy.lastRow) == ConversationId{"D1"});
    // …and the previous one (C2, read) has left the list.
    REQUIRE(list.rowForId(ConversationId{"C2"}) < 0);
}

TEST_CASE(
    "unreads-only treats a muted chat with unreads as read, and exempts starred ones",
    "[unreads-only]"
) {
    ConvListWidget list(nullptr);
    list.setUnreadsOnly(true);
    Conversation muted = unread(channel("C_MUTED", "noise"), 5);
    muted.isMuted      = true;
    list.setConversations(
        {unread(channel("C1", "general")), muted, starred(channel("C_STAR", "pinned"))}
    );

    // A muted conversation paints silent, so it does not count as unread.
    REQUIRE(list.rowForId(ConversationId{"C_MUTED"}) < 0);
    // A star is an explicit "keep in front of me": the filter never hides it.
    REQUIRE(list.rowForId(ConversationId{"C_STAR"}) >= 0);
    REQUIRE(list.rowForId(ConversationId{"C1"}) >= 0);
}

TEST_CASE(
    "unreads-only follows the mentions-only highlight setting for \"Just mentions\" channels",
    "[unreads-only][mentions-only]"
) {
    // Settings → Notifications "Highlight mentions-only channels for any new
    // message". The filter shares paintRow's paintsUnread() rule, so it is the
    // observable proxy for the bold emphasis.
    ConvListWidget list(nullptr);
    list.setUnreadsOnly(true);
    Conversation quiet  = unread(channel("C_QUIET", "announcements"), 4);
    quiet.notifLevel    = NotificationLevel::Mentions; // unreads, none @mention me
    Conversation pinged = unread(channel("C_PING", "ops"), 2);
    pinged.notifLevel   = NotificationLevel::Mentions;
    pinged.mentionCount = 1;
    auto dm             = hiddenDm("D1", "U9");
    dm.unread           = 1;
    dm.notifLevel       = NotificationLevel::Mentions;
    list.setConversations({unread(channel("C_ALL", "general")), quiet, pinged, dm});

    // Default (on): any unread makes a mentions-only channel bold, so it lists.
    REQUIRE(list.rowForId(ConversationId{"C_QUIET"}) >= 0);
    REQUIRE(list.rowForId(ConversationId{"C_PING"}) >= 0);

    // Off: only an @mention (the badge) earns the emphasis. DMs and "All new
    // posts" channels badge every unread, so they are untouched.
    list.setHighlightMentionsOnlyUnreads(false);
    REQUIRE(list.rowForId(ConversationId{"C_QUIET"}) < 0);
    REQUIRE(list.rowForId(ConversationId{"C_PING"}) >= 0);
    REQUIRE(list.rowForId(ConversationId{"C_ALL"}) >= 0);
    REQUIRE(list.rowForId(ConversationId{"D1"}) >= 0);

    // The global default level counts too: a channel on Default under a
    // "Direct messages and mentions only" default is mentions-only.
    Conversation deflt = unread(channel("C_DEF", "misc"), 3);
    list.setConversations({deflt});
    REQUIRE(list.rowForId(ConversationId{"C_DEF"}) >= 0);
    list.setDefaultNotifyLevel(NotificationLevel::Mentions);
    REQUIRE(list.rowForId(ConversationId{"C_DEF"}) < 0);

    // Back on: it lists again.
    list.setHighlightMentionsOnlyUnreads(true);
    REQUIRE(list.rowForId(ConversationId{"C_DEF"}) >= 0);
}

TEST_CASE(
    "the Threads entry stays listed under unreads-only and tracks its unread state",
    "[unreads-only]"
) {
    // Issue #59: the entry is a fixed nav row (never filtered), and its unread
    // face is driven by the session's count of threads with unread replies.
    ConvListWidget list(nullptr);
    list.setShowThreads(true);
    list.setConversations({channel("C1", "general")});
    list.setUnreadsOnly(true);
    // C1 is read and hidden; the Threads row (plus section headers) remains.
    REQUIRE(list.rowForId(ConversationId{"C1"}) < 0);
    REQUIRE(list.rowCount() > 0);

    CHECK(list.unreadThreadCount() == 0);
    list.setUnreadThreadCount(3);
    CHECK(list.unreadThreadCount() == 3);
    list.setUnreadThreadCount(-1); // clamped: a count can't go negative
    CHECK(list.unreadThreadCount() == 0);
}

TEST_CASE("unreads-only applies to the Agents & apps section", "[unreads-only]") {
    ConvListWidget list(nullptr);
    list.setUsers({botUser("B1"), botUser("B2")});
    list.setConversations(
        {unread(channel("C1", "general")), appDm("A1", "B1"), unread(appDm("A2", "B2"))}
    );
    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);

    list.setUnreadsOnly(true);
    REQUIRE(list.rowForId(ConversationId{"A1"}) < 0);
    REQUIRE(list.rowForId(ConversationId{"A2"}) >= 0);
    // The open app DM stays reachable while it is open.
    REQUIRE(list.selectConversation(ConversationId{"A1"}));
    REQUIRE(list.rowForId(ConversationId{"A1"}) >= 0);
}

TEST_CASE("section unread emphasis follows membership and notification preferences") {
    ConvListWidget list(nullptr);
    auto           ch  = channel("C1", "general");
    auto           dm  = hiddenDm("D1", "U1");
    auto           app = appDm("A1", "B1");
    list.setUsers({botUser("B1")});
    ch.unread = dm.unread = app.unread = 1;
    list.setConversations({ch, dm, app});
    CHECK(list.sectionHasUnread(0));
    CHECK(list.sectionHasUnread(1));
    CHECK(list.sectionHasUnread(2));
    CHECK_FALSE(list.sectionHasUnread(3));
    dm.isStarred  = true;
    ch.notifLevel = NotificationLevel::Mute;
    app.unread    = 0;
    list.setConversations({ch, dm, app});
    CHECK_FALSE(list.sectionHasUnread(0));
    CHECK_FALSE(list.sectionHasUnread(1));
    CHECK_FALSE(list.sectionHasUnread(2));
    CHECK(list.sectionHasUnread(3));
    dm.unread     = 0;
    ch.notifLevel = NotificationLevel::Mentions;
    list.setHighlightMentionsOnlyUnreads(false);
    list.setConversations({ch, dm, app});
    CHECK_FALSE(list.sectionHasUnread(0));
    CHECK_FALSE(list.sectionHasUnread(3));
    ch.mentionCount = 1;
    list.setConversations({ch, dm, app});
    CHECK(list.sectionHasUnread(0));
}

TEST_CASE("one-to-one DM context menu stars and unstars the selected conversation") {
    ConvListWidget list(nullptr);
    list.resize(300, 500);
    auto dm         = hiddenDm("D_CONTEXT_STAR", "U_CONTEXT_STAR");
    dm.unread       = 1;
    bool wasStarred = false;
    SECTION("star") {}
    SECTION("unstar") {
        wasStarred = true;
    }
    dm.isStarred = wasStarred;
    list.setConversations({dm});
    list.show();
    QApplication::processEvents();
    ConversationId target;
    bool           requestedStar = wasStarred;
    QObject::connect(
        &list, &ConvListWidget::starConversationRequested, [&](ConversationId id, bool star) {
            target        = id;
            requestedStar = star;
        }
    );
    ContextMenu *menu = nullptr;
    // Only the DM row has a context menu. Scan viewport positions so this test
    // does not depend on the current theme's row height or section spacing.
    for (int y = 0; y < list.viewport()->height() && !menu; y += 5) {
        QPointF     pos(60, y);
        QMouseEvent press(
            QEvent::MouseButtonPress,
            pos,
            list.viewport()->mapToGlobal(pos.toPoint()),
            Qt::RightButton,
            Qt::RightButton,
            Qt::NoModifier
        );
        QApplication::sendEvent(list.viewport(), &press);
        menu = list.findChild<ContextMenu *>();
    }
    REQUIRE(menu);
    // First menu item is Star/Unstar; padding and shadow occupy the top 14px.
    const QPointF pos(50, 30);
    QMouseEvent   press(
        QEvent::MouseButtonPress,
        pos,
        menu->mapToGlobal(pos.toPoint()),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(menu, &press);
    QMouseEvent release(
        QEvent::MouseButtonRelease,
        pos,
        menu->mapToGlobal(pos.toPoint()),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(menu, &release);
    CHECK(target == dm.id);
    CHECK(requestedStar == !wasStarred);
}

TEST_CASE("collapsed Starred header keeps its unread state") {
    ConvListWidget list(nullptr);
    list.resize(300, 500);
    auto dm   = starred(hiddenDm("D_COLLAPSED_STAR", "U_COLLAPSED_STAR"));
    dm.unread = 1;
    list.setConversations({dm});
    list.show();
    REQUIRE(list.rowForId(dm.id) >= 0);
    // Starred is the first header (top inset 8px, default row height 30px).
    const QPointF pos(60, 20);
    QMouseEvent   press(
        QEvent::MouseButtonPress,
        pos,
        list.viewport()->mapToGlobal(pos.toPoint()),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(list.viewport(), &press);
    CHECK(list.rowForId(dm.id) < 0);
    CHECK(list.sectionHasUnread(3));
    dm.unread = 0;
    list.setConversations({dm});
    CHECK_FALSE(list.sectionHasUnread(3));
}

// Shift+Del in a Claude Code workspace removes the selected session from msga —
// but only a gray-dot (idle) one: a working (green) or terminal-held (yellow)
// session stays, as does every row outside the agent-sessions list.
TEST_CASE("removeSelectedIdleSession removes only an idle agent session") {
    ConvListWidget list(nullptr);
    list.setAgentSessions(true);
    list.setConversations({hiddenDm("D1", "U1")});
    ConversationId removed;
    QObject::connect(&list, &ConvListWidget::leaveConversationRequested, [&](ConversationId id) {
        removed = id;
    });
    REQUIRE(list.selectConversation(ConversationId{"D1"}));

    User u;
    u.id = UserId{"U1"};
    SECTION("working (green)") {
        u.isActive = true;
        list.setUsers({u});
        REQUIRE_FALSE(list.removeSelectedIdleSession());
        REQUIRE(removed.value.isEmpty());
    }
    SECTION("held by a terminal (yellow)") {
        u.unavailable = true;
        list.setUsers({u});
        REQUIRE_FALSE(list.removeSelectedIdleSession());
        REQUIRE(removed.value.isEmpty());
    }
    SECTION("idle (gray)") {
        list.setUsers({u});
        REQUIRE(list.removeSelectedIdleSession());
        REQUIRE(removed == ConversationId{"D1"});
    }
    SECTION("not an agent-sessions workspace") {
        list.setUsers({u});
        list.setAgentSessions(false);
        REQUIRE(list.selectConversation(ConversationId{"D1"}));
        REQUIRE_FALSE(list.removeSelectedIdleSession());
    }
}
