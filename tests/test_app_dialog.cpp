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
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
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

QWidget *makeHost(int w = 1200, int h = 800) {
    auto *host = new QWidget;
    host->resize(w, h);
    host->show();
    return host;
}

// A column of wrapped labels — content whose height the card has to measure.
// Hidden ones model the guided-flow pattern: built up front, revealed later.
QList<QLabel *> fillWithLines(AppDialog *dlg, int lines, bool hidden = false) {
    QList<QLabel *> out;
    for (int i = 0; i < lines; ++i) {
        auto *l = new QLabel(QString("content line %1").arg(i));
        l->setWordWrap(true);
        l->setVisible(!hidden);
        dlg->contentLayout()->addWidget(l);
        out.append(l);
    }
    return out;
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

// ── Fitting a window too small for the card ──────────────────────────────────
// The main window shrinks itself to fit small screens (issue #45), so a card can
// now find itself in a window shorter or narrower than it wants to be.

TEST_CASE("content taller than the window scrolls instead of being clipped") {
    QWidget *host = makeHost(700, 420);
    auto    *dlg  = new AppDialog("Tall", host);
    fillWithLines(dlg, 40);
    dlg->open();
    QApplication::processEvents();

    auto *card = dlg->findChild<QWidget *>("appDialogCard");
    REQUIRE(card != nullptr);
    CHECK(card->height() <= host->height());

    auto *scroll = dlg->findChild<QScrollArea *>();
    REQUIRE(scroll != nullptr);
    // Everything below the fold is reachable rather than cut off.
    CHECK(scroll->widget()->height() > scroll->viewport()->height());
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
    delete host;
}

TEST_CASE("the card never grows wider than the window holding it") {
    QWidget *host = makeHost(400, 600);
    auto    *dlg  = new AppDialog("Narrow", host);
    fillWithLines(dlg, 3);
    dlg->open();
    QApplication::processEvents();

    auto *card = dlg->findChild<QWidget *>("appDialogCard");
    REQUIRE(card != nullptr);
    CHECK(card->width() <= host->width());
    delete host;
}

TEST_CASE("a roomy window grows the card instead of scrolling it") {
    QWidget *host = makeHost(1200, 800);
    auto    *dlg  = new AppDialog("Roomy", host);
    fillWithLines(dlg, 3);
    const QList<QLabel *> hidden = fillWithLines(dlg, 15, /*hidden=*/true);
    dlg->open();
    QApplication::processEvents();

    auto *card = dlg->findChild<QWidget *>("appDialogCard");
    REQUIRE(card != nullptr);
    const int shortH = card->height();

    // Content revealed after the fact (the guided-flow pattern): the card must
    // re-measure it. A scroll area does not pass its content's new size hint up
    // to the card's layout on its own, so updateCard() has to invalidate it.
    for (QLabel *l : hidden)
        l->setVisible(true);
    dlg->updateCard();
    QApplication::processEvents();

    CHECK(card->height() > shortH);
    CHECK(card->height() <= host->height());
    auto *scroll = dlg->findChild<QScrollArea *>();
    REQUIRE(scroll != nullptr);
    // Room to show it all → no scrolling.
    CHECK(scroll->verticalScrollBar()->maximum() == 0);
    delete host;
}
