// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Widget tests for SummaryDialog (the "Summarize down" report card):
//   - Escape closes it (inherited from AppDialog — regression guard)
//   - long content scrolls inside a QScrollArea instead of growing the card
//   - the card is the widened 840px (clamped to the host window)
//   - Copy is the only button; it puts the raw Markdown on the clipboard
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>

#include "ui/summary_dialog/summary_dialog.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-summary-dialog");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return Catch::Session().run(argc, argv);
}

namespace {

QWidget *makeHost() {
    auto *host = new QWidget;
    host->resize(1400, 900);
    host->show();
    return host;
}

} // namespace

TEST_CASE("Escape closes the dialog") {
    QWidget *host = makeHost();
    auto    *dlg  = new SummaryDialog("**Goal:** test", SummaryDialog::Kind::Report, host);
    dlg->open();
    QApplication::processEvents();
    REQUIRE(dlg->isVisible());

    QSignalSpy rejectedSpy(dlg, &AppDialog::rejected);
    QTest::keyClick(
        QApplication::focusWidget() ? QApplication::focusWidget() : dlg, Qt::Key_Escape
    );
    QApplication::processEvents();

    CHECK(rejectedSpy.count() == 1);
    CHECK(!dlg->isVisible());
    delete host;
}

TEST_CASE("long content scrolls instead of growing the card") {
    QWidget *host = makeHost();
    QString  longMd;
    for (int i = 0; i < 200; ++i)
        longMd += QString("- bullet line %1\n").arg(i);
    auto *dlg = new SummaryDialog(longMd, SummaryDialog::Kind::Report, host);
    dlg->open();
    QApplication::processEvents();

    auto *scroll = dlg->findChild<QScrollArea *>();
    REQUIRE(scroll != nullptr);
    // The body outgrew the viewport → vertical scrolling is engaged.
    CHECK(scroll->widget()->height() > scroll->viewport()->height());
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
    // The card itself stayed inside the host window.
    auto *card = dlg->findChild<QFrame *>("appDialogCard");
    REQUIRE(card != nullptr);
    CHECK(card->height() <= host->height());
    delete host;
}

TEST_CASE("card uses the widened width on a big host") {
    QWidget *host = makeHost();
    auto    *dlg  = new SummaryDialog("short", SummaryDialog::Kind::Report, host);
    dlg->open();
    QApplication::processEvents();
    auto *card = dlg->findChild<QFrame *>("appDialogCard");
    REQUIRE(card != nullptr);
    CHECK(card->width() == 840);
    delete host;
}

TEST_CASE("Copy is the only button and copies the raw Markdown") {
    QWidget *host = makeHost();
    auto    *dlg  = new SummaryDialog("**Goal:** ship it", SummaryDialog::Kind::Report, host);
    dlg->open();
    QApplication::processEvents();

    // One StyledButton (Copy); the header × (an IconButton) is not one.
    const auto buttons = dlg->findChildren<StyledButton *>();
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.first()->text() == "Copy");

    buttons.first()->click();
    CHECK(QApplication::clipboard()->text() == "**Goal:** ship it");
    delete host;
}

TEST_CASE("body renders Markdown as rich text, not literally") {
    QWidget *host = makeHost();
    auto    *dlg  = new SummaryDialog(
        "Deploy **frozen** until *Friday*\n\n- QA signoff", SummaryDialog::Kind::Report, host
    );
    dlg->open();
    QApplication::processEvents();

    // The body is the scroll area's widget (a bare findChild<QLabel*> would
    // land on the dialog's header title label first).
    auto *scroll = dlg->findChild<QScrollArea *>();
    REQUIRE(scroll != nullptr);
    auto *body = qobject_cast<QLabel *>(scroll->widget());
    REQUIRE(body != nullptr);
    CHECK(body->textFormat() == Qt::RichText);
    // Markdown markers were converted, not shown to the user.
    CHECK(!body->text().contains("**"));
    CHECK(body->text().contains("font-weight"));       // **frozen** → bold
    CHECK(body->text().contains("font-style:italic")); // *Friday* → italic
    CHECK(body->text().contains("<li"));               // "- QA signoff" → list
    delete host;
}

TEST_CASE("failure notice has no buttons and default width") {
    QWidget *host = makeHost();
    auto    *dlg =
        new SummaryDialog("Couldn't summarize: timeout", SummaryDialog::Kind::Failure, host);
    dlg->open();
    QApplication::processEvents();
    CHECK(dlg->findChildren<StyledButton *>().isEmpty());
    // Notices keep the compact AppDialog card, not the widened report card.
    auto *card = dlg->findChild<QFrame *>("appDialogCard");
    REQUIRE(card != nullptr);
    CHECK(card->width() <= 560);
    delete host;
}

TEST_CASE("no-provider notice deep-links to settings") {
    QWidget *host = makeHost();
    auto    *dlg =
        new SummaryDialog("Summaries need an AI provider.", SummaryDialog::Kind::NoProvider, host);
    dlg->open();
    QApplication::processEvents();

    const auto buttons = dlg->findChildren<StyledButton *>();
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.first()->text() == "Open settings");

    QSignalSpy settingsSpy(dlg, &SummaryDialog::openSettingsRequested);
    buttons.first()->click();
    QApplication::processEvents();
    CHECK(settingsSpy.count() == 1);
    CHECK(!dlg->isVisible()); // the notice closes itself before deep-linking
    delete host;
}
