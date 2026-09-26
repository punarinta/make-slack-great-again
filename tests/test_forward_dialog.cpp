// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStandardPaths>
#include <memory>

#include "session/session.h"
#include "stub_backend.h"
#include "text/mrkdwn_parser.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/dropdown/dropdown.h"
#include "ui/forward_dialog/forward_dialog.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-forward-dialog");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

namespace {

void copyLink(ForwardDialog &dialog) {
    for (auto *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == ForwardDialog::tr("Copy Link")) {
            button->click();
            return;
        }
    }
    FAIL("Copy Link button is missing");
}

Message linkedMessage(const QString &url) {
    Message message;
    message.text = MrkdwnParser::parse("<" + url + "|original link>");
    return message;
}

} // namespace

TEST_CASE("forward copy link retains the original message data", "[forward_dialog][clipboard]") {
    QString url;
    SECTION("ordinary link") {
        url = "https://example.com/original";
    }
    SECTION("Slack message link") {
        url = "https://team.slack.com/archives/C123/p1700000000000100";
    }
    auto          message = linkedMessage(url);
    ForwardDialog dialog(message, nullptr);
    // The caller can reuse its message after constructing the modeless dialog.
    message = linkedMessage("https://example.com/replacement");
    QApplication::clipboard()->setText("before copy");
    copyLink(dialog);
    CHECK(QApplication::clipboard()->text() == url);
}

TEST_CASE("forward copy link outlives the source message", "[forward_dialog][clipboard]") {
    const QString                  url = "https://example.com/original";
    std::unique_ptr<ForwardDialog> dialog;
    {
        const auto message = linkedMessage(url);
        dialog             = std::make_unique<ForwardDialog>(message, nullptr);
    }
    QApplication::clipboard()->setText("before copy");
    copyLink(*dialog);
    CHECK(QApplication::clipboard()->text() == url);
}

// ── Cross-workspace targets ─────────────────────────────────────────────────

namespace {

void wipeTeamCache(const QString &teamId) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId)
        .removeRecursively();
}

std::unique_ptr<Session> sessionWithChannel(const QString &teamId, const QString &channel) {
    wipeTeamCache(teamId);
    auto        *stub = new msga_test::StubBackendBase;
    Conversation c;
    c.id         = ConversationId{"C_" + channel};
    c.name       = channel;
    c.kind       = ConvKind::PublicChannel;
    stub->_convs = std::vector<Conversation>{c};
    auto session = std::make_unique<Session>(std::unique_ptr<Backend>(stub), teamId);
    session->start();
    return session;
}

QLineEdit *convSearch(ForwardDialog &dialog) {
    auto *selector = dialog.findChild<ConvSelectorWidget *>();
    return selector ? selector->findChild<QLineEdit *>() : nullptr;
}

// Type into the conversation search and return the labels it offers.
QStringList offeredConversations(ForwardDialog &dialog, const QString &query) {
    auto *edit = convSearch(dialog);
    REQUIRE(edit);
    edit->clear();
    edit->setText(query);
    QStringList labels;
    for (auto *list : dialog.window()->findChildren<QListWidget *>())
        for (int i = 0; i < list->count(); ++i)
            labels << list->item(i)->text();
    return labels;
}

QPushButton *forwardButton(ForwardDialog &dialog) {
    for (auto *button : dialog.findChildren<QPushButton *>())
        if (button->text() == ForwardDialog::tr("Forward"))
            return button;
    return nullptr;
}

} // namespace

TEST_CASE("forward stays in its workspace without a choice", "[forward_dialog][workspaces]") {
    auto    source = sessionWithChannel("T_FWD_A", "general");
    Message message;
    SECTION("no workspaces") {
        ForwardDialog dialog(message, source.get());
        CHECK_FALSE(dialog.findChild<Dropdown *>());
        CHECK(dialog.targetSession() == source.get());
    }
    SECTION("only its own workspace") {
        ForwardDialog dialog(message, source.get(), {{source.get(), "A"}});
        CHECK_FALSE(dialog.findChild<Dropdown *>());
        CHECK(dialog.targetSession() == source.get());
    }
}

TEST_CASE("forward picks conversations from the chosen workspace", "[forward_dialog][workspaces]") {
    auto    agents = sessionWithChannel("T_FWD_CC", "agents");
    auto    team   = sessionWithChannel("T_FWD_TEAM", "general");
    Message message;
    message.text = MrkdwnParser::parse("done");

    ForwardDialog dialog(message, agents.get(), {{team.get(), "Team"}, {agents.get(), "Agents"}});
    CHECK(dialog.usesSession(team.get())); // the host must close it if Team goes away
    CHECK(dialog.usesSession(agents.get()));
    auto *picker = dialog.findChild<Dropdown *>();
    REQUIRE(picker);
    REQUIRE(forwardButton(dialog));
    CHECK(picker->currentText() == "Agents"); // starts on the message's own workspace
    CHECK(dialog.targetSession() == agents.get());
    CHECK(offeredConversations(dialog, "a") == QStringList{"#agents"});

    // Pick a conversation, then switch workspace: the pick goes with it.
    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.targetConv() == ConversationId{"C_agents"});
    CHECK(forwardButton(dialog)->isEnabled());

    picker->setCurrentIndex(0);
    CHECK(dialog.targetSession() == team.get());
    CHECK(dialog.targetConv().value.isEmpty());
    CHECK_FALSE(forwardButton(dialog)->isEnabled());
    CHECK(offeredConversations(dialog, "e") == QStringList{"#general"});

    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.targetConv() == ConversationId{"C_general"});
    CHECK(forwardButton(dialog)->isEnabled());
}
