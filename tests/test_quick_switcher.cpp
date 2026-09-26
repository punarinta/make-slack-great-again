// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for QuickSwitcherDialog — the Ctrl/Cmd+K conversation switcher (issue #52):
//   - Construction / paint smoke test
//   - Populated from the name-resolved conversation list, most recent first
//   - Unnamed conversations are dropped (an unresolved id is not navigable)
//   - Fuzzy filter (issue #60): subsequence match, case-insensitive, over
//     channels and DMs alike; matches ranked best-first, ties by recency
//   - Group DMs rank under a 1:1 DM / channel that matches as well (issue #61),
//     but still above a clearly weaker match; an empty query keeps recency
//   - The top match is always preselected, so Enter opens without an arrow press
//   - Up/Down move the selection and wrap; Enter emits the selected id
//   - No match → the empty notice replaces the list and Enter is inert
//   - Several workspaces: one tab per workspace above the list, opening on the
//     active one; ←/→ and Tab step through them and wrap; the list follows the
//     tab; a query re-aims the tab at the workspace with the best match (ties
//     keep the shown tab); a hand-picked tab holds while it still has matches
//     and lets go when the query is cleared or runs dry; activation reports
//     the tab's workspace; one workspace shows no tabs and keeps ←/→ for the
//     caret
//
// Assertions go through BrowseListView's count()/visibleCount()/selectedRow()
// accessors and the dialog's signal, none of which depend on geometry, so the
// tests run deterministically headless.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "backend/domain.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/quick_switcher/quick_switcher_dialog.h"
#include "ui/quick_switcher/workspace_tab_strip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-quick-switcher");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();

    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return msga_test::runCatch(argc, argv);
}

// ── Test data ─────────────────────────────────────────────────────────────────
//
// activitySeconds is the switcher's ordering key: bob (300) > general (200) >
// design (100) > the group DM (0).

static const NamedConversation kGeneral = {
    .id              = ConversationId{"C1"},
    .name            = "general",
    .kind            = ConvKind::PublicChannel,
    .activitySeconds = 200,
};
static const NamedConversation kDesign = {
    .id              = ConversationId{"C2"},
    .name            = "design-review",
    .kind            = ConvKind::PrivateChannel,
    .activitySeconds = 100,
};
static const NamedConversation kBob = {
    .id              = ConversationId{"D1"},
    .name            = "Bob Builder",
    .kind            = ConvKind::Im,
    .avatarUrl       = "https://example.invalid/bob.png",
    .activitySeconds = 300,
};
static const NamedConversation kGroup = {
    .id   = ConversationId{"G1"},
    .name = "Alice, Bob",
    .kind = ConvKind::Mpim,
};

static const std::vector<NamedConversation> kAll = {kGeneral, kDesign, kBob, kGroup};

// ── Helpers ───────────────────────────────────────────────────────────────────

static BrowseListView *list(QWidget *dlg) {
    return dlg->findChild<BrowseListView *>("quickSwitcherList");
}

static QLineEdit *field(QWidget *dlg) {
    return dlg->findChild<QLineEdit *>();
}

static void sendKey(QWidget *dlg, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(field(dlg), &press);
}

// ── Construction ──────────────────────────────────────────────────────────────

TEST_CASE("QuickSwitcher: constructs without crash", "[quickswitch][smoke]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    CHECK(field(&dlg) != nullptr);
    REQUIRE(list(&dlg) != nullptr);
}

TEST_CASE("QuickSwitcher: renders without crash", "[quickswitch][smoke]") {
    // Channels (hash/lock icon) and DMs (avatar disc) exercise both paint paths.
    QuickSwitcherDialog dlg(kAll, nullptr);
    dlg.resize(800, 600);
    QPixmap px(dlg.size());
    px.fill(Qt::transparent);
    dlg.render(&px);
    CHECK(!px.isNull());
}

// ── Population ────────────────────────────────────────────────────────────────

TEST_CASE("QuickSwitcher: lists every named conversation", "[quickswitch][items]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    CHECK(list(&dlg)->count() == 4);
    CHECK(list(&dlg)->visibleCount() == 4); // no filter yet
}

TEST_CASE("QuickSwitcher: keeps the order it was given", "[quickswitch][items]") {
    // ConvListWidget::namedConversations() hands over a most-recent-first list;
    // the dialog must not re-sort it alphabetically behind that.
    QuickSwitcherDialog dlg({kBob, kGeneral, kDesign}, nullptr);
    CHECK(list(&dlg)->idAt(0) == "D1");
    CHECK(list(&dlg)->idAt(1) == "C1");
    CHECK(list(&dlg)->idAt(2) == "C2");
}

