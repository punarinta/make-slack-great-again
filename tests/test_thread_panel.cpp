// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Regression tests for what ThreadPanel wires up between its embedded message
// list, its composer, and the session.
//
// The bug these exist for: attaching an image to a thread reply silently did
// nothing — no message, no error, and the composer emptied so it looked sent.
// ComposerWidget emits uploadRequested (not sendRequested) as soon as files are
// attached, and ThreadPanel had only ever connected sendRequested, so the emit
// hit nothing at all. Nothing downstream could notice: Session was never
// called, so no ghost, no request, no error banner.
//
// A dead connection is invisible to tests of the code on either side of it —
// Session::uploadFiles and the Slack wire body can both be perfect while the
// signal goes nowhere. So these assert the wiring itself: emit the composer's
// signals and check what reaches the backend, including the thread root, which
// is the part that decides whether the file lands in the thread or at the
// channel root.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextEdit>

#include "backend/backend.h"
#include "backend/domain.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"
#include "session/session.h"
#include "ui/composer/composer_widget.h"
#include "ui/message_list/message_list.h"
#include "ui/thread_panel/thread_panel.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-thread-panel");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── StubBackend ───────────────────────────────────────────────────────────────
// Records the calls the panel is supposed to produce; everything else is inert.

struct StubBackend : Backend {
    rpl::variable<AuthState>                 _authState{AuthState::LoggedIn};
    rpl::variable<UserId>                    _meId;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;

    struct UploadCall {
        ConversationId    conv;
        QStringList       paths;
        QString           comment;
        std::optional<Ts> threadRoot;
    };
    std::vector<UploadCall> uploadCalls;

    struct SendCall {
        ConversationId  conv;
        OutgoingMessage msg;
    };
    std::vector<SendCall> sendCalls;

    rpl::producer<AuthState> authState() const override { return _authState.value(); }
    Capabilities             capabilities() const override { return {}; }
    void                     connectRealtime() override {}
    void                     disconnectRealtime() override {}

    rpl::producer<UserId>                    loadMe() override { return _meId.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers() override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }

    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>(MessagePage{}).value();
    }
    // The thread as the server currently has it. Tests mutate this between
    // fetches to model a reply arriving, and count the fetches to prove the
    // refresh is conditional rather than unconditional polling.
    MessagePage _threadPage;
    int         threadLoads = 0;

    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        ++threadLoads;
        return rpl::variable<MessagePage>(_threadPage).value();
    }

    void sendMessage(ConversationId c, OutgoingMessage m) override {
        sendCalls.push_back({c, std::move(m)});
    }
    struct EditCall {
        ConversationId   conv;
        Ts               ts;
        TextWithEntities text;
    };
    std::vector<EditCall> editCalls;

    void editMessage(ConversationId c, Ts ts, TextWithEntities t) override {
        editCalls.push_back({c, ts, std::move(t)});
    }
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override {}

    void uploadFiles(
        ConversationId                     c,
        const QStringList                 &paths,
        const QString                     &comment,
        std::optional<Ts>                  threadRoot = std::nullopt,
        std::function<void(bool, QString)> done       = {}
    ) override {
        uploadCalls.push_back({c, paths, comment, threadRoot});
        // Leave `done` uncalled: an upload in flight is the state the ghost is
        // meant to survive in, and neither outcome is what these tests assert.
        Q_UNUSED(done);
    }
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)> = {}
    ) override {}

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }

    rpl::producer<Event> events() const override { return _events.events(); }
};

// ── Fixture ───────────────────────────────────────────────────────────────────

static const Conversation kConv = {
    .id       = ConversationId{"C1"},
    .kind     = ConvKind::PublicChannel,
    .name     = "general",
    .isMember = true,
    .lastRead = "0",
};

static const Ts kRoot = QStringLiteral("100.500");

struct Fixture {
    QTemporaryDir            tempDir;
    StubBackend             *stub = nullptr;
    std::unique_ptr<Session> session;
    QString                  filePath;

