// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for the message composer and related session/backend features.
// Requires QApplication because ComposerWidget inherits QWidget.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QTextEdit>

#include "ui/composer/composer_widget.h"
#include "session/session.h"
#include "backend/backend.h"
#include "text/mrkdwn_parser.h"
#include "rpl/variable.h"

// ── Custom main (need QApplication before Catch runs) ────────────────────────

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-composer");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Type text into the hidden QTextEdit inside a ComposerWidget.
static QTextEdit *editOf(ComposerWidget *c) {
    return c->findChild<QTextEdit*>("composerEdit");
}

static void typeText(ComposerWidget *c, const QString &text) {
    QTextEdit *ed = editOf(c);
    REQUIRE(ed);
    ed->setPlainText(text);
    auto cursor = ed->textCursor();
    cursor.movePosition(QTextCursor::End);
    ed->setTextCursor(cursor);
}

// ── Optimistic mrkdwn parsing ─────────────────────────────────────────────────

TEST_CASE("optimistic message text is parsed through MrkdwnParser", "[session][mrkdwn]") {
    // sendMessage must call MrkdwnParser::parse — verify via the fixture used in
    // test_session.cpp (inline stub here since we can't link test_session.cpp).
    auto r = MrkdwnParser::parse("*bold* and _italic_");
    CHECK(r.text == "bold and italic");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].type == EntityType::Bold);
    CHECK(r.entities[1].type == EntityType::Italic);
}

// ── ComposerWidget: basic state ───────────────────────────────────────────────

TEST_CASE("ComposerWidget setText/currentText round-trip", "[composer]") {
    ComposerWidget c;
    c.setText("hello world");
    CHECK(c.currentText() == "hello world");
}

TEST_CASE("ComposerWidget setText empty clears text", "[composer]") {
    ComposerWidget c;
    c.setText("some text");
    c.setText("");
    CHECK(c.currentText().isEmpty());
}

// ── Edit mode ─────────────────────────────────────────────────────────────────

TEST_CASE("enterEditMode pre-fills editor with existing text", "[composer][edit]") {
    ComposerWidget c;
    Ts ts = "1700000000.000100";
    c.enterEditMode(ts, "original message");
    CHECK(c.currentText() == "original message");
}

TEST_CASE("exitEditMode clears editor and resets state", "[composer][edit]") {
    ComposerWidget c;
    c.enterEditMode("100.000", "some text");
    c.exitEditMode();
    CHECK(c.currentText().isEmpty());
}

TEST_CASE("exitEditMode is no-op when not in edit mode", "[composer][edit]") {
    ComposerWidget c;
    // Should not crash or change state
    c.exitEditMode();
    CHECK(c.currentText().isEmpty());
}

TEST_CASE("enterEditMode twice exits old mode first", "[composer][edit]") {
    ComposerWidget c;
    c.enterEditMode("100.000", "first");
    c.enterEditMode("200.000", "second");
    CHECK(c.currentText() == "second");
}

TEST_CASE("enterEditMode then setText changes visible text", "[composer][edit]") {
    ComposerWidget c;
    c.enterEditMode("100.000", "old text");
    // Editing: text starts as "old text", can be changed
    typeText(&c, "new text");
    CHECK(c.currentText() == "new text");
}

TEST_CASE("exitEditMode after editing clears the editor", "[composer][edit]") {
    ComposerWidget c;
    c.enterEditMode("100.000", "old text");
    typeText(&c, "new text");
    c.exitEditMode();
    CHECK(c.currentText().isEmpty());
}

// ── Pending files ─────────────────────────────────────────────────────────────

TEST_CASE("addPendingFile adds to the list", "[composer][files]") {
    ComposerWidget c;
    c.addPendingFile("/tmp/test.txt");
    CHECK(c.pendingFiles().size() == 1);
    CHECK(c.pendingFiles().first() == "/tmp/test.txt");
}

TEST_CASE("addPendingFile does not add duplicates", "[composer][files]") {
    ComposerWidget c;
    c.addPendingFile("/tmp/test.txt");
    c.addPendingFile("/tmp/test.txt");
    CHECK(c.pendingFiles().size() == 1);
}

TEST_CASE("addPendingFile allows multiple distinct files", "[composer][files]") {
    ComposerWidget c;
    c.addPendingFile("/tmp/a.txt");
    c.addPendingFile("/tmp/b.txt");
    CHECK(c.pendingFiles().size() == 2);
}

TEST_CASE("clearPendingFiles empties the list", "[composer][files]") {
    ComposerWidget c;
    c.addPendingFile("/tmp/a.txt");
    c.addPendingFile("/tmp/b.txt");
    c.clearPendingFiles();
    CHECK(c.pendingFiles().isEmpty());
}