TEST_CASE("QuickSwitcher: drops conversations with no resolved name", "[quickswitch][items]") {
    NamedConversation nameless;
    nameless.id   = ConversationId{"D9"};
    nameless.kind = ConvKind::Im;
    QuickSwitcherDialog dlg({kGeneral, nameless}, nullptr);
    CHECK(list(&dlg)->count() == 1);
    CHECK(list(&dlg)->idAt(0) == "C1");
}

// ── Filtering ─────────────────────────────────────────────────────────────────

TEST_CASE("QuickSwitcher: filters by substring", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("design");
    CHECK(list(&dlg)->visibleCount() == 1);
    CHECK(list(&dlg)->idAt(0) == "C2");
}

TEST_CASE("QuickSwitcher: filter matches mid-name, not just the start", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("review");
    CHECK(list(&dlg)->visibleCount() == 1);
    CHECK(list(&dlg)->idAt(0) == "C2");
}

TEST_CASE("QuickSwitcher: filter is case-insensitive and finds DMs", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("BOB");
    // "Bob Builder" (the DM) and "Alice, Bob" (the group DM).
    CHECK(list(&dlg)->visibleCount() == 2);
}

TEST_CASE("QuickSwitcher: fuzzy — xdg finds #xd-general (issue #60)", "[quickswitch][filter]") {
    NamedConversation xdGeneral = kGeneral;
    xdGeneral.id                = ConversationId{"C7"};
    xdGeneral.name              = "xd-general";
    QuickSwitcherDialog dlg({kBob, xdGeneral, kDesign}, nullptr);
    field(&dlg)->setText("xdg");
    REQUIRE(list(&dlg)->visibleCount() == 1);
    CHECK(list(&dlg)->idAt(0) == "C7");
}

TEST_CASE("QuickSwitcher: fuzzy — initials find a person", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("bb");
    // "Bob Builder" and "Alice, Bob" (a-l-i-c-e-,- -B-o-B) both contain b…b.
    REQUIRE(list(&dlg)->visibleCount() == 2);
    // The word-initial alignment ranks above the one buried in a single word.
    CHECK(list(&dlg)->idAt(0) == "D1");
}

TEST_CASE("QuickSwitcher: fuzzy — letters out of order do not match", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("ngis"); // "design" backwards-ish
    CHECK(list(&dlg)->visibleCount() == 0);
}

TEST_CASE("QuickSwitcher: matches are ranked best-first, not by recency", "[quickswitch][filter]") {
    // Both contain "gen"; the verbatim prefix must win even though the other
    // conversation is more recent and was handed over first.
    NamedConversation goEngineering = kGeneral;
    goEngineering.id                = ConversationId{"C8"};
    goEngineering.name              = "go-engineering";
    goEngineering.activitySeconds   = 900;
    QuickSwitcherDialog dlg({goEngineering, kBob, kGeneral}, nullptr);
    field(&dlg)->setText("gen");
    REQUIRE(list(&dlg)->visibleCount() == 2);
    CHECK(list(&dlg)->idAt(0) == "C1");
    CHECK(list(&dlg)->idAt(1) == "C8");
}

TEST_CASE("QuickSwitcher: equally good matches keep recency order", "[quickswitch][filter]") {
    NamedConversation designB = kDesign;
    designB.id                = ConversationId{"C9"};
    designB.name              = "design-backend";
    // Given most-recent-first: C9 before C2. Same score for "design-" → same order.
    QuickSwitcherDialog dlg({designB, kDesign, kBob}, nullptr);
    field(&dlg)->setText("design-");
    REQUIRE(list(&dlg)->visibleCount() == 2);
    CHECK(list(&dlg)->idAt(0) == "C9");
    CHECK(list(&dlg)->idAt(1) == "C2");
}

