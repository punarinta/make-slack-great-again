// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QClipboard>
#include <QPushButton>
#include <memory>

#include "text/mrkdwn_parser.h"
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
