// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for the message composer and related session/backend features.
// Requires QApplication because ComposerWidget inherits QWidget.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QImage>
#include <QMimeData>
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
    return c->findChild<QTextEdit *>("composerEdit");
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
    Ts             ts = "1700000000.000100";
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

// ── Clipboard paste ───────────────────────────────────────────────────────────

TEST_CASE("pasting a clipboard image attaches it as a pending file", "[composer][files][paste]") {
    ComposerWidget c;
    QImage         img(4, 4, QImage::Format_ARGB32);
    img.fill(Qt::red);
    QApplication::clipboard()->setImage(img);

    editOf(&c)->paste();

    REQUIRE(c.pendingFiles().size() == 1);
    const QString path = c.pendingFiles().first();
    CHECK(path.endsWith(".png"));
    CHECK(QFile::exists(path));
    CHECK(editOf(&c)->toPlainText().isEmpty()); // nothing inserted into the text
    QFile::remove(path);
    QApplication::clipboard()->clear();
}

TEST_CASE("pasting two clipboard images attaches two distinct files", "[composer][files][paste]") {
    ComposerWidget c;
    QImage         img(4, 4, QImage::Format_ARGB32);
    img.fill(Qt::blue);
    QApplication::clipboard()->setImage(img);

    editOf(&c)->paste();
    editOf(&c)->paste();

    REQUIRE(c.pendingFiles().size() == 2);
    CHECK(c.pendingFiles()[0] != c.pendingFiles()[1]);
    for (const QString &p : c.pendingFiles())
        QFile::remove(p);
    QApplication::clipboard()->clear();
}

TEST_CASE("pasting plain text still inserts text", "[composer][paste]") {
    ComposerWidget c;
    QApplication::clipboard()->setText("just text");

    editOf(&c)->paste();

    CHECK(c.currentText() == "just text");
    CHECK(c.pendingFiles().isEmpty());
    QApplication::clipboard()->clear();
}

TEST_CASE("pasting a copied local file attaches it", "[composer][files][paste]") {
    // A file copied in a file manager arrives as a local-file URL on the clipboard.
    const QString tmpPath = QDir::tempPath() + "/msga-paste-test.txt";
    {
        QFile f(tmpPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("hi");
    }
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(tmpPath)});
    QApplication::clipboard()->setMimeData(mime);

    ComposerWidget c;
    editOf(&c)->paste();

    REQUIRE(c.pendingFiles().size() == 1);
    CHECK(c.pendingFiles().first() == tmpPath);
    CHECK(editOf(&c)->toPlainText().isEmpty());
    QFile::remove(tmpPath);
    QApplication::clipboard()->clear();
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

    int typingCallCount = 0;
    struct ScheduledMsg {
        ConversationId conv;
        QString        text;
        qint64         postAt;
    };
    std::vector<ScheduledMsg> scheduled;

    rpl::producer<AuthState>                 authState() const override { return _auth.value(); }
    Capabilities                             capabilities() const override { return {}; }
    void                                     connectRealtime() override {}
    void                                     disconnectRealtime() override {}
    rpl::producer<UserId>                    loadMe() override { return _me.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers() override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    void sendMessage(ConversationId, OutgoingMessage) override {}
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override {}
    void sendTyping(ConversationId) override { ++typingCallCount; }
    void scheduleMessage(ConversationId conv, OutgoingMessage msg, qint64 postAt) override {
        scheduled.push_back({conv, msg.text.text, postAt});
    }
    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }
    void uploadFiles(
        ConversationId,
        const QStringList &,
        const QString &,
        std::function<void(bool, QString)> = {}
    ) override {}
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)>
    ) override {}
    rpl::producer<Event> events() const override { return _events.events(); }
};

TEST_CASE("Session::sendTyping delegates to backend", "[session][typing]") {
    auto   *stub = new StubBackend2;
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    session.sendTyping(ConversationId{"C1"});
    CHECK(stub->typingCallCount == 1);

    session.sendTyping(ConversationId{"C1"});
    CHECK(stub->typingCallCount == 2);
}

TEST_CASE(
    "Session::scheduleMessage delegates to backend with correct args", "[session][schedule]"
) {
    auto   *stub = new StubBackend2;
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    session.scheduleMessage(ConversationId{"C1"}, "hello future", 9999999999LL);
    REQUIRE(stub->scheduled.size() == 1);
    CHECK(stub->scheduled[0].conv == ConversationId{"C1"});
    CHECK(stub->scheduled[0].text == "hello future");
    CHECK(stub->scheduled[0].postAt == 9999999999LL);
}

TEST_CASE("Session::scheduleMessage parses mrkdwn in text", "[session][schedule]") {
    auto   *stub = new StubBackend2;
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    session.scheduleMessage(ConversationId{"C1"}, "*bold*", 1000000000LL);
    REQUIRE(stub->scheduled.size() == 1);
    CHECK(stub->scheduled[0].text == "bold"); // mrkdwn stripped in text field
}