TEST_CASE(
    "QuickSwitcher: a 1:1 DM outranks group DMs it is named in (issue #61)", "[quickswitch][filter]"
) {
    // "Bob" starts both names, so the alignments score the same; before #61 the
    // more recent group DM (handed over first) won the tie and the person was
    // buried under every group he is a member of.
    NamedConversation bobCarol = kGroup;
    bobCarol.id                = ConversationId{"G2"};
    bobCarol.name              = "Bob Builder, Carol";
    NamedConversation bobDave  = kGroup;
    bobDave.id                 = ConversationId{"G3"};
    bobDave.name               = "Bob Builder, Dave";
    QuickSwitcherDialog dlg({bobCarol, bobDave, kBob, kGroup}, nullptr);

    field(&dlg)->setText("bob");
    REQUIRE(list(&dlg)->visibleCount() == 4);
    CHECK(list(&dlg)->idAt(0) == "D1");
    // The groups keep their own recency order behind him.
    CHECK(list(&dlg)->idAt(1) == "G2");
    CHECK(list(&dlg)->idAt(2) == "G3");
    CHECK(list(&dlg)->idAt(3) == "G1");

    // Typing the full name — still the person, not the groups.
    field(&dlg)->setText("bob builder");
    REQUIRE(list(&dlg)->visibleCount() == 3);
    CHECK(list(&dlg)->idAt(0) == "D1");
}

TEST_CASE(
    "QuickSwitcher: a group DM matched at a word start beats a 1:1 DM matched mid-word",
    "[quickswitch][filter]"
) {
    // The demotion is a bias, not a tier: a group whose name matches a whole
    // character better still wins over a person whose name barely does.
    NamedConversation abbot = kBob;
    abbot.id                = ConversationId{"D2"};
    abbot.name              = "Abbot Bosch"; // a-B-b-O-t- -B-osch: "bob" without a full run
    QuickSwitcherDialog dlg({abbot, kGroup}, nullptr);
    field(&dlg)->setText("bob");
    REQUIRE(list(&dlg)->visibleCount() == 2);
    CHECK(list(&dlg)->idAt(0) == "G1"); // "Alice, Bob": a verbatim word-start run
    CHECK(list(&dlg)->idAt(1) == "D2");
}

TEST_CASE(
    "QuickSwitcher: an empty query keeps group DMs in recency order", "[quickswitch][filter]"
) {
    // The bias only ranks matches; the "recent chats" list is untouched.
    QuickSwitcherDialog dlg({kGroup, kBob, kGeneral}, nullptr);
    CHECK(list(&dlg)->idAt(0) == "G1");
    CHECK(list(&dlg)->idAt(1) == "D1");
}

TEST_CASE("QuickSwitcher: clearing the filter restores everything", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("general");
    REQUIRE(list(&dlg)->visibleCount() == 1);
    field(&dlg)->clear();
    CHECK(list(&dlg)->visibleCount() == 4);
}

TEST_CASE("QuickSwitcher: no match shows the empty notice", "[quickswitch][filter]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    field(&dlg)->setText("zzz-no-match");
    CHECK(list(&dlg)->visibleCount() == 0);
    CHECK_FALSE(list(&dlg)->isVisible());

    bool fired = false;
    QObject::connect(&dlg, &QuickSwitcherDialog::conversationActivated, &dlg, [&fired] {
        fired = true;
    });
    sendKey(&dlg, Qt::Key_Return);
    CHECK_FALSE(fired); // nothing selected → Enter is inert, not a crash
}

// ── Selection ─────────────────────────────────────────────────────────────────

TEST_CASE("QuickSwitcher: the top match is preselected", "[quickswitch][select]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    CHECK(list(&dlg)->selectedRow() == 0);

    // ...and again after every filter change, not just at construction.
    field(&dlg)->setText("bob");
    CHECK(list(&dlg)->selectedRow() == 0);
    CHECK(list(&dlg)->selectedId() == "D1");
}

TEST_CASE("QuickSwitcher: arrow keys move the selection", "[quickswitch][select]") {
    QuickSwitcherDialog dlg({kBob, kGeneral, kDesign}, nullptr);
    REQUIRE(list(&dlg)->selectedRow() == 0);

    sendKey(&dlg, Qt::Key_Down);
    CHECK(list(&dlg)->selectedRow() == 1);
    sendKey(&dlg, Qt::Key_Down);
    CHECK(list(&dlg)->selectedRow() == 2);
    sendKey(&dlg, Qt::Key_Up);
    CHECK(list(&dlg)->selectedRow() == 1);
}

TEST_CASE("QuickSwitcher: the selection wraps at both ends", "[quickswitch][select]") {
    QuickSwitcherDialog dlg({kBob, kGeneral, kDesign}, nullptr);
    sendKey(&dlg, Qt::Key_Up);
    CHECK(list(&dlg)->selectedRow() == 2); // up from the top → last match
    sendKey(&dlg, Qt::Key_Down);
    CHECK(list(&dlg)->selectedRow() == 0); // and back around
}

