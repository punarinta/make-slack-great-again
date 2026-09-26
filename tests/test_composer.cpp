// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for the message composer and related session/backend features.
// Requires QApplication because ComposerWidget inherits QWidget.

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"
#include "stub_backend.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QKeyEvent>
#include <QUrl>
#include <QImage>
#include <QMimeData>
#include <QStandardPaths>
#include <QTextEdit>
#include <QWindow>

#include "ui/composer/composer_widget.h"
#include "ui/composer/undo_send_pill.h"
#include "ui/shortcuts.h"
#include "session/session.h"
#include "backend/backend.h"
#include "text/mrkdwn_parser.h"
#include "rpl/variable.h"

// ── Custom main (need QApplication before Catch runs) ────────────────────────

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-composer");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
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

// ── Drag-and-drop attachments ─────────────────────────────────────────────────

// Qt delivers drag-and-drop at the QWindow level: QWidgetWindow routes the
// events to the widget under the cursor (events sent straight to a QWidget are
// discarded by QApplication::notify). A DragEnter must precede the Drop so the
// window knows the drag target.
static bool sendDrop(ComposerWidget *c, const QMimeData *mime) {
    c->show();
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(c->windowHandle(), &enter);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(c->windowHandle(), &drop);
    return drop.isAccepted();
}

TEST_CASE("dropping a local file attaches it", "[composer][files][drop]") {
    const QString tmpPath = QDir::tempPath() + "/msga-drop-test.txt";
    {
        QFile f(tmpPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("hi");
    }
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(tmpPath)});

    ComposerWidget c;
    CHECK(sendDrop(&c, &mime));
    REQUIRE(c.pendingFiles().size() == 1);
    CHECK(c.pendingFiles().first() == tmpPath);
    QFile::remove(tmpPath);
}

TEST_CASE("dropping raw image data attaches it as a PNG", "[composer][files][drop]") {
    QImage img(4, 4, QImage::Format_RGB32);
    img.fill(Qt::blue);
    QMimeData mime;
    mime.setImageData(img);

    ComposerWidget c;
    CHECK(sendDrop(&c, &mime));
    REQUIRE(c.pendingFiles().size() == 1);
    CHECK(c.pendingFiles().first().endsWith(".png"));
    QFile::remove(c.pendingFiles().first());
}

TEST_CASE("drag of plain text is not accepted", "[composer][drop]") {
    QMimeData mime;
    mime.setText("just text");

    ComposerWidget c;
    CHECK_FALSE(sendDrop(&c, &mime));
    CHECK(c.pendingFiles().isEmpty());
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

// ── Draft stash: takeDraft / restoreDraft ─────────────────────────────────────
//
// The conversation-switch contract, and a security boundary: input staged for
// one chat (text, attachments, subject) must never be able to ride into a send
// from another chat. takeDraft() is the single leave-a-conversation entry point
// and must leave the composer provably empty; restoreDraft() must replace the
// composer's content wholesale, never merge into it.

TEST_CASE("takeDraft returns the staged input and empties the composer", "[composer][draft]") {
    ComposerWidget c;
    typeText(&c, "unsent secret");
    c.addPendingFile("/tmp/secret.png");

    const ComposerDraft d = c.takeDraft();
    CHECK(d.text == "unsent secret");
    CHECK(d.files == QStringList{"/tmp/secret.png"});

    // The security half: nothing staged survives inside the composer.
    CHECK(c.currentText().isEmpty());
    CHECK(c.pendingFiles().isEmpty());

    // And a second take finds nothing left to carry anywhere.
    CHECK(c.takeDraft().isEmpty());
}

TEST_CASE("Enter after takeDraft sends nothing", "[composer][draft]") {
    // The end of the leak: even if the user hits Enter right after a switch,
    // there is nothing left in the composer that could go out.
    ComposerWidget c;
    typeText(&c, "secret");
    c.addPendingFile("/tmp/secret.png");

    int sends = 0, uploads = 0;
    QObject::connect(&c, &ComposerWidget::sendRequested, &c, [&](const QString &) { ++sends; });
    QObject::connect(
        &c, &ComposerWidget::uploadRequested, &c, [&](const QStringList &, const QString &) {
            ++uploads;
        }
    );

    c.takeDraft();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &enter);

    CHECK(sends == 0);
    CHECK(uploads == 0);
}