    Fixture() {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());
        auto backend = std::make_unique<StubBackend>();
        stub         = backend.get();
        stub->_meId  = UserId{"U1"};
        stub->_convs = std::vector<Conversation>{kConv};
        stub->_users = std::vector<User>{
            {.id = UserId{"U1"}, .name = "me", .displayName = "Me", .isActive = true}
        };
        session = std::make_unique<Session>(std::move(backend), "T_THREAD_PANEL_TEST");
        session->start();

        // Session::uploadFiles stats the file for the optimistic attachment, so
        // it has to actually exist.
        filePath = tempDir.path() + "/pic.png";
        QFile f(filePath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("img");
    }
    ~Fixture() {
        session.reset();
        QDir(tempDir.path()).removeRecursively();
    }
};

// The composer is ThreadPanel's own child; the panel owns the wiring under test.
static ComposerWidget *composerOf(ThreadPanel &panel) {
    auto *c = panel.findChild<ComposerWidget *>();
    REQUIRE(c != nullptr);
    return c;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("attaching a file in a thread reaches the backend with the thread root", "[thread]") {
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    emit composerOf(panel)->uploadRequested({f.filePath}, QStringLiteral("look at this"));

    // The dead connection made this zero: nothing was ever asked to upload.
    REQUIRE(f.stub->uploadCalls.size() == 1);
    const auto &call = f.stub->uploadCalls[0];
    CHECK(call.conv == kConv.id);
    CHECK(call.paths == QStringList{f.filePath});
    CHECK(call.comment == QStringLiteral("look at this"));
    // Without the root the file would post at the channel root instead — the
    // reply would land, just not in the thread the user was typing in.
    REQUIRE(call.threadRoot.has_value());
    CHECK(*call.threadRoot == kRoot);
}

TEST_CASE("a caption-less attachment still uploads into the thread", "[thread]") {
    // Attaching with an empty composer is the ordinary case — drag an image in
    // and hit enter — and it must not be mistaken for "nothing to send".
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    emit composerOf(panel)->uploadRequested({f.filePath}, QString());

    REQUIRE(f.stub->uploadCalls.size() == 1);
    CHECK(f.stub->uploadCalls[0].comment.isEmpty());
    REQUIRE(f.stub->uploadCalls[0].threadRoot.has_value());
    CHECK(*f.stub->uploadCalls[0].threadRoot == kRoot);
}

TEST_CASE("a plain text reply still carries the thread root", "[thread]") {
    // The control: sendRequested was always wired, and wiring uploadRequested
    // beside it must not have disturbed it.
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    emit composerOf(panel)->sendRequested(QStringLiteral("just text"));

    REQUIRE(f.stub->sendCalls.size() == 1);
    CHECK(f.stub->sendCalls[0].conv == kConv.id);
    REQUIRE(f.stub->sendCalls[0].msg.threadRoot.has_value());
    CHECK(*f.stub->sendCalls[0].msg.threadRoot == kRoot);
    CHECK(f.stub->uploadCalls.empty());
}

// ── Live refresh of an open thread ────────────────────────────────────────────
//
// The bug: an open thread panel never showed replies that arrived after it was
// opened. Thread replies are absent from conversations.history, so the safety
// poll — the only delivery mechanism on a session-auth workspace, and the
// recovery path when the shared socket's round-robin steal drops an event —
// carried nothing that could reach a thread view, and its EvHeadRefresh was
// discarded outright in thread mode. The channel's reply bar meanwhile counted
// up from the root's reply_count in that same page, so the UI stated a reply
// existed and then refused to show it.

static Message replyMsg(const Ts &ts, const QString &text, const QString &author) {
    return Message{
        .ts         = ts,
        .date       = decimalTsToMicros(ts),
        .threadRoot = kRoot,
        .author     = UserId{author},
        .text       = TextWithEntities{text, {}},
    };
}

// The head page the safety poll hands over: just the thread root, carrying the
// reply bookkeeping that tells an open panel whether the thread has moved.
static Message rootInHeadPage(const Ts &latestReply, int replyCount) {
    Message m{
        .ts     = kRoot,
        .date   = decimalTsToMicros(kRoot),
        .author = UserId{"U2"},
        .text   = TextWithEntities{"root", {}},
    };
    m.replyCount  = replyCount;
    m.latestReply = latestReply;
    return m;
}

static MessageListWidget *listOf(ThreadPanel &panel) {
    auto *l = panel.findChild<MessageListWidget *>();
    REQUIRE(l != nullptr);
    return l;
}

TEST_CASE("a reply arriving while the thread is open appears in the panel", "[thread][refresh]") {
    Fixture f;
    // Thread as opened: root + one reply from someone else.
    f.stub->_threadPage =
        MessagePage{{rootInHeadPage("100.600", 1), replyMsg("100.600", "first", "U2")}, {}};

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    REQUIRE(f.stub->threadLoads == 1);
    // Nothing of ours in the thread yet — the anchor for the assertion below.
    CHECK_FALSE(listOf(panel)->lastOwnMessage(UserId{"U1"}).has_value());

    // A second reply lands on the server. Only conversations.replies can see it.
    f.stub->_threadPage = MessagePage{
        {rootInHeadPage("100.700", 2),
         replyMsg("100.600", "first", "U2"),
         replyMsg("100.700", "second", "U1")},
        {}
    };

    // The safety poll's head page: the root now advertises the newer reply.
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {rootInHeadPage("100.700", 2)}});

    // Before the fix this was still 1: the head page was dropped on the floor.
    CHECK(f.stub->threadLoads == 2);
    const auto latest = listOf(panel)->lastOwnMessage(UserId{"U1"});
    REQUIRE(latest.has_value());
    CHECK(latest->ts == QStringLiteral("100.700"));
}