// ── Activation ────────────────────────────────────────────────────────────────

TEST_CASE("QuickSwitcher: Enter emits the selected conversation", "[quickswitch][activate]") {
    QuickSwitcherDialog dlg({kBob, kGeneral, kDesign}, nullptr);
    ConversationId      activated;
    QObject::connect(
        &dlg,
        &QuickSwitcherDialog::conversationActivated,
        &dlg,
        [&activated](const QString &, ConversationId id) { activated = id; }
    );

    sendKey(&dlg, Qt::Key_Down); // → general
    sendKey(&dlg, Qt::Key_Return);
    CHECK(activated.value == "C1");
}

TEST_CASE("QuickSwitcher: Enter after filtering opens the top match", "[quickswitch][activate]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    ConversationId      activated;
    QObject::connect(
        &dlg,
        &QuickSwitcherDialog::conversationActivated,
        &dlg,
        [&activated](const QString &, ConversationId id) { activated = id; }
    );

    field(&dlg)->setText("desi");
    sendKey(&dlg, Qt::Key_Return);
    CHECK(activated.value == "C2");
}

TEST_CASE("QuickSwitcher: a row click emits it too", "[quickswitch][activate]") {
    QuickSwitcherDialog dlg({kBob, kGeneral}, nullptr);
    ConversationId      activated;
    QObject::connect(
        &dlg,
        &QuickSwitcherDialog::conversationActivated,
        &dlg,
        [&activated](const QString &, ConversationId id) { activated = id; }
    );

    // The same hook the virtual list invokes on a click.
    list(&dlg)->onActivated("C1");
    CHECK(activated.value == "C1");
}

TEST_CASE("QuickSwitcher: activation closes the dialog", "[quickswitch][activate]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    dlg.show();
    REQUIRE(dlg.isVisible());
    sendKey(&dlg, Qt::Key_Return);
    CHECK_FALSE(dlg.isVisible());
}

// ── Several workspaces ────────────────────────────────────────────────────────
//
// Workspace A (active) holds the usual four; workspace B holds #general too,
// plus #backend and a DM with Zed — names that only B can answer for.

static WorkspaceTabStrip *tabs(QWidget *dlg) {
    return dlg->findChild<WorkspaceTabStrip *>("quickSwitcherTabs");
}

static const NamedConversation kBackend = {
    .id              = ConversationId{"C21"},
    .name            = "backend",
    .kind            = ConvKind::PublicChannel,
    .activitySeconds = 500,
};
static const NamedConversation kZed = {
    .id              = ConversationId{"D21"},
    .name            = "Zed Zephyr",
    .kind            = ConvKind::Im,
    .activitySeconds = 400,
};
static const NamedConversation kGeneralB = {
    .id              = ConversationId{"C22"},
    .name            = "general",
    .kind            = ConvKind::PublicChannel,
    .activitySeconds = 50,
};

static std::vector<QuickSwitcherDialog::Workspace> twoWorkspaces() {
    return {
        {.teamId = "slack:TA", .name = "Acme", .iconUrl = {}, .conversations = kAll},
        {.teamId        = "slack:TB",
         .name          = "Beta Corp",
         .iconUrl       = {},
         .conversations = {kBackend, kZed, kGeneralB}},
    };
}

TEST_CASE("QuickSwitcher: one tab per workspace, opening on the active one", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TB", nullptr);
    dlg.show();
    REQUIRE(tabs(&dlg) != nullptr);
    CHECK(tabs(&dlg)->isVisible());
    CHECK(tabs(&dlg)->count() == 2);
    CHECK(tabs(&dlg)->currentIndex() == 1);
    CHECK(tabs(&dlg)->currentTeamId() == "slack:TB");
    // The list is that workspace's recent chats.
    CHECK(list(&dlg)->count() == 3);
    CHECK(list(&dlg)->idAt(0) == "C21");
}

TEST_CASE("QuickSwitcher: an unknown active id falls back to the first tab", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TZ", nullptr);
    CHECK(tabs(&dlg)->currentIndex() == 0);
    CHECK(list(&dlg)->count() == 4);
}