TEST_CASE("restoreDraft replaces any leftover content wholesale", "[composer][draft]") {
    // Defense in depth: even if some leave-path missed its takeDraft(), the
    // restore on open must make the composer hold exactly the incoming
    // conversation's draft — not a merge with whatever was left behind.
    ComposerWidget c;
    typeText(&c, "text that was never stashed");
    c.addPendingFile("/tmp/leftover.png");

    ComposerDraft d;
    d.text  = "the right draft";
    d.files = QStringList{"/tmp/right.png"};
    c.restoreDraft(d);

    CHECK(c.currentText() == "the right draft");
    CHECK(c.pendingFiles() == QStringList{"/tmp/right.png"});
}

TEST_CASE("restoring an empty draft leaves an empty composer", "[composer][draft]") {
    ComposerWidget c;
    typeText(&c, "leftover");
    c.addPendingFile("/tmp/leftover.png");

    c.restoreDraft({});

    CHECK(c.currentText().isEmpty());
    CHECK(c.pendingFiles().isEmpty());
}

TEST_CASE("take/restore round-trips mention tokens unchanged", "[composer][draft]") {
    ComposerWidget c;
    c.setText("ping <@U123> now");
    const ComposerDraft d = c.takeDraft();
    CHECK(d.text == "ping <@U123> now");
    c.restoreDraft(d);
    CHECK(c.currentText() == "ping <@U123> now");
}

TEST_CASE("takeDraft discards an in-progress edit instead of stashing it", "[composer][draft]") {
    // Edit-mode text belongs to an existing message, not to unsent input;
    // stashing it would resurface that message as "your draft" elsewhere. Files
    // attached while editing are new user content and do travel.
    ComposerWidget c;
    c.enterEditMode("100.000", "someone's published message");
    typeText(&c, "half-finished correction");
    c.addPendingFile("/tmp/new-file.png");

    const ComposerDraft d = c.takeDraft();
    CHECK(d.text.isEmpty());
    CHECK(d.files == QStringList{"/tmp/new-file.png"});
    CHECK(c.currentText().isEmpty());
}

TEST_CASE("the subject line travels with the draft and is cleared by take", "[composer][draft]") {
    ComposerWidget c;
    c.setSubjectVisible(true);
    c.setSubjectText("Custom subject");
    typeText(&c, "mail body");

    const ComposerDraft d = c.takeDraft();
    CHECK(d.subject == "Custom subject");
    CHECK(c.subjectText().isEmpty()); // must not ride into the next conversation

    c.restoreDraft(d);
    CHECK(c.subjectText() == "Custom subject");
    CHECK(c.currentText() == "mail body");
}

TEST_CASE("a subject-less draft keeps the host's reply prefill", "[composer][draft]") {
    // openConversation() prefills "Re: …" for the incoming conversation before
    // restoring; a draft without a subject of its own must not wipe it.
    ComposerWidget c;
    c.setSubjectVisible(true);
    c.setSubjectText("Re: incoming thread");

    ComposerDraft d;
    d.text = "body only";
    c.restoreDraft(d);

    CHECK(c.subjectText() == "Re: incoming thread");
    CHECK(c.currentText() == "body only");
}

// ── Session: new methods ──────────────────────────────────────────────────────

// StubBackend mirrors the one in test_session.cpp but adds typing/schedule tracking.
struct StubBackend2 : msga_test::StubBackendBase {
    int typingCallCount = 0;
    struct ScheduledMsg {
        ConversationId conv;
        QString        text;
        qint64         postAt;
    };
    std::vector<ScheduledMsg> scheduled;

