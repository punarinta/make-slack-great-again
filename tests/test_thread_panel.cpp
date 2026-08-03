// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Regression tests for what ThreadPanel wires up between its composer and the
// session.
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
#include <QSettings>
#include <QTemporaryDir>

#include "backend/backend.h"
#include "backend/domain.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"
#include "session/session.h"
#include "ui/composer/composer_widget.h"
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
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }

    void sendMessage(ConversationId c, OutgoingMessage m) override {
        sendCalls.push_back({c, std::move(m)});
    }
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
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