TEST_CASE("QuickSwitcher: a single workspace shows no tabs", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    dlg.show();
    REQUIRE(tabs(&dlg) != nullptr);
    CHECK_FALSE(tabs(&dlg)->isVisible());
    // ←/→ stay with the field — nothing to switch, and the caret needs them.
    field(&dlg)->setText("bob");
    field(&dlg)->setCursorPosition(3);
    sendKey(&dlg, Qt::Key_Left);
    CHECK(field(&dlg)->cursorPosition() == 2);
    CHECK(tabs(&dlg)->currentIndex() == 0);
}

TEST_CASE("QuickSwitcher: ←/→ and Tab step through the workspaces and wrap", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    REQUIRE(tabs(&dlg)->currentIndex() == 0);

    sendKey(&dlg, Qt::Key_Right);
    CHECK(tabs(&dlg)->currentIndex() == 1);
    CHECK(list(&dlg)->count() == 3); // the list follows the tab
    sendKey(&dlg, Qt::Key_Right);
    CHECK(tabs(&dlg)->currentIndex() == 0); // wraps
    CHECK(list(&dlg)->count() == 4);
    sendKey(&dlg, Qt::Key_Left);
    CHECK(tabs(&dlg)->currentIndex() == 1); // wraps the other way
    sendKey(&dlg, Qt::Key_Tab);
    CHECK(tabs(&dlg)->currentIndex() == 0);
    sendKey(&dlg, Qt::Key_Backtab);
    CHECK(tabs(&dlg)->currentIndex() == 1);
    // The caret was left alone by all of that.
    field(&dlg)->setText("bob");
    field(&dlg)->setCursorPosition(3);
    sendKey(&dlg, Qt::Key_Left);
    CHECK(field(&dlg)->cursorPosition() == 3);
}

TEST_CASE("QuickSwitcher: a bubble click switches the tab", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    tabs(&dlg)->setCurrentIndex(1); // what mousePressEvent does on a hit
    CHECK(list(&dlg)->count() == 3);
    CHECK(list(&dlg)->selectedRow() == 0);
}

TEST_CASE(
    "QuickSwitcher: typing re-aims the tab at the workspace with the match", "[quickswitch][ws]"
) {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    field(&dlg)->setText("zed");
    CHECK(tabs(&dlg)->currentIndex() == 1);
    REQUIRE(list(&dlg)->visibleCount() == 1);
    CHECK(list(&dlg)->idAt(0) == "D21");
    CHECK(list(&dlg)->selectedRow() == 0);

    // ...and back, when the letters only fit the other workspace.
    field(&dlg)->setText("design");
    CHECK(tabs(&dlg)->currentIndex() == 0);
    CHECK(list(&dlg)->idAt(0) == "C2");
}

TEST_CASE("QuickSwitcher: the better match wins across workspaces", "[quickswitch][ws]") {
    // "back" is a verbatim prefix of #backend (B); in A it only scatters
    // through "Bob Builder" / "Alice, Bob" — B is the probable target.
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    field(&dlg)->setText("back");
    CHECK(tabs(&dlg)->currentIndex() == 1);
    CHECK(list(&dlg)->idAt(0) == "C21");
}

TEST_CASE("QuickSwitcher: an equal match keeps the shown workspace", "[quickswitch][ws]") {
    // #general exists in both: no reason to flip away from what is on screen.
    {
        QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
        field(&dlg)->setText("general");
        CHECK(tabs(&dlg)->currentIndex() == 0);
        CHECK(list(&dlg)->idAt(0) == "C1");
    }
    {
        QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TB", nullptr);
        field(&dlg)->setText("general");
        CHECK(tabs(&dlg)->currentIndex() == 1);
        CHECK(list(&dlg)->idAt(0) == "C22");
    }
}

TEST_CASE(
    "QuickSwitcher: a hand-picked tab holds while it still has matches", "[quickswitch][ws]"
) {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    field(&dlg)->setText("gen"); // both have #general → stays on A
    REQUIRE(tabs(&dlg)->currentIndex() == 0);
    sendKey(&dlg, Qt::Key_Right); // the user wants B's #general
    REQUIRE(tabs(&dlg)->currentIndex() == 1);
    CHECK(list(&dlg)->idAt(0) == "C22");

    // More letters: A's "general" scores the same, so re-aiming would have
    // been a tie anyway — but B is now pinned regardless of scores. "b" alone
    // scores best in A (Bob at a word start) yet B still has #backend: hold.
    field(&dlg)->setText("b");
    CHECK(tabs(&dlg)->currentIndex() == 1);
    CHECK(list(&dlg)->idAt(0) == "C21");
}