    void sendTyping(ConversationId) override { ++typingCallCount; }
    void scheduleMessage(ConversationId conv, OutgoingMessage msg, qint64 postAt) override {
        scheduled.push_back({conv, msg.text.text, postAt});
    }
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

// Session persists its roster to an on-disk per-team cache, and the roster merge
// retains cached users the backend snapshot omits (Slack Connect externals). Wipe
// the team cache so users saved by other cases (or a previous run) can't leak in.
static void wipeTeamCache(const QString &teamId = QStringLiteral("T_TEST")) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId)
        .removeRecursively();
}

TEST_CASE("Session::currentUsers returns snapshot", "[session]") {
    wipeTeamCache();
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
    wipeTeamCache();
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

TEST_CASE("setText shows a GIPHY link token as a GIF pill", "[composer][gif]") {
    ComposerWidget c;
    c.setText("look <https://media.giphy.com/media/abc123/giphy.gif|Dancing cat> !");
    CHECK(editOf(&c)->toPlainText() == QString::fromUtf8("look GIF · Dancing cat !"));
    CHECK(c.currentText() == "look <https://media.giphy.com/media/abc123/giphy.gif|Dancing cat> !");
}

TEST_CASE("setText shows an untitled GIPHY link as a bare GIF pill", "[composer][gif]") {
    ComposerWidget c;
    c.setText("<https://i.giphy.com/abc123.gif>");
    CHECK(editOf(&c)->toPlainText() == "GIF");
    CHECK(c.currentText() == "<https://i.giphy.com/abc123.gif>");
}

TEST_CASE("setText leaves other labelled links literal", "[composer][gif]") {
    ComposerWidget c;
    c.setText("<https://example.com/e|Stand-Up>");
    CHECK(editOf(&c)->toPlainText() == "<https://example.com/e|Stand-Up>");
    CHECK(c.currentText() == "<https://example.com/e|Stand-Up>");
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
#include "ui/mention_popup/mention_popup.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
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

TEST_CASE("broadcast autocomplete highlights and sends Slack tokens", "[composer][mention]") {
    Session session(std::make_unique<StubBackend2>(), "T_TEST");
    session.start();
    QWidget host;
    host.resize(600, 600);
    ComposerWidget c(&host);
    c.setGeometry(0, 450, 600, 150);
    c.setSession(&session);
    host.show();

    for (const auto &name : {"channel", "here", "everyone"}) {
        const QString alias = "@" + QString(name);
        const QString token = "<!" + QString(name) + ">";
        typeText(&c, alias);
        releaseKey(&c, Qt::Key_E, "e");
        auto *popup = host.findChild<MentionPopup *>();
        REQUIRE(popup);
        REQUIRE(popup->isOpen());
        REQUIRE(popup->handleKey(Qt::Key_Return));
        CHECK(editOf(&c)->toPlainText() == alias + " ");
        CHECK(c.currentText() == token + " ");
        QTextCursor cursor(editOf(&c)->document());
        cursor.setPosition(1);
        CHECK(cursor.charFormat().foreground().color() == Th::c().message.replyLink);

        // Draft restoration and editing must keep the visible label and wire token.
        const auto draft = c.takeDraft();
        c.restoreDraft(draft);
        CHECK(editOf(&c)->toPlainText() == alias + " ");
        CHECK(c.currentText() == token + " ");
        QString sent;
        auto    connection =
            QObject::connect(&c, &ComposerWidget::sendRequested, [&](const QString &text) {
                sent = text;
            });
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editOf(&c), &enter);
        CHECK(sent == token);
        QObject::disconnect(connection);

        c.enterEditMode("100.000", token);
        CHECK(editOf(&c)->toPlainText() == alias);
        CHECK(c.currentText() == token);
        c.exitEditMode();
    }
}

TEST_CASE("mention rows show an elided description as a tooltip", "[composer][mention]") {
    auto *stub = new StubBackend2;
    User  user;
    user.id          = UserId{"U7"};
    user.name        = "ann";
    user.displayName = "Ann Example";
    stub->_users     = std::vector<User>{user};
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();
    QWidget host;
    host.resize(600, 600);
    ComposerWidget c(&host);
    c.setGeometry(0, 450, 600, 150);
    c.setSession(&session);
    host.show();

    // The hovered row gains the Enter badge, which elides the long @channel
    // description but leaves a short user handle whole: only the former gets
    // a tooltip, and leaving the row hides it again.
    struct Probe {
        const char *typed;
        int         key;
        const char *keyText;
        const char *rowName;
        bool        expectTip;
    };
    for (const Probe &pr :
         {Probe{"@chan", Qt::Key_N, "n", "@channel", true},
          Probe{"@ann", Qt::Key_N, "n", "@Ann Example", false}}) {
        editOf(&c)->clear();
        typeText(&c, pr.typed);
        releaseKey(&c, pr.key, pr.keyText);
        auto *popup = host.findChild<MentionPopup *>();
        REQUIRE(popup);
        REQUIRE(popup->isOpen());
        QWidget *row = nullptr;
        for (auto *w : popup->findChildren<QWidget *>())
            if (w->accessibleName() == pr.rowName)
                row = w;
        REQUIRE(row);
        const QPointF centre(row->width() / 2.0, row->height() / 2.0);
        QEnterEvent   enter(centre, centre, row->mapToGlobal(centre.toPoint()));
        QApplication::sendEvent(row, &enter);
        auto *tip = host.findChild<PopupTooltip *>();
        REQUIRE(tip);
        CHECK(tip->isVisible() == pr.expectTip);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(row, &leave);
        CHECK_FALSE(tip->isVisible());
    }
}

TEST_CASE(
    "thread broadcasts explain the restriction and insert literal text", "[composer][mention]"
) {
    auto *stub = new StubBackend2;
    User  user;
    user.id      = UserId{"U42"};
    user.name    = "maria";
    stub->_users = std::vector<User>{user};
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    session.start();
    QWidget host;
    host.resize(400, 600);
    ComposerWidget c(&host);
    c.setGeometry(0, 450, 400, 150);
    c.setSession(&session);
    c.setThreadMode(true);
    host.show();

    for (const auto &alias : {"@channel", "@here", "@everyone"}) {
        typeText(&c, alias);
        releaseKey(&c, Qt::Key_E, "e");
        auto *popup = host.findChild<MentionPopup *>();
        REQUIRE(popup);
        REQUIRE(popup->isOpen());
        bool notice = false;
        for (auto *row : popup->findChildren<QWidget *>())
            if (row->accessibleName() == alias)
                notice = row->accessibleDescription() == "Disabled in threads";
        CHECK(notice);
        CHECK(popup->width() <= host.width());
        REQUIRE(popup->handleKey(Qt::Key_Tab));
        CHECK(c.currentText() == QString(alias) + " ");
        QTextCursor cursor(editOf(&c)->document());
        cursor.setPosition(1);
        CHECK_FALSE(cursor.charFormat().hasProperty(QTextFormat::UserProperty));
    }

    typeText(&c, "@maria");
    releaseKey(&c, Qt::Key_A, "a");
    auto *popup = host.findChild<MentionPopup *>();
    REQUIRE(popup->isOpen());
    REQUIRE(popup->handleKey(Qt::Key_Return));
    CHECK(c.currentText() == "<@U42> ");

    for (const auto kind : {ConvKind::Im, ConvKind::Mpim}) {
        c.setConvKind(kind);
        typeText(&c, "@here");
        releaseKey(&c, Qt::Key_E, "e");
        CHECK_FALSE(popup->isOpen());
    }
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
    // In production the composer lives at the bottom of a tall msgArea, which is
    // the completer's parent and gives it the room to float above the trigger.
    // (A bare standalone composer is shorter than the popup — see the next test.)
    QWidget host;
    host.resize(600, 600);
    auto *c = new ComposerWidget(&host);
    c->setGeometry(0, 480, 600, 120);
    host.show();
    typeText(c, ":fir");
    releaseKey(c, Qt::Key_R, "r");
    auto *comp = host.findChild<MentionCompleter *>();
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    QTextEdit  *ed = editOf(c);
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
    CHECK(comp->pos().y() >= 0); // never clipped off the top of the parent
}

TEST_CASE(
    "completer flips below and stays inside the parent with no room above", "[composer][emoji]"
) {
    // Composer at the very top of its parent: there is no room above the trigger
    // for the popup. The old code positioned it at a negative y (top rows clipped
    // away by the parent widget); placePopup now flips it below and keeps it
    // fully inside the parent.
    QWidget host;
    host.resize(600, 600);
    auto *c = new ComposerWidget(&host);
    c->setGeometry(0, 0, 600, 120);
    host.show();
    typeText(c, ":fir");
    releaseKey(c, Qt::Key_R, "r");
    auto *comp = host.findChild<MentionCompleter *>();
    REQUIRE(comp);
    REQUIRE(comp->isVisible());
    CHECK(comp->pos().y() >= 0);
    CHECK(comp->pos().y() + comp->height() <= host.height());
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

// ── Inline #channel autocomplete ──────────────────────────────────────────────

// Builds a session with one public and one private channel, plus a DM that must
// never appear among the channel suggestions.
static Session *channelSession() {
    auto        *stub = new StubBackend2;
    Conversation pub;
    pub.id   = ConversationId{"C1"};
    pub.name = "general";
    pub.kind = ConvKind::PublicChannel;
    Conversation priv;
    priv.id   = ConversationId{"G1"};
    priv.name = "secret-stuff";
    priv.kind = ConvKind::PrivateChannel;
    Conversation dm;
    dm.id        = ConversationId{"D1"};
    dm.name      = "ann";
    dm.kind      = ConvKind::Im;
    stub->_convs = std::vector<Conversation>{pub, priv, dm};

    auto *session = new Session(std::unique_ptr<Backend>(stub), "T_TEST");
    session->start();
    return session;
}

TEST_CASE("typing # at text start opens the channel completer", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    typeText(&c, "#");
    releaseKey(&c, Qt::Key_NumberSign, "#");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    CHECK(comp->isVisible()); // a bare '#' lists every channel
    delete session;
}

TEST_CASE("# inside a word does not trigger the channel completer", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    typeText(&c, "abc#gen");
    releaseKey(&c, Qt::Key_N, "n");
    auto *comp = completerOf(&c);
    CHECK((!comp || !comp->isVisible()));
    delete session;
}

TEST_CASE("# after a space triggers the channel completer", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    typeText(&c, "join #gen");
    releaseKey(&c, Qt::Key_N, "n");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    CHECK(comp->isVisible());
    delete session;
}

TEST_CASE("Enter inserts the selected channel as a pill", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    typeText(&c, "#gen");
    releaseKey(&c, Qt::Key_N, "n");
    auto *comp = completerOf(&c);
    REQUIRE(comp);
    REQUIRE(comp->isVisible());

    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &enter);
    // The editor shows the channel name as a pill; the wire format keeps Slack's
    // channel link, with a trailing space.
    CHECK(editOf(&c)->toPlainText() == "#general ");
    CHECK(c.currentText() == "<#C1|general> ");
    CHECK(!comp->isVisible());
    delete session;
}

TEST_CASE("Backspace deletes the whole channel pill at once", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    typeText(&c, "#gen");
    releaseKey(&c, Qt::Key_N, "n");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editOf(&c), &enter);
    REQUIRE(editOf(&c)->toPlainText() == "#general ");

    // Place the cursor just after the pill (before the trailing space) and
    // backspace once — the entire "#general" block disappears.
    QTextEdit *ed = editOf(&c);
    auto       tc = ed->textCursor();
    tc.setPosition(QString("#general").size());
    ed->setTextCursor(tc);
    QKeyEvent bs(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(ed, &bs);
    CHECK(ed->toPlainText() == " ");
    CHECK(c.currentText().trimmed().isEmpty());
    delete session;
}

TEST_CASE("a channel pill round-trips through setText", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    c.setText("see <#C1|general> please");
    // The raw token shows as a "#general" pill but serializes back unchanged.
    CHECK(editOf(&c)->toPlainText() == "see #general please");
    CHECK(c.currentText() == "see <#C1|general> please");
    delete session;
}

TEST_CASE("a bare channel link resolves its name from the session", "[composer][channel]") {
    auto          *session = channelSession();
    ComposerWidget c;
    c.setSession(session);
    // Slack often stores the link without the name; the composer recovers it.
    c.setText("see <#C1> now");
    CHECK(editOf(&c)->toPlainText() == "see #general now");
    delete session;
}

// ── Arrow-key boundary navigation ─────────────────────────────────────────────

// Vertical cursor movement needs a laid-out document — show the widget and
// let the layout settle before sending arrow keys.
static void showWithText(ComposerWidget *c, const QString &text) {
    c->show();
    typeText(c, text);
    pumpEvents();
}

static void placeCursor(ComposerWidget *c, int pos) {
    QTextEdit *ed = editOf(c);
    auto       tc = ed->textCursor();
    tc.setPosition(pos);
    ed->setTextCursor(tc);
}

static void sendKey(ComposerWidget *c, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(editOf(c), &press);
}

TEST_CASE("Up on the first line moves the cursor to the line start", "[composer][nav]") {
    ComposerWidget c;
    showWithText(&c, "first line\nsecond line");
    placeCursor(&c, 5); // middle of "first"
    sendKey(&c, Qt::Key_Up);
    CHECK(editOf(&c)->textCursor().position() == 0);
}

TEST_CASE("Down on the last line moves the cursor to the line end", "[composer][nav]") {
    ComposerWidget c;
    showWithText(&c, "first line\nsecond line");
    const int end = editOf(&c)->toPlainText().size();
    placeCursor(&c, end - 4); // middle of "second line"
    sendKey(&c, Qt::Key_Down);
    CHECK(editOf(&c)->textCursor().position() == end);
}

TEST_CASE("Up on a non-first line still moves up a line", "[composer][nav]") {
    ComposerWidget c;
    showWithText(&c, "first line\nsecond line");
    const int secondLineMid = QString("first line\nsec").size();
    placeCursor(&c, secondLineMid);
    sendKey(&c, Qt::Key_Up);
    const auto tc = editOf(&c)->textCursor();
    CHECK(tc.blockNumber() == 0);
    CHECK(tc.position() != 0); // landed inside line 1, not snapped to its start
}

// ── Schedule-send capability gating ───────────────────────────────────────────

// The schedule-send dropdown (chevron beside Send) is Slack-only; MainWindow
// hides it on backends whose Capabilities::scheduledSend is false so it isn't a
// dead control. Verify setScheduleVisible drives the button's visibility.
TEST_CASE("setScheduleVisible toggles the schedule-send dropdown", "[composer][schedule]") {
    ComposerWidget c;
    showWithText(&c, "");
    auto *drop = c.findChild<QPushButton *>("composerScheduleBtn");
    REQUIRE(drop);

    // Shown by default (Slack path leaves it visible).
    CHECK(drop->isVisible());

    c.setScheduleVisible(false);
    CHECK_FALSE(drop->isVisible());

    c.setScheduleVisible(true);
    CHECK(drop->isVisible());
}

// ── Send key: Enter vs Ctrl+Enter ────────────────────────────────────────────

namespace {
void sendKeyMod(ComposerWidget *c, int key, Qt::KeyboardModifiers mods) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(editOf(c), &press);
}
struct CtrlEnterMode {
    explicit CtrlEnterMode(bool on) { Ui::Shortcuts::setCtrlEnterSends(on); }
    ~CtrlEnterMode() { Ui::Shortcuts::setCtrlEnterSends(false); }
};
} // namespace