TEST_CASE("an unchanged thread costs no extra fetch", "[thread][refresh]") {
    // latest_reply on the root is the "has this thread moved?" flag. A panel
    // left open on a quiet thread must not burn a conversations.replies call on
    // every poll tick just to learn nothing changed.
    Fixture f;
    f.stub->_threadPage =
        MessagePage{{rootInHeadPage("100.600", 1), replyMsg("100.600", "first", "U2")}, {}};

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    REQUIRE(f.stub->threadLoads == 1);

    f.stub->_events.fire(EvHeadRefresh{kConv.id, {rootInHeadPage("100.600", 1)}});

    CHECK(f.stub->threadLoads == 1);
}

TEST_CASE("a thread whose root is off the head page is refreshed anyway", "[thread][refresh]") {
    // An old root with an active thread isn't in the channel's head page at all,
    // so there is no latest_reply to compare against — and "can't prove it's
    // unchanged" must mean fetch, not skip, or exactly the long-running threads
    // people actually watch would be the ones that never update.
    Fixture f;
    f.stub->_threadPage =
        MessagePage{{rootInHeadPage("100.600", 1), replyMsg("100.600", "first", "U2")}, {}};

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    REQUIRE(f.stub->threadLoads == 1);

    // Head page holds only unrelated, newer channel traffic.
    Message other{
        .ts     = "900.000",
        .date   = decimalTsToMicros("900.000"),
        .author = UserId{"U2"},
        .text   = TextWithEntities{"unrelated", {}},
    };
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {other}});

    CHECK(f.stub->threadLoads == 2);
}

TEST_CASE("a head refresh for another conversation leaves the thread alone", "[thread][refresh]") {
    // Background polls target other conversations; a panel showing C1's thread
    // must not re-fetch because C2 moved.
    Fixture f;
    f.stub->_threadPage =
        MessagePage{{rootInHeadPage("100.600", 1), replyMsg("100.600", "first", "U2")}, {}};

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    REQUIRE(f.stub->threadLoads == 1);

    f.stub->_events.fire(EvHeadRefresh{ConversationId{"C2"}, {rootInHeadPage("100.700", 2)}});

    CHECK(f.stub->threadLoads == 1);
}

