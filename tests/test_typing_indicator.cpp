// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for TypingIndicatorWidget — the "<b>Alice</b> is typing…" strip above
// the composer:
//   - hidden when nobody is typing; shown when someone is
//   - names rendered bold and comma-separated
//   - "is typing" (one) vs "are typing" (many) grammar
//   - userTyping() refreshes an existing typer instead of duplicating
//   - userStopped() / clearAll() remove typers and re-hide when empty
//   - a typer falls off automatically after the expiry window
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QLabel>
#include <QTest>

#include "ui/theme_manager.h"
#include "ui/typing_indicator/typing_indicator.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-typing-indicator");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return Catch::Session().run(argc, argv);
}

// Current rich text of the indicator's label.
static QString labelText(const TypingIndicatorWidget &w) {
    auto *label = w.findChild<QLabel *>();
    REQUIRE(label != nullptr);
    return label->text();
}

TEST_CASE("typing indicator starts empty and hidden") {
    TypingIndicatorWidget w;
    CHECK(w.isHidden());
}

TEST_CASE("one typer shows a bold name with 'is typing'") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");

    CHECK_FALSE(w.isHidden());
    const QString text = labelText(w);
    CHECK(text.contains("<b>Alice</b>"));
    CHECK(text.contains("is typing"));
    CHECK_FALSE(text.contains("are typing"));
}

TEST_CASE("multiple typers are comma-separated, bold, with 'are typing'") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userTyping(UserId{"U2"}, "Bob");

    const QString text = labelText(w);
    CHECK(text.contains("<b>Alice</b>"));
    CHECK(text.contains("<b>Bob</b>"));
    CHECK(text.contains("<b>Alice</b>, <b>Bob</b>"));
    CHECK(text.contains("are typing"));
    CHECK_FALSE(text.contains("is typing"));
}

TEST_CASE("repeated typing from the same user does not duplicate") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userTyping(UserId{"U1"}, "Alice");

    const QString text = labelText(w);
    CHECK(text.count("<b>Alice</b>") == 1);
    CHECK(text.contains("is typing"));
}

TEST_CASE("a refreshed name is reflected in the label") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userTyping(UserId{"U1"}, "Alice Cooper");

    const QString text = labelText(w);
    CHECK(text.contains("<b>Alice Cooper</b>"));
    CHECK(text.count("<b>") == 1);
}

TEST_CASE("userStopped removes a typer and reverts grammar") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userTyping(UserId{"U2"}, "Bob");

    w.userStopped(UserId{"U2"});
    const QString text = labelText(w);
    CHECK_FALSE(text.contains("Bob"));
    CHECK(text.contains("<b>Alice</b>"));
    CHECK(text.contains("is typing"));

    // Removing the last typer hides the strip.
    w.userStopped(UserId{"U1"});
    CHECK(w.isHidden());
}

TEST_CASE("userStopped for an unknown user is a no-op") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userStopped(UserId{"U999"});

    CHECK_FALSE(w.isHidden());
    CHECK(labelText(w).contains("<b>Alice</b>"));
}

TEST_CASE("clearAll hides the indicator") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userTyping(UserId{"U2"}, "Bob");

    w.clearAll();
    CHECK(w.isHidden());
}

TEST_CASE("self typing from another client shows the 'another device' cue") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"UME"}, "Vladimir", /*isSelf=*/true);

    CHECK_FALSE(w.isHidden());
    const QString text = labelText(w);
    CHECK(text.contains("<b>You</b>"));
    CHECK(text.contains("another device"));
    // The account's display name is not shown for the self cue.
    CHECK_FALSE(text.contains("Vladimir"));
}

TEST_CASE("self alongside another typer renders as 'You' in the list") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    w.userTyping(UserId{"UME"}, "Vladimir", /*isSelf=*/true);

    const QString text = labelText(w);
    CHECK(text.contains("<b>Alice</b>"));
    CHECK(text.contains("<b>You</b>"));
    CHECK(text.contains("are typing"));
    CHECK_FALSE(text.contains("another device"));
    CHECK_FALSE(text.contains("Vladimir"));
}

TEST_CASE("names are HTML-escaped so they can't break markup") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "<b>x</b> & y");

    const QString text = labelText(w);
    CHECK(text.contains("&lt;b&gt;x&lt;/b&gt; &amp; y"));
}

TEST_CASE("a typer expires after the silence window") {
    TypingIndicatorWidget w;
    w.userTyping(UserId{"U1"}, "Alice");
    CHECK_FALSE(w.isHidden());

    // Expiry is 6 s with a 1 s purge tick; wait past it, pumping the event loop
    // so the purge timer fires.
    QTest::qWait(7500);
    CHECK(w.isHidden());
}