TEST_CASE(
    "Enter sends by default, Shift+Enter and Ctrl+Enter as documented", "[composer][sendkey]"
) {
    CtrlEnterMode  mode(false);
    ComposerWidget c;
    QStringList    sent;
    QObject::connect(&c, &ComposerWidget::sendRequested, &c, [&](const QString &t) { sent << t; });
    showWithText(&c, "hello");

    sendKeyMod(&c, Qt::Key_Return, Qt::ShiftModifier);
    CHECK(sent.isEmpty()); // newline, not a send
    CHECK_FALSE(c.currentText().isEmpty());

    sendKey(&c, Qt::Key_Return);
    REQUIRE(sent.size() == 1);
    CHECK(sent.constFirst().startsWith("hello"));
    CHECK(c.currentText().isEmpty());

    typeText(&c, "again");
    sendKeyMod(&c, Qt::Key_Return, Qt::ControlModifier);
    REQUIRE(sent.size() == 2); // Ctrl+Enter sends in the default mode too
}

TEST_CASE(
    "With the Ctrl+Enter option, Enter inserts a newline and Ctrl+Enter sends",
    "[composer][sendkey]"
) {
    CtrlEnterMode  mode(true);
    ComposerWidget c;
    int            sends = 0;
    QObject::connect(&c, &ComposerWidget::sendRequested, &c, [&](const QString &) { ++sends; });
    showWithText(&c, "first");

    sendKey(&c, Qt::Key_Return);
    CHECK(sends == 0);
    CHECK(editOf(&c)->document()->blockCount() == 2); // the editor took the Enter

    sendKeyMod(&c, Qt::Key_Return, Qt::ControlModifier);
    CHECK(sends == 1);
    CHECK(c.currentText().isEmpty());
}