TEST_CASE("Session::currentUsers returns snapshot", "[session]") {
    auto *stub = new StubBackend2;
    User  u;
    u.id         = UserId{"U1"};
    u.name       = "alice";
    stub->_users = std::vector<User>{u};

    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    const auto &users = session.currentUsers();
    REQUIRE(users.size() == 1);
    CHECK(users[0].name == "alice");
}

TEST_CASE("Session::currentConversations returns snapshot", "[session]") {
    auto        *stub = new StubBackend2;
    Conversation c;
    c.id         = ConversationId{"C1"};
    c.name       = "general";
    c.kind       = ConvKind::PublicChannel;
    stub->_convs = std::vector<Conversation>{c};

    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    const auto &convs = session.currentConversations();
    REQUIRE(convs.size() == 1);
    CHECK(convs[0].name == "general");
}

// ── Mention pills ─────────────────────────────────────────────────────────────
// The editor shows "@Name" while currentText()/sends keep the raw <@U…> token.

TEST_CASE("setText shows mention token as @id pill without a session", "[composer][mention]") {
    ComposerWidget c;
    c.setText("ping <@U123ABC> ok");
    CHECK(editOf(&c)->toPlainText() == "ping @U123ABC ok");
    CHECK(c.currentText() == "ping <@U123ABC> ok"); // wire format unchanged
}

TEST_CASE("setText uses the |label of a labeled mention token", "[composer][mention]") {
    ComposerWidget c;
    c.setText("hi <@U1|maria>!");
    CHECK(editOf(&c)->toPlainText() == "hi @maria!");
    CHECK(c.currentText() == "hi <@U1|maria>!");
}

TEST_CASE("setText resolves mention display name via session", "[composer][mention]") {
    auto *stub = new StubBackend2;
    User  u;
    u.id          = UserId{"U42"};
    u.name        = "maria";
    u.displayName = "Maria O";
    stub->_users  = std::vector<User>{u};

    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();

    ComposerWidget c;
    c.setSession(&session);
    c.setText("hi <@U42>");
    CHECK(editOf(&c)->toPlainText() == "hi @Maria O");
    CHECK(c.currentText() == "hi <@U42>");
}

TEST_CASE("enterEditMode keeps raw mention tokens through currentText", "[composer][mention]") {
    ComposerWidget c;
    c.enterEditMode("100.000", "ask <@U7> about it");
    CHECK(editOf(&c)->toPlainText() == "ask @U7 about it");
    CHECK(c.currentText() == "ask <@U7> about it");
}

TEST_CASE("hand-edited mention pill falls back to its literal text", "[composer][mention]") {
    ComposerWidget c;
    c.setText("<@U1|bob>");
    QTextEdit *ed = editOf(&c);
    REQUIRE(ed->toPlainText() == "@bob");
    // Delete the last character inside the pill — it no longer matches the
    // stored display string, so it must serialize as what the user sees.
    auto tc = ed->textCursor();
    tc.movePosition(QTextCursor::End);
    tc.deletePreviousChar();
    CHECK(c.currentText() == "@bo");
}

TEST_CASE("mention serialization preserves newlines", "[composer][mention]") {
    ComposerWidget c;
    c.setText("line one\ncc <@U9|zoe>\nline three");
    CHECK(editOf(&c)->toPlainText() == "line one\ncc @zoe\nline three");
    CHECK(c.currentText() == "line one\ncc <@U9|zoe>\nline three");
}

TEST_CASE("adjacent mention pills serialize independently", "[composer][mention]") {
    ComposerWidget c;
    c.setText("<@U1|ann><@U1|ann>");
    CHECK(editOf(&c)->toPlainText() == "@ann@ann");
    CHECK(c.currentText() == "<@U1|ann><@U1|ann>");
}

// ── Inline :emoji: autocomplete ───────────────────────────────────────────────

#include "ui/composer/mention_completer.h"
#include <QEventLoop>
#include <QKeyEvent>
#include <QPushButton>
#include <QTimer>

// Let the completer's QTimer::singleShot(0) fire.
static void pumpEvents(int ms = 30) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The completer reacts to KeyRelease in the editor's event filter.
// The composer must be shown: the completer is a plain child widget, so its
// isVisible() is false while any ancestor is hidden.
static void releaseKey(ComposerWidget *c, int key, const QString &txt) {
    if (!c->isVisible())
        c->show();
    QKeyEvent ev(QEvent::KeyRelease, key, Qt::NoModifier, txt);
    QApplication::sendEvent(editOf(c), &ev);
    pumpEvents();
}

static MentionCompleter *completerOf(ComposerWidget *c) {
    return c->findChild<MentionCompleter *>();
}

TEST_CASE("typing :letters at text start opens the emoji completer", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    CHECK(comp->isVisible());
    const auto rows = comp->findChildren<QPushButton *>();
    REQUIRE(!rows.isEmpty());
    CHECK(rows.first()->text().contains(":fire:"));
    CHECK(rows.first()->text().contains("🔥"));
}

TEST_CASE("emoji completer triggers after a space", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, "hello :wav");
    releaseKey(&c, Qt::Key_V, "v");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    CHECK(comp->isVisible());
}

