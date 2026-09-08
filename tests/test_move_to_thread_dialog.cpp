// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests for MoveToThreadDialog — the "Move to thread…" target picker:
//   - Rows come from the given thread choices, in the given order
//   - The top row is preselected, so Move/Enter act on a visible highlight
//   - Filter matches root text and author, case-insensitive
//   - No match → the empty notice replaces the list and Enter is inert
//   - Enter / Move accept with the highlighted root; a click only selects
//   - No threads at all → nothing to select, Move stays disabled
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>

#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/move_to_thread_dialog/move_to_thread_dialog.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-move-to-thread");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();

    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── Test data (newest first, as the host hands them over) ─────────────────────

static const ThreadChoice kDeploy = {
    .root       = "300.000",
    .text       = "Deploy plan for Friday",
    .author     = "Alice Wonder",
    .date       = 1'700'000'300'000'000,
    .replyCount = 4,
};
static const ThreadChoice kLunch = {
    .root       = "200.000",
    .text       = "Lunch options?",
    .author     = "Bob Builder",
    .date       = 1'700'000'200'000'000,
    .replyCount = 1,
};
static const ThreadChoice kFileOnly = {
    .root       = "100.000",
    .text       = "",
    .author     = "Carol",
    .date       = 1'700'000'100'000'000,
    .replyCount = 2,
};

static const std::vector<ThreadChoice> kAll = {kDeploy, kLunch, kFileOnly};

// ── Helpers ───────────────────────────────────────────────────────────────────

static BrowseListView *list(QWidget *dlg) {
    return dlg->findChild<BrowseListView *>("moveToThreadList");
}
static QLineEdit *field(QWidget *dlg) {
    return dlg->findChild<StyledLineEdit *>()->lineEdit();
}
static StyledButton *moveButton(QWidget *dlg) {
    for (auto *b : dlg->findChildren<StyledButton *>())
        if (b->text() == "Move")
            return b;
    return nullptr;
}
static void pressKey(QWidget *w, int key) {
    QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("rows follow the given threads and the top one is preselected", "[move][dialog]") {
    MoveToThreadDialog dlg(kAll, nullptr);
    auto              *l = list(&dlg);
    REQUIRE(l);
    CHECK(l->count() == 3);
    CHECK(l->visibleCount() == 3);
    CHECK(l->idAt(0) == "300.000");
    CHECK(l->idAt(2) == "100.000");
    CHECK(l->selectedRow() == 0);
    CHECK(dlg.selectedRoot() == "300.000");
    REQUIRE(moveButton(&dlg));
    CHECK(moveButton(&dlg)->isEnabled());
}

TEST_CASE("filter matches root text and author, case-insensitively", "[move][dialog]") {
    MoveToThreadDialog dlg(kAll, nullptr);
    field(&dlg)->setText("LUNCH");
    CHECK(list(&dlg)->visibleCount() == 1);
    CHECK(dlg.selectedRoot() == "200.000");

    field(&dlg)->setText("carol"); // a file-only root is found by its author
    CHECK(list(&dlg)->visibleCount() == 1);
    CHECK(dlg.selectedRoot() == "100.000");
}

TEST_CASE("no match shows the empty notice and disables Move", "[move][dialog]") {
    MoveToThreadDialog dlg(kAll, nullptr);
    field(&dlg)->setText("zzz");
    CHECK(list(&dlg)->visibleCount() == 0);
    CHECK(dlg.selectedRoot().isEmpty());
    CHECK_FALSE(moveButton(&dlg)->isEnabled());

    int accepted = 0;
    QObject::connect(&dlg, &AppDialog::accepted, [&] { ++accepted; });
    pressKey(field(&dlg), Qt::Key_Return);
    CHECK(accepted == 0);
}

TEST_CASE("arrows move the highlight and Enter accepts it", "[move][dialog]") {
    MoveToThreadDialog dlg(kAll, nullptr);
    int                accepted = 0;
    QObject::connect(&dlg, &AppDialog::accepted, [&] { ++accepted; });

    pressKey(field(&dlg), Qt::Key_Down);
    CHECK(dlg.selectedRoot() == "200.000");
    pressKey(field(&dlg), Qt::Key_Return);
    CHECK(accepted == 1);
    CHECK(dlg.selectedRoot() == "200.000");
}

TEST_CASE("a click selects a row without accepting", "[move][dialog]") {
    MoveToThreadDialog dlg(kAll, nullptr);
    int                accepted = 0;
    QObject::connect(&dlg, &AppDialog::accepted, [&] { ++accepted; });

    list(&dlg)->onActivated("100.000"); // what a row click fires
    CHECK(dlg.selectedRoot() == "100.000");
    CHECK(accepted == 0);
    CHECK(moveButton(&dlg)->isEnabled());
}

TEST_CASE("with no threads nothing is selectable", "[move][dialog]") {
    MoveToThreadDialog dlg({}, nullptr);
    CHECK(dlg.selectedRoot().isEmpty());
    CHECK_FALSE(moveButton(&dlg)->isEnabled());
    auto *empty = dlg.findChild<QLabel *>();
    REQUIRE(empty);
}

TEST_CASE("the attribution note is opt-in", "[move][dialog]") {
    MoveToThreadDialog dlg(kAll, nullptr);
    CHECK_FALSE(dlg.addNote()); // unchecked by default
    auto *box = dlg.findChild<QCheckBox *>();
    REQUIRE(box);
    box->setChecked(true);
    CHECK(dlg.addNote());
}