// ── Undo send ─────────────────────────────────────────────────────────────────
//
// The host offers the undo from inside its sendRequested handler; Ctrl+Z in the
// empty editor (or a click on the chip) runs it and puts the sent input back.

namespace {
// Host stand-in: every send is offered for undo; `undone` records which ones
// the composer took back.
struct UndoHost {
    QStringList undone;
    void        wire(ComposerWidget *c) {
        QObject::connect(c, &ComposerWidget::sendRequested, c, [this, c](const QString &t) {
            c->offerUndoSend([this, t] { undone << t; });
        });
        QObject::connect(
            c,
            &ComposerWidget::uploadRequested,
            c,
            [this, c](const QStringList &files, const QString &t) {
                c->offerUndoSend([this, files, t] { undone << files.join(',') + '|' + t; });
            }
        );
    }
};
void ctrlZ(ComposerWidget *c) {
    sendKeyMod(c, Qt::Key_Z, Qt::ControlModifier);
}
} // namespace

TEST_CASE("Ctrl+Z after a send runs the undo and restores the text", "[composer][undosend]") {
    ComposerWidget c;
    UndoHost       host;
    host.wire(&c);
    showWithText(&c, "oops wrong chat");

    sendKey(&c, Qt::Key_Return);
    REQUIRE(c.currentText().isEmpty());
    CHECK(c.undoSendOffered());
    auto *pill = c.window()->findChild<UndoSendPill *>("undoSendPill");
    REQUIRE(pill);
    CHECK(pill->isVisible());

    ctrlZ(&c);
    CHECK(host.undone == QStringList{"oops wrong chat"});
    CHECK(c.currentText() == "oops wrong chat");
    CHECK_FALSE(c.undoSendOffered());
    CHECK_FALSE(pill->isVisible());

    // A second Ctrl+Z (now the editor's own undo) must not re-run the send undo.
    ctrlZ(&c);
    CHECK(host.undone.size() == 1);
}