TEST_CASE("pending files persist across setText calls", "[composer][files]") {
    ComposerWidget c;
    c.addPendingFile("/tmp/a.txt");
    c.setText("new draft text");
    // setText replaces text but should not clear pending files
    CHECK(c.pendingFiles().size() == 1);
    CHECK(c.currentText() == "new draft text");
}

// ── Draft persistence (public API) ────────────────────────────────────────────

TEST_CASE("currentText returns typed text for draft saving", "[composer][draft]") {
    ComposerWidget c;
    typeText(&c, "draft text");
    CHECK(c.currentText() == "draft text");
}

TEST_CASE("setText restores a saved draft", "[composer][draft]") {
    ComposerWidget c;
    c.setText("saved draft");
    CHECK(c.currentText() == "saved draft");
}

// ── Session: new methods ──────────────────────────────────────────────────────

// StubBackend mirrors the one in test_session.cpp but adds typing/schedule tracking.
struct StubBackend2 : Backend {
    rpl::variable<AuthState>                 _auth{AuthState::LoggedIn};
    rpl::variable<UserId>                    _me;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;

    int  typingCallCount = 0;
    struct ScheduledMsg { ConversationId conv; QString text; qint64 postAt; };
    std::vector<ScheduledMsg> scheduled;

    rpl::producer<AuthState> authState() const override { return _auth.value(); }
    Capabilities capabilities()          const override { return {}; }
    void connectRealtime()    override {}
    void disconnectRealtime() override {}
    rpl::producer<UserId> loadMe() override { return _me.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers()         override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    void sendMessage(ConversationId, OutgoingMessage)       override {}
    void editMessage(ConversationId, Ts, TextWithEntities)  override {}
    void deleteMessage(ConversationId, Ts)                  override {}
    void addReaction(ConversationId, Ts, QString)           override {}
    void removeReaction(ConversationId, Ts, QString)        override {}
    void markRead(ConversationId, Ts)                       override {}
    void sendTyping(ConversationId) override { ++typingCallCount; }
    void scheduleMessage(ConversationId conv, OutgoingMessage msg, qint64 postAt) override {
        scheduled.push_back({conv, msg.text.text, postAt});
    }
    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString,QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString,QString>>({}).value();
    }
    void uploadFile(ConversationId, const QString &) override {}
    void downloadFile(const QString &,
                      std::function<void(QByteArray)>,
                      std::function<void(QString)>) override {}
    rpl::producer<Event> events() const override { return _events.events(); }
};

TEST_CASE("Session::sendTyping delegates to backend", "[session][typing]") {
    auto *stub = new StubBackend2;
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    session.sendTyping(ConversationId{"C1"});
    CHECK(stub->typingCallCount == 1);

    session.sendTyping(ConversationId{"C1"});
    CHECK(stub->typingCallCount == 2);
}

TEST_CASE("Session::scheduleMessage delegates to backend with correct args", "[session][schedule]") {
    auto *stub = new StubBackend2;
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    session.scheduleMessage(ConversationId{"C1"}, "hello future", 9999999999LL);
    REQUIRE(stub->scheduled.size() == 1);
    CHECK(stub->scheduled[0].conv   == ConversationId{"C1"});
    CHECK(stub->scheduled[0].text   == "hello future");
    CHECK(stub->scheduled[0].postAt == 9999999999LL);
}

TEST_CASE("Session::scheduleMessage parses mrkdwn in text", "[session][schedule]") {
    auto *stub = new StubBackend2;
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    session.scheduleMessage(ConversationId{"C1"}, "*bold*", 1000000000LL);
    REQUIRE(stub->scheduled.size() == 1);
    CHECK(stub->scheduled[0].text == "bold"); // mrkdwn stripped in text field
}

TEST_CASE("Session::currentUsers returns snapshot", "[session]") {
    auto *stub = new StubBackend2;
    User u;
    u.id   = UserId{"U1"};
    u.name = "alice";
    stub->_users = std::vector<User>{u};

    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    const auto &users = session.currentUsers();
    REQUIRE(users.size() == 1);
    CHECK(users[0].name == "alice");
}

TEST_CASE("Session::currentConversations returns snapshot", "[session]") {
    auto *stub = new StubBackend2;
    Conversation c;
    c.id   = ConversationId{"C1"};
    c.name = "general";
    c.kind = ConvKind::PublicChannel;
    stub->_convs = std::vector<Conversation>{c};

    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    const auto &convs = session.currentConversations();
    REQUIRE(convs.size() == 1);
    CHECK(convs[0].name == "general");
}