TEST_CASE("a thread refresh does not overwrite the channel's cached history", "[thread][refresh]") {
    // cacheMessages REPLACES a conversation's cached page. Writing the thread's
    // replies under the channel key would make the channel reopen showing
    // nothing but replies — so the merge must skip the cache in thread mode.
    Fixture                    f;
    const std::vector<Message> channelHistory = {
        Message{
            .ts     = "50.000",
            .date   = decimalTsToMicros("50.000"),
            .author = UserId{"U2"},
            .text   = TextWithEntities{"channel message", {}},
        },
    };
    f.session->cacheMessages(kConv.id, channelHistory);
    f.stub->_threadPage =
        MessagePage{{rootInHeadPage("100.600", 1), replyMsg("100.600", "first", "U2")}, {}};

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    f.stub->_events.fire(EvHeadRefresh{kConv.id, {rootInHeadPage("100.700", 2)}});

    const auto cached = f.session->cachedMessages(kConv.id);
    REQUIRE(cached.size() == 1);
    CHECK(cached[0].ts == QStringLiteral("50.000"));
}

TEST_CASE("an upload with no thread open is dropped, not sent to the channel", "[thread]") {
    // The guard in the slot: with no root there is no thread to reply in, and
    // silently redirecting the file to the channel root would be worse than
    // doing nothing.
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    // No openThread() — _rootTs is empty.

    emit composerOf(panel)->uploadRequested({f.filePath}, QStringLiteral("stray"));

    CHECK(f.stub->uploadCalls.empty());
}

// ── Editing a reply from the thread panel ─────────────────────────────────────
//
// The bug (issue #44): in a thread, "Edit message" from the context menu did
// nothing at all, and only the newest own reply could be edited — by pressing ↑
// in an empty composer. Two separate wires reach the composer's edit mode:
// editLastRequested (the ↑ key) and the list's editMessageRequested (the menu
// item, and the only way to reach anything but the last message). ThreadPanel
// connected the first and not the second, so the menu item emitted into
// nothing: no edit mode, no request, no error.

TEST_CASE("Edit message on a thread reply loads it into the thread composer", "[thread][edit]") {
    Fixture f;
    f.stub->_threadPage = MessagePage{
        {rootInHeadPage("100.700", 2),
         replyMsg("100.600", "older reply", "U1"),
         replyMsg("100.700", "newest reply", "U1")},
        {}
    };

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    // What the context menu emits for an older reply — not the last one, which
    // is the only message the ↑ path can ever reach.
    emit listOf(panel)->editMessageRequested(QStringLiteral("100.600"), "older reply", {});

    // Before the fix the composer stayed empty: the signal had no receiver.
    CHECK(composerOf(panel)->currentText() == QStringLiteral("older reply"));
}