TEST_CASE("Clicking the chip undoes and keeps text typed since in place", "[composer][undosend]") {
    ComposerWidget c;
    UndoHost       host;
    host.wire(&c);
    showWithText(&c, "sent text");
    sendKey(&c, Qt::Key_Return);
    typeText(&c, "typed after");

    // With text in the editor Ctrl+Z is the editor's undo, not ours.
    ctrlZ(&c);
    CHECK(host.undone.isEmpty());
    CHECK(c.undoSendOffered());

    auto *pill = c.window()->findChild<UndoSendPill *>("undoSendPill");
    REQUIRE(pill);
    QMouseEvent click(
        QEvent::MouseButtonPress,
        QPointF(5, 5),
        pill->mapToGlobal(QPoint(5, 5)),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(pill, &click);
    CHECK(host.undone == QStringList{"sent text"});
    CHECK(c.currentText() == "sent text\ntyped after");
}

TEST_CASE("Undo restores attachments alongside the text", "[composer][undosend]") {
    ComposerWidget c;
    UndoHost       host;
    host.wire(&c);
    showWithText(&c, "with a file");
    c.addPendingFile("/tmp/msga-test-undo.png");
    sendKey(&c, Qt::Key_Return);
    REQUIRE(c.pendingFiles().isEmpty());
    REQUIRE(c.undoSendOffered());

    ctrlZ(&c);
    CHECK(host.undone == QStringList{"/tmp/msga-test-undo.png|with a file"});
    CHECK(c.pendingFiles() == QStringList{"/tmp/msga-test-undo.png"});
    CHECK(c.currentText() == "with a file");
}

TEST_CASE("A new send supersedes the previous undo offer", "[composer][undosend]") {
    ComposerWidget c;
    UndoHost       host;
    host.wire(&c);
    showWithText(&c, "first");
    sendKey(&c, Qt::Key_Return);
    typeText(&c, "second");
    sendKey(&c, Qt::Key_Return);

    ctrlZ(&c);
    CHECK(host.undone == QStringList{"second"});
    CHECK(c.currentText() == "second");
    ctrlZ(&c); // nothing left to take back
    CHECK(host.undone.size() == 1);
}

TEST_CASE("Leaving the conversation withdraws the undo offer", "[composer][undosend]") {
    // The chip's restore would otherwise drop the sent text into the NEXT chat.
    ComposerWidget c;
    UndoHost       host;
    host.wire(&c);
    showWithText(&c, "for chat A");
    sendKey(&c, Qt::Key_Return);
    REQUIRE(c.undoSendOffered());

    CHECK(c.takeDraft().isEmpty());
    CHECK_FALSE(c.undoSendOffered());
    ctrlZ(&c);
    CHECK(host.undone.isEmpty());
    CHECK(c.currentText().isEmpty());
}

TEST_CASE("Hiding the composer withdraws the undo offer", "[composer][undosend]") {
    ComposerWidget c;
    UndoHost       host;
    host.wire(&c);
    showWithText(&c, "bye");
    sendKey(&c, Qt::Key_Return);
    REQUIRE(c.undoSendOffered());
    c.hide();
    CHECK_FALSE(c.undoSendOffered());
}

TEST_CASE("offerUndoSend by ghost ts needs a session that can delete", "[composer][undosend]") {
    ComposerWidget c;
    c.offerUndoSend(ConversationId{"C1"}, Ts{"1.000"});
    CHECK_FALSE(c.undoSendOffered());

    // A session whose backend lacks deleteMessage (StubBackend2 reports no
    // capabilities) gets no offer either — the chip would be a dead control.
    auto *stub   = new StubBackend2;
    stub->_meId  = UserId{"U1"};
    stub->_convs = std::vector<Conversation>{};
    Session session(std::unique_ptr<Backend>(stub), "T_TEST");
    c.setSession(&session);
    c.offerUndoSend(ConversationId{"C1"}, Ts{"1.000"});
    CHECK_FALSE(c.undoSendOffered());
}
