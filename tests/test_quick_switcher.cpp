// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for QuickSwitcherDialog — the Ctrl/Cmd+K conversation switcher (issue #52):
//   - Construction / paint smoke test
//   - Populated from the name-resolved conversation list, most recent first
//   - Unnamed conversations are dropped (an unresolved id is not navigable)
//   - Substring filter, case-insensitive, over channels and DMs alike
//   - The top match is always preselected, so Enter opens without an arrow press
//   - Up/Down move the selection and wrap; Enter emits the selected id
//   - No match → the empty notice replaces the list and Enter is inert
//
// Assertions go through BrowseListView's count()/visibleCount()/selectedRow()
// accessors and the dialog's signal, none of which depend on geometry, so the
// tests run deterministically headless.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

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
#include "ui/theme.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-quick-switcher");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();

    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
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
        &dlg, &QuickSwitcherDialog::conversationActivated, &dlg, [&activated](ConversationId id) {
            activated = id;
        }
    );

    sendKey(&dlg, Qt::Key_Down); // → general
    sendKey(&dlg, Qt::Key_Return);
    CHECK(activated.value == "C1");
}

TEST_CASE("QuickSwitcher: Enter after filtering opens the top match", "[quickswitch][activate]") {
    QuickSwitcherDialog dlg(kAll, nullptr);
    ConversationId      activated;
    QObject::connect(
        &dlg, &QuickSwitcherDialog::conversationActivated, &dlg, [&activated](ConversationId id) {
            activated = id;
        }
    );

    field(&dlg)->setText("desi");
    sendKey(&dlg, Qt::Key_Return);
    CHECK(activated.value == "C2");
}

TEST_CASE("QuickSwitcher: a row click emits it too", "[quickswitch][activate]") {
    QuickSwitcherDialog dlg({kBob, kGeneral}, nullptr);
    ConversationId      activated;
    QObject::connect(
        &dlg, &QuickSwitcherDialog::conversationActivated, &dlg, [&activated](ConversationId id) {
            activated = id;
        }
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