TEST_CASE("QuickSwitcher: a tab picked before typing does not pin", "[quickswitch][ws]") {
    // Open on B, step to A over an empty field, then type: that was browsing,
    // not a choice about the query — "nik"-style letters that only B answers
    // must still re-aim (the Henrik-vs-Nikita report).
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TB", nullptr);
    sendKey(&dlg, Qt::Key_Left);
    REQUIRE(tabs(&dlg)->currentIndex() == 0);
    field(&dlg)->setText("zed");
    CHECK(tabs(&dlg)->currentIndex() == 1);
    CHECK(list(&dlg)->idAt(0) == "D21");

    // And with a match on both sides the better one still wins: "b" is a
    // word-start hit in A (Bob) but a verbatim prefix in B (#backend).
    field(&dlg)->clear();
    sendKey(&dlg, Qt::Key_Left);
    REQUIRE(tabs(&dlg)->currentIndex() == 0);
    field(&dlg)->setText("back");
    CHECK(tabs(&dlg)->currentIndex() == 1);
}

TEST_CASE("QuickSwitcher: a hand-picked tab lets go when it runs dry", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    sendKey(&dlg, Qt::Key_Right);
    REQUIRE(tabs(&dlg)->currentIndex() == 1);
    field(&dlg)->setText("design"); // nothing in B, one hit in A
    CHECK(tabs(&dlg)->currentIndex() == 0);
    CHECK(list(&dlg)->idAt(0) == "C2");
}

TEST_CASE("QuickSwitcher: clearing the query releases the pinned tab", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    field(&dlg)->setText("gen");
    sendKey(&dlg, Qt::Key_Right);
    REQUIRE(tabs(&dlg)->currentIndex() == 1);
    field(&dlg)->clear();
    CHECK(tabs(&dlg)->currentIndex() == 1); // clearing itself moves nothing
    CHECK(list(&dlg)->visibleCount() == 3);
    field(&dlg)->setText("bob"); // fresh query → free to re-aim
    CHECK(tabs(&dlg)->currentIndex() == 0);
    CHECK(list(&dlg)->idAt(0) == "D1");
}

TEST_CASE("QuickSwitcher: → onto a dimmed workspace is honoured", "[quickswitch][ws]") {
    // Nothing in B matches "design"; the user can still look there, and the
    // notice says where the matches are instead of a bare "nothing".
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    field(&dlg)->setText("design");
    REQUIRE(tabs(&dlg)->currentIndex() == 0);
    sendKey(&dlg, Qt::Key_Right);
    CHECK(tabs(&dlg)->currentIndex() == 1);
    CHECK(list(&dlg)->visibleCount() == 0);
    auto *empty = dlg.findChild<QLabel *>();
    REQUIRE(empty != nullptr);
    bool found = false;
    for (auto *lbl : dlg.findChildren<QLabel *>())
        found = found || lbl->text().contains("Beta Corp");
    CHECK(found);
}

TEST_CASE("QuickSwitcher: activation reports the tab's workspace", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    QString             team;
    ConversationId      activated;
    QObject::connect(
        &dlg,
        &QuickSwitcherDialog::conversationActivated,
        &dlg,
        [&](const QString &t, ConversationId id) {
            team      = t;
            activated = id;
        }
    );
    field(&dlg)->setText("zed");
    sendKey(&dlg, Qt::Key_Return);
    CHECK(team == "slack:TB");
    CHECK(activated.value == "D21");
}

TEST_CASE("QuickSwitcher: the single-workspace form reports an empty team", "[quickswitch][ws]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    QString             team = "unset";
    QObject::connect(
        &dlg,
        &QuickSwitcherDialog::conversationActivated,
        &dlg,
        [&](const QString &t, ConversationId) { team = t; }
    );
    sendKey(&dlg, Qt::Key_Return);
    CHECK(team.isEmpty());
}

TEST_CASE("QuickSwitcher: renders with tabs without crash", "[quickswitch][ws][smoke]") {
    QuickSwitcherDialog dlg(twoWorkspaces(), "slack:TA", nullptr);
    dlg.resize(800, 600);
    field(&dlg)->setText("zed"); // one dimmed tab, one lit
    QPixmap px(dlg.size());
    px.fill(Qt::transparent);
    dlg.render(&px);
    CHECK(!px.isNull());
}