TEST_CASE("editing an older thread reply reaches the backend", "[thread][edit]") {
    // The whole chain, end to end: menu → composer edit mode → Enter → API,
    // carrying the ts of the message the user actually picked. Enter only emits
    // editRequested while the composer is in edit mode, so a dead menu wire
    // makes this post a brand-new reply instead of updating anything.
    Fixture f;
    f.stub->_threadPage = MessagePage{
        {rootInHeadPage("100.700", 2),
         replyMsg("100.600", "older reply", "U1"),
         replyMsg("100.700", "newest reply", "U1")},
        {}
    };

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    emit listOf(panel)->editMessageRequested(QStringLiteral("100.600"), "older reply", {});

    auto *ed = composerOf(panel)->findChild<QTextEdit *>("composerEdit");
    REQUIRE(ed != nullptr);
    ed->setPlainText(QStringLiteral("fixed it"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(ed, &enter);

    REQUIRE(f.stub->editCalls.size() == 1);
    CHECK(f.stub->editCalls[0].conv == kConv.id);
    CHECK(f.stub->editCalls[0].ts == QStringLiteral("100.600"));
    CHECK(f.stub->editCalls[0].text.text == QStringLiteral("fixed it"));
    // Not a new reply: an unwired menu item leaves the composer in plain send
    // mode, where the same Enter posts "fixed it" as a fresh message.
    CHECK(f.stub->sendCalls.empty());
}

TEST_CASE("the ↑ path still edits the newest own reply", "[thread][edit]") {
    // The control: the one edit route that did work must keep working.
    Fixture f;
    f.stub->_threadPage = MessagePage{
        {rootInHeadPage("100.700", 2),
         replyMsg("100.600", "older reply", "U1"),
         replyMsg("100.700", "newest reply", "U1")},
        {}
    };

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    emit composerOf(panel)->editLastRequested();

    CHECK(composerOf(panel)->currentText() == QStringLiteral("newest reply"));
}

// ── Draft stashing across threads ─────────────────────────────────────────────
//
// Security-boundary tests: input staged in one thread (text + attachments) must
// never send into another thread, another conversation, or another workspace —
// and must come back intact when the user returns to where it was typed. The
// failure mode these guard against is the worst kind: a reply drafted for one
// audience silently posted to a different one.

static void typeInto(ComposerWidget *c, const QString &text) {
    auto *ed = c->findChild<QTextEdit *>("composerEdit");
    REQUIRE(ed != nullptr);
    ed->setPlainText(text);
}

static void pressEnter(ComposerWidget *c) {
    auto *ed = c->findChild<QTextEdit *>("composerEdit");
    REQUIRE(ed != nullptr);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(ed, &enter);
}

static const Ts kRoot2 = QStringLiteral("200.500");

TEST_CASE("input staged in one thread never sends into another", "[thread][draft]") {
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    auto *c = composerOf(panel);
    typeInto(c, "secret for thread A");
    c->addPendingFile(f.filePath);

    // Another thread — and also another conversation entirely: both must find
    // the composer empty.
    panel.openThread(ConversationId{"C2"}, kRoot2);
    CHECK(c->currentText().isEmpty());
    CHECK(c->pendingFiles().isEmpty());

    // Enter with leftover state would have posted A's text and file here.
    pressEnter(c);
    CHECK(f.stub->sendCalls.empty());
    CHECK(f.stub->uploadCalls.empty());

    // A genuine reply in the other thread carries neither A's text nor A's
    // file: had the pending file leaked, this send would be an upload instead.
    panel.openThread(kConv.id, kRoot2);
    typeInto(c, "hello B");
    pressEnter(c);
    REQUIRE(f.stub->sendCalls.size() == 1);
    CHECK(f.stub->sendCalls[0].conv == kConv.id);
    CHECK(f.stub->sendCalls[0].msg.text.text == QStringLiteral("hello B"));
    REQUIRE(f.stub->sendCalls[0].msg.threadRoot.has_value());
    CHECK(*f.stub->sendCalls[0].msg.threadRoot == kRoot2);
    CHECK(f.stub->uploadCalls.empty());
}

TEST_CASE("returning to a thread restores its stashed input", "[thread][draft]") {
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    auto *c = composerOf(panel);
    typeInto(c, "wip reply");
    c->addPendingFile(f.filePath);

    panel.openThread(kConv.id, kRoot2); // away…
    panel.openThread(kConv.id, kRoot);  // …and back

    CHECK(c->currentText() == QStringLiteral("wip reply"));
    CHECK(c->pendingFiles() == QStringList{f.filePath});

    // The restored draft sends into ITS thread, file and text as one message.
    pressEnter(c);
    REQUIRE(f.stub->uploadCalls.size() == 1);
    CHECK(f.stub->uploadCalls[0].conv == kConv.id);
    CHECK(f.stub->uploadCalls[0].paths == QStringList{f.filePath});
    CHECK(f.stub->uploadCalls[0].comment == QStringLiteral("wip reply"));
    REQUIRE(f.stub->uploadCalls[0].threadRoot.has_value());
    CHECK(*f.stub->uploadCalls[0].threadRoot == kRoot);
    CHECK(f.stub->sendCalls.empty());
}

TEST_CASE("two threads in the same conversation stash separately", "[thread][draft]") {
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());

    panel.openThread(kConv.id, kRoot);
    typeInto(composerOf(panel), "for root one");

    panel.openThread(kConv.id, kRoot2);
    CHECK(composerOf(panel)->currentText().isEmpty());
    typeInto(composerOf(panel), "for root two");

    panel.openThread(kConv.id, kRoot);
    CHECK(composerOf(panel)->currentText() == QStringLiteral("for root one"));
    panel.openThread(kConv.id, kRoot2);
    CHECK(composerOf(panel)->currentText() == QStringLiteral("for root two"));
}

TEST_CASE("closing the panel stashes the reply draft", "[thread][draft]") {
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    typeInto(composerOf(panel), "typed then closed");
    composerOf(panel)->addPendingFile(f.filePath);

    panel.close();
    CHECK(composerOf(panel)->currentText().isEmpty());
    CHECK(composerOf(panel)->pendingFiles().isEmpty());

    panel.openThread(kConv.id, kRoot);
    CHECK(composerOf(panel)->currentText() == QStringLiteral("typed then closed"));
    CHECK(composerOf(panel)->pendingFiles() == QStringList{f.filePath});
}

TEST_CASE("identical thread ids in another workspace see none of the input", "[thread][draft]") {
    // Ids are only unique within a workspace; the stash key must be
    // workspace-qualified or a reply drafted in one workspace could surface —
    // and send — in another that happens to reuse the same conv id and root ts.
    Fixture f;
    auto    backend2 = std::make_unique<StubBackend>();
    auto   *stub2    = backend2.get();
    stub2->_meId     = UserId{"U9"};
    stub2->_convs    = std::vector<Conversation>{kConv}; // same "C1"
    Session session2(std::move(backend2), "T_THREAD_PANEL_TEST_B");
    session2.start();

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    typeInto(composerOf(panel), "workspace A secret");
    composerOf(panel)->addPendingFile(f.filePath);

    // Same conversation id, same root ts — different workspace.
    panel.setSession(&session2);
    panel.openThread(kConv.id, kRoot);
    CHECK(composerOf(panel)->currentText().isEmpty());
    CHECK(composerOf(panel)->pendingFiles().isEmpty());

    pressEnter(composerOf(panel));
    CHECK(stub2->sendCalls.empty());
    CHECK(stub2->uploadCalls.empty());

    // Back in workspace A the draft is exactly where it was left.
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    CHECK(composerOf(panel)->currentText() == QStringLiteral("workspace A secret"));
    CHECK(composerOf(panel)->pendingFiles() == QStringList{f.filePath});
}

TEST_CASE("purgeDrafts forgets a logged-out workspace's reply drafts", "[thread][draft]") {
    Fixture     f;
    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);
    typeInto(composerOf(panel), "must not survive logout");

    panel.close(); // stashes
    panel.purgeDrafts(f.session->teamId());

    panel.openThread(kConv.id, kRoot);
    CHECK(composerOf(panel)->currentText().isEmpty());
}

TEST_CASE("Forward message on a thread reply reaches the host", "[thread][forward]") {
    // Same dead-wire class: the list's forward signal (context menu, hover
    // toolbar, and the image viewer) had no receiver in the panel either, so
    // forwarding from a thread silently did nothing.
    Fixture f;
    f.stub->_threadPage =
        MessagePage{{rootInHeadPage("100.600", 1), replyMsg("100.600", "older reply", "U1")}, {}};

    ThreadPanel panel(/*imgCache*/ nullptr);
    panel.setSession(f.session.get());
    panel.openThread(kConv.id, kRoot);

    std::vector<std::pair<ConversationId, Message>> forwarded;
    QObject::connect(
        &panel,
        &ThreadPanel::forwardMessageRequested,
        &panel,
        [&](const ConversationId &c, const Message &m) { forwarded.emplace_back(c, m); }
    );

    emit listOf(panel)->forwardMessageRequested(replyMsg("100.600", "older reply", "U1"));

    REQUIRE(forwarded.size() == 1);
    // The host needs the source conversation: the label path (email) reads the
    // message from it, and the panel is not always on the conversation shown.
    CHECK(forwarded[0].first == kConv.id);
    CHECK(forwarded[0].second.ts == QStringLiteral("100.600"));
}
