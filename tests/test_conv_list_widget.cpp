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
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <vector>

#include "backend/domain.h"
#include "ui/conv_list/conv_list_widget.h"
#include "ui/theme.h"
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
    ConvListWidget list(nullptr);
    list.setConversations({channel("C1", "general"), hiddenDm("D1", "U1")});
    SelectionSpy spy(&list);

    // Precondition mirroring the bug: the DM is hidden, so the old notification
    // path (rowForId + openConversation) would find no row and do nothing —
    // leaving the header/selection stale on the previously-open conversation.
    REQUIRE(list.rowForId(ConversationId{"D1"}) < 0);

    REQUIRE(list.selectConversation(ConversationId{"D1"}));

    // It is now visible, selected, and the signal carried its row — so the
    // coordinated header + highlight update can run.
    REQUIRE(list.rowForId(ConversationId{"D1"}) >= 0);
    REQUIRE(spy.count == 1);
    REQUIRE(list.conversationId(spy.lastRow) == ConversationId{"D1"});
    REQUIRE(list.conversationId(list.selectedIndex()) == ConversationId{"D1"});
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
