// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Widget tests for AppDialog::topmostVisible — what the window's close shortcut
// (Cmd+W / Ctrl+W) uses to decide whether a dialog should absorb the keystroke.
// Dialogs are in-window children, so nothing else knows they are closeable; if
// the stacking order is read wrong, Cmd+W hides the whole window with a dialog
// still open on it.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QWidget>

#include "ui/app_dialog/app_dialog.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-app-dialog");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return Catch::Session().run(argc, argv);
}

namespace {

QWidget *makeHost() {
    auto *host = new QWidget;
    host->resize(1200, 800);
    host->show();
    return host;
}

} // namespace

TEST_CASE("no dialog up means nothing to close") {
    QWidget *host = makeHost();
    CHECK(AppDialog::topmostVisible(host) == nullptr);

    // Constructed but never shown: still nothing for Cmd+W to absorb.
    new AppDialog("Hidden", host);
    CHECK(AppDialog::topmostVisible(host) == nullptr);

    CHECK(AppDialog::topmostVisible(nullptr) == nullptr);
    delete host;
}

TEST_CASE("the frontmost visible dialog wins") {
    QWidget *host  = makeHost();
    auto    *below = new AppDialog("Below", host);
    auto    *above = new AppDialog("Above", host);

    below->open();
    CHECK(AppDialog::topmostVisible(host) == below);

    // A dialog opened on top of another (e.g. session import over settings) is
    // the one a close request must dismiss first.
    above->open();
    CHECK(AppDialog::topmostVisible(host) == above);

    // Dismissing it uncovers the one underneath, which becomes the next target.
    above->reject();
    CHECK(AppDialog::topmostVisible(host) == below);

    below->reject();
    CHECK(AppDialog::topmostVisible(host) == nullptr);
    delete host;
}

TEST_CASE("re-raising a dialog makes it the frontmost again") {
    QWidget *host   = makeHost();
    auto    *first  = new AppDialog("First", host);
    auto    *second = new AppDialog("Second", host);
    first->open();
    second->open();
    REQUIRE(AppDialog::topmostVisible(host) == second);

    // topmostVisible reads the parent's child order, which raise() rewrites.
    first->raise();
    CHECK(AppDialog::topmostVisible(host) == first);
    delete host;
}

TEST_CASE("a dialog on another window is not reported") {
    QWidget *host  = makeHost();
    QWidget *other = makeHost();
    auto    *dlg   = new AppDialog("Elsewhere", other);
    dlg->open();

    CHECK(AppDialog::topmostVisible(other) == dlg);
    CHECK(AppDialog::topmostVisible(host) == nullptr);
    delete other;
    delete host;
}