TEST_CASE("colon inside a word does not trigger the completer", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, "abc:fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    CHECK((!comp || !comp->isVisible()));
}

TEST_CASE("uppercase after colon does not trigger the completer", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":D");
    releaseKey(&c, Qt::Key_D, "D");
    auto *comp = completerOf(&c);
    CHECK((!comp || !comp->isVisible()));
}

TEST_CASE("completer hides when the word stops looking like an emoji code", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    typeText(&c, ":fir)");
    releaseKey(&c, Qt::Key_ParenRight, ")");
    CHECK(!comp->isVisible());
}

TEST_CASE("completer hides when nothing matches anymore", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    typeText(&c, ":firzzz");
    releaseKey(&c, Qt::Key_Z, "z");
    CHECK(!comp->isVisible());
}

TEST_CASE("Enter inserts the selected emoji glyph", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &enter);
    CHECK(editOf(&c)->toPlainText() == QString("🔥 "));
    CHECK(!comp->isVisible());
}

TEST_CASE("Down then Enter inserts the second suggestion", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":fi");
    releaseKey(&c, Qt::Key_I, "i");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());
    const auto rows = comp->findChildren<QPushButton *>();
    REQUIRE(rows.size() >= 2);

    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &down);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &enter);

    const QString text = editOf(&c)->toPlainText();
    CHECK(!text.startsWith(':'));
    CHECK(rows[1]->text().startsWith(text.trimmed()));
}

TEST_CASE("Escape dismisses the completer without inserting", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &esc);
    CHECK(!comp->isVisible());
    CHECK(editOf(&c)->toPlainText() == ":fir");
}

TEST_CASE("completer is a plain child widget, not a window", "[composer][emoji]") {
    // A real window steals focus on show (the editor's FocusOut then dismisses
    // it instantly — the "blink" bug) and cannot be positioned on Wayland.
    ComposerWidget c;
    typeText(&c, ":smi");
    releaseKey(&c, Qt::Key_I, "i");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    CHECK(comp->isVisible());
    CHECK(!comp->isWindow());
    CHECK(comp->focusPolicy() == Qt::NoFocus);
}

TEST_CASE("completer stays anchored at the colon while typing", "[composer][emoji]") {
    ComposerWidget c;
    typeText(&c, ":s");
    releaseKey(&c, Qt::Key_S, "s");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());
    const QPoint posAfterOneLetter = comp->pos();

    typeText(&c, ":sm");
    releaseKey(&c, Qt::Key_M, "m");
    REQUIRE(comp->isVisible());
    CHECK(comp->pos().x() == posAfterOneLetter.x());
}

TEST_CASE("completer sits above the editor, not far from it", "[composer][emoji]") {
    ComposerWidget c;
    c.resize(600, 120);
    c.show();
    typeText(&c, ":fir");
    releaseKey(&c, Qt::Key_R, "r");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    QTextEdit  *ed = editOf(&c);
    QTextCursor tc = ed->textCursor();
    tc.setPosition(0); // the ':' trigger
    // Anchor mapped into the completer's parent coordinates, the same space
    // as comp->pos().
    const QPoint anchor =
        comp->parentWidget()->mapFromGlobal(ed->mapToGlobal(ed->cursorRect(tc).topLeft()));
    // Bottom edge hugs the trigger line (4px gap) and is left-aligned with
    // ':' (subject to the clamp keeping it inside the parent).
    CHECK(
        comp->pos().x() ==
        qBound(0, anchor.x(), qMax(0, comp->parentWidget()->width() - comp->width()))
    );
    CHECK(comp->pos().y() + comp->height() + 4 == anchor.y());
}

// Simulates real typing: KeyPress (which inserts the char via QTextEdit's own
// handler) followed by KeyRelease — unlike typeText() which uses setPlainText.
static void typeChar(ComposerWidget *c, int key, const QString &txt) {
    if (!c->isVisible())
        c->show();
    QTextEdit *ed = editOf(c);
    QKeyEvent  press(QEvent::KeyPress, key, Qt::NoModifier, txt);
    QApplication::sendEvent(ed, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, txt);
    QApplication::sendEvent(ed, &release);
    pumpEvents();
}

// Types ':', 's', 'm' as real key events, stops, and verifies the list is
// still open at full size — it must not collapse on the second keystroke.
TEST_CASE("completer stays open after typing :sm and stopping", "[composer][emoji]") {
    ComposerWidget c;
    typeChar(&c, Qt::Key_Colon, ":");
    typeChar(&c, Qt::Key_S, "s");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    CHECK(comp->isVisible());

    typeChar(&c, Qt::Key_M, "m");
    CHECK(editOf(&c)->toPlainText() == ":sm");
    CHECK(comp->isVisible());

    // The rebuilt rows must actually be visible: children added to an
    // already-visible parent stay hidden unless explicitly shown, which
    // collapsed the popup to a sliver on the second keystroke.
    const auto rows = comp->findChildren<QPushButton *>();
    REQUIRE(!rows.isEmpty());
    for (const auto *row : rows)
        CHECK(row->isVisible());
    CHECK(comp->height() > 50);
}
