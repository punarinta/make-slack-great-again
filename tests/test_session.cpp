// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "session/session.h"
#include "backend/backend.h"
#include "backend/common_commands.h"
#include "cache/workspace_cache.h"
#include "rpl/producer.h"
#include "rpl/variable.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── StubBackend ───────────────────────────────────────────────────────────────
// Minimal controllable Backend for session tests.
// All producers use rpl::variable so they fire synchronously on subscription.

struct StubBackend : Backend {
    rpl::variable<AuthState>                 _authState{AuthState::LoggedIn};
    rpl::variable<UserId>                    _meId;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;

    bool presenceResult = false;
    struct SentMessage {
        ConversationId  conv;
        OutgoingMessage msg;
    };
    std::vector<SentMessage> sentMessages;

    User           botInfoResult; // returned by loadBotInfo; empty id = not found
    int            botInfoCallCount = 0;
    QList<QString> botInfoRequested;

    rpl::producer<AuthState>  authState() const override { return _authState.value(); }
    Capabilities              capabilities() const override { return {}; }
    // Emulates a Slack-like backend's native command set (Slack conventions —
    // /shrug, /mute, /away, … — all live in the backend, not app-level).
    std::vector<SlashCommand> nativeCommands() const override {
        return CommonCommands::select(
            {"shrug", "mute", "active", "away", "dnd", "status", "msg", "dm", "leave"}
        );
    }
    void connectRealtime() override {}
    void disconnectRealtime() override {}
    // false models a poll-only backend (Slack session/cookie auth): no push, so the
    // Session must drive discovery itself.
    bool pushEnabled = true;
    bool hasRealtimePush() const override { return pushEnabled; }

    // Whole-workspace activity snapshot (Slack's client.counts). nullopt models a
    // backend that cannot serve one — the producer completes WITHOUT a value, which
    // is the graceful-degrade signal the Session counts failures on.
    std::optional<std::vector<ConvCounts>> countsResult;
    int                                    countsCalls = 0;
    rpl::producer<std::vector<ConvCounts>> loadUnreadCounts() override {
        ++countsCalls;
        if (!countsResult)
            return [](auto consumer) {
                consumer.put_done();
                return rpl::lifetime();
            };
        return [v = *countsResult](auto consumer) mutable {
            consumer.put_next(std::move(v));
            consumer.put_done();
            return rpl::lifetime();
        };
    }
    int  verifyRealtimeCalls      = 0;
    int  reestablishRealtimeCalls = 0;
    void verifyRealtime() override { ++verifyRealtimeCalls; }
    void reestablishRealtime() override { ++reestablishRealtimeCalls; }

    // Mirror the Slack id-shape rules the Session used to hardcode, so the
    // fetch-routing / synthetic-account tests keep exercising the same paths.
    bool isSyntheticUser(UserId id) const override {
        return id.value == QLatin1String("USLACKBOT") || id.value == QLatin1String("USLACK");
    }
    bool isBotId(UserId id) const override { return id.value.startsWith('B'); }
    bool isUserId(UserId id) const override {
        return id.value.startsWith('U') || id.value.startsWith('W');
    }

    bool loadMeAlwaysFails    = false; // every call completes empty (auth.test error)
    bool loadMeFirstCallFails = false; // first call completes empty, later ones succeed
    int  loadMeCalls          = 0;
    rpl::producer<UserId> loadMe() override {
        ++loadMeCalls;
        if (loadMeAlwaysFails || (loadMeFirstCallFails && loadMeCalls == 1))
            return [](auto consumer) {
                consumer.put_done();
                return rpl::lifetime();
            };
        return _meId.value();
    }

    int                                      loadConversationsCalls = 0;
    rpl::producer<std::vector<Conversation>> loadConversations() override {
        ++loadConversationsCalls;
        return _convs.value();
    }
    rpl::producer<std::vector<User>> loadUsers() override { return _users.value(); }

    int                 loadPresenceCalls = 0; // times loadPresence was actually invoked
    UserId              lastPresenceUser;      // user id of the most recent loadPresence call
    rpl::producer<bool> loadPresence(UserId id) override {
        ++loadPresenceCalls;
        lastPresenceUser = id;
        return rpl::variable<bool>(presenceResult).value();
    }
    SelfPresence                selfPresenceResult;                 // returned by loadSelfPresence
    bool                        selfPresenceFirstCallFails = false; // first call completes empty
    int                         selfPresenceCalls          = 0;
    rpl::producer<SelfPresence> loadSelfPresence() override {
        ++selfPresenceCalls;
        if (selfPresenceFirstCallFails && selfPresenceCalls == 1)
            return [](auto consumer) {
                consumer.put_done();
                return rpl::lifetime();
            };
        return rpl::variable<SelfPresence>(selfPresenceResult).value();
    }
    // Controllable head page for the realtime safety-net poll. Mutate between
    // ticks to model a message deleted from another client.
    std::vector<Message>       historyPage;
    int                        loadHistoryCalls = 0;
    ConversationId             lastHistoryConv;
    QList<QString>             historyConvIds; // every conversation fetched, in order
    rpl::producer<MessagePage> loadHistory(ConversationId c, std::optional<QString>) override {
        ++loadHistoryCalls;
        lastHistoryConv = c;
        historyConvIds.append(c.value);
        return rpl::variable<MessagePage>(MessagePage{historyPage, std::nullopt}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }

    void sendMessage(ConversationId c, OutgoingMessage m) override {
        sentMessages.push_back({c, std::move(m)});
    }
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}

    struct MarkReadCall {
        ConversationId conv;
        Ts             ts;
    };
    std::vector<MarkReadCall> markReadCalls;
    void markRead(ConversationId c, Ts t) override { markReadCalls.push_back({c, std::move(t)}); }

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }
    struct UploadCall {
        ConversationId                     conv;
        QStringList                        paths;
        QString                            comment;
        std::optional<Ts>                  threadRoot;
        std::function<void(bool, QString)> done;
    };
    std::vector<UploadCall> uploadCalls;
    void                    uploadFiles(
                           ConversationId                     c,
                           const QStringList                 &paths,
                           const QString                     &comment,
                           std::optional<Ts> threadRoot = std::nullopt,
                           std::function<void(bool, QString)> done = {}
                       ) override {
        uploadCalls.push_back({c, paths, comment, threadRoot, std::move(done)});
    }

    // Canvas fixtures: conv id → file id, file id → HTML.
    std::unordered_map<QString, QString> convCanvas;
    std::unordered_map<QString, QString> canvasHtml;
    struct EditCanvasCall {
        QString                   canvasId;
        std::vector<CanvasChange> changes;
    };
    std::vector<EditCanvasCall> editCanvasCalls;
    bool                        editCanvasOk = true;

    void loadChannelCanvas(ConversationId c, std::function<void(QString, bool)> done) override {
        const auto it = convCanvas.find(c.value);
        if (done)
            done(it == convCanvas.end() ? QString() : it->second, false);
    }
    void loadCanvasContent(
        const QString                    &fileId,
        std::function<void(QString html)> onHtml,
        std::function<void(QString)>      onError = {}
    ) override {
        const auto it = canvasHtml.find(fileId);
        if (it == canvasHtml.end()) {
            if (onError)
                onError(QStringLiteral("canvas_not_found"));
        } else if (onHtml) {
            onHtml(it->second);
        }
    }
    void createChannelCanvas(
        ConversationId                      c,
        const QString                      &markdown,
        std::function<void(QString fileId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override {
        if (convCanvas.count(c.value)) {
            if (onError)
                onError(QStringLiteral("channel_canvas_already_exists"));
            return;
        }
        const QString id    = QStringLiteral("FCV-%1").arg(c.value);
        convCanvas[c.value] = id;
        canvasHtml[id]      = QStringLiteral("<p>%1</p>").arg(markdown);
        if (onSuccess)
            onSuccess(id);
    }
    void editCanvas(
        const QString                            &canvasId,
        const std::vector<CanvasChange>          &changes,
        std::function<void(bool ok, QString err)> done = {}
    ) override {
        editCanvasCalls.push_back({canvasId, changes});
        if (done)
            done(editCanvasOk, editCanvasOk ? QString() : QStringLiteral("canvas_editing_failed"));
    }
    std::unordered_map<QString, QString> canvasTitle;
    std::vector<QString>                 deleteCanvasCalls;
    bool                                 deleteCanvasOk = true;
    void                                 loadCanvasMeta(
                                        const QString &fileId, std::function<void(QString, QString, CanvasMetaState)> done
                                    ) override {
        if (!done)
            return;
        if (!canvasHtml.count(fileId)) {
            done({}, {}, CanvasMetaState::Gone); // file_deleted
            return;
        }
        const auto it = canvasTitle.find(fileId);
        done(
            it == canvasTitle.end() ? QString() : it->second,
            QStringLiteral("https://stub.slack.com/docs/T0/%1").arg(fileId),
            CanvasMetaState::Ok
        );
    }
    void
    deleteCanvas(const QString &canvasId, std::function<void(bool, QString)> done = {}) override {
        deleteCanvasCalls.push_back(canvasId);
        if (done)
            done(deleteCanvasOk, deleteCanvasOk ? QString() : QStringLiteral("canvas_not_found"));
    }
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)>
    ) override {}

    rpl::producer<Event> events() const override { return _events.events(); }

    rpl::producer<User> loadBotInfo(UserId botId) override {
        botInfoCallCount++;
        botInfoRequested.append(botId.value);
        User result = botInfoResult;
        return [result](auto consumer) mutable {
            if (!result.id.value.isEmpty())
                consumer.put_next(std::move(result));
            consumer.put_done();
            return rpl::lifetime();
        };
    }

    struct StarCall {
        ConversationId id;
        bool           star;
    };
    std::vector<StarCall>       starCalls;
    std::vector<ConversationId> leaveCalls;

    void starConversation(ConversationId id, bool star) override {
        starCalls.push_back({id, star});
    }
    void leaveConversation(ConversationId id) override { leaveCalls.push_back(id); }

    struct CreateChannelCall {
        QString name;
        bool    isPrivate;
    };
    std::vector<CreateChannelCall> createChannelCalls;
    bool                           createChannelShouldFail = false;
    ConversationId                 createChannelResultId;
    QString                        createChannelError;

    void createChannel(
        const QString                      &name,
        bool                                isPrivate,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override {
        createChannelCalls.push_back({name, isPrivate});
        if (createChannelShouldFail) {
            if (onError)
                onError(createChannelError);
        } else {
            if (onSuccess)
                onSuccess(createChannelResultId);
        }
    }

    struct JoinChannelCall {
        ConversationId id;
    };
    std::vector<JoinChannelCall> joinChannelCalls;
    bool                         joinChannelShouldFail = false;
    ConversationId               joinChannelResultId;
    QString                      joinChannelError;

    void joinChannel(
        ConversationId                      id,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override {
        joinChannelCalls.push_back({id});
        if (joinChannelShouldFail) {
            if (onError)
                onError(joinChannelError);
        } else {
            if (onSuccess)
                onSuccess(joinChannelResultId);
        }
    }

    std::vector<UserId> openDmCalls;
    bool                openDmShouldFail = false;
    ConversationId      openDmResultId;
    QString             openDmError;
    void                openDm(
                       UserId                              user,
                       std::function<void(ConversationId)> onSuccess = {},
                       std::function<void(QString)>        onError   = {}
                   ) override {
        openDmCalls.push_back(user);
        if (openDmShouldFail) {
            if (onError)
                onError(openDmError);
        } else {
            if (onSuccess)
                onSuccess(openDmResultId);
        }
    }

    // conversations.info results for the DM activity sweep; missing id = error
    // (producer completes empty, like the real backend on failure).
    QHash<QString, Conversation> infoResults;
    QList<QString>               infoRequested;
    rpl::producer<Conversation>
    loadConversationInfo(ConversationId id, bool /*background*/) override {
        infoRequested.append(id.value);
        const auto it = infoResults.constFind(id.value);
        if (it == infoResults.constEnd())
            return [](auto consumer) {
                consumer.put_done();
                return rpl::lifetime();
            };
        Conversation result = *it;
        return [result](auto consumer) mutable {
            consumer.put_next(std::move(result));
            consumer.put_done();
            return rpl::lifetime();
        };
    }

    // users.info fixtures for the missing-DM-peer resolver; missing id = error
    // (producer completes empty, like the real backend on failure).
    QHash<QString, User> userInfoResults;
    QList<QString>       userInfoRequested;
    rpl::producer<User>  loadUser(UserId id) override {
        userInfoRequested.append(id.value);
        const auto it = userInfoResults.constFind(id.value);
        if (it == userInfoResults.constEnd())
            return [](auto consumer) {
                consumer.put_done();
                return rpl::lifetime();
            };
        User result = *it;
        return [result](auto consumer) mutable {
            consumer.put_next(std::move(result));
            consumer.put_done();
            return rpl::lifetime();
        };
    }

    // Self presence / status — recorded calls; configurable failure.
    QString           selfActionError; // non-empty = all three calls fail with this
    std::vector<bool> presenceCalls;   // away flag per setPresence call
    struct StatusCall {
        QString emoji;
        QString text;
        qint64  expirationTs;
    };
    std::vector<StatusCall> statusCalls;
    std::vector<int>        dndCalls; // minutes per setDndSnooze call

    void setPresence(bool away, std::function<void(bool, QString)> done = {}) override {
        presenceCalls.push_back(away);
        if (done)
            done(selfActionError.isEmpty(), selfActionError);
    }
    void setStatus(
        const QString                     &emoji,
        const QString                     &text,
        qint64                             expirationTs = 0,
        std::function<void(bool, QString)> done         = {}
    ) override {
        statusCalls.push_back({emoji, text, expirationTs});
        if (done)
            done(selfActionError.isEmpty(), selfActionError);
    }
    void setDndSnooze(int minutes, std::function<void(bool, QString)> done = {}) override {
        dndCalls.push_back(minutes);
        if (done)
            done(selfActionError.isEmpty(), selfActionError);
    }

    // commands.list result; empty = unsupported (producer completes empty).
    std::vector<SlashCommand>                commandsResult;
    rpl::producer<std::vector<SlashCommand>> listCommands() override {
        if (commandsResult.empty())
            return [](auto consumer) {
                consumer.put_done();
                return rpl::lifetime();
            };
        return rpl::variable<std::vector<SlashCommand>>(commandsResult).value();
    }

    struct RunCommandCall {
        ConversationId conv;
        QString        command;
        QString        text;
    };
    std::vector<RunCommandCall> runCommandCalls;
    bool                        runCommandShouldFail = false;
    void                        runCommand(
                               ConversationId                     c,
                               const QString                     &command,
                               const QString                     &text,
                               std::function<void(bool, QString)> done = {}
                           ) override {
        runCommandCalls.push_back({c, command, text});
        if (done)
            done(!runCommandShouldFail, runCommandShouldFail ? "missing_scope" : QString());
    }

    void fireEvent(Event e) { _events.fire(std::move(e)); }
};

// ── SessionFixture ────────────────────────────────────────────────────────────

static const User kAlice = User{
    .id            = UserId{"U1"},
    .name          = "alice",
    .displayName   = "Alice Wonder",
    .avatarUrl     = "https://av/alice.png",
    .isBot         = false,
    .isActive      = false,
    .isDeactivated = false,
};
static const User kBob = User{
    .id            = UserId{"U2"},
    .name          = "bob",
    .displayName   = "Bob Builder",
    .avatarUrl     = "",
    .isBot         = false,
    .isActive      = true,
    .isDeactivated = false,
};
// Counterparts that users.getPresence cannot answer for — querying them returns
// internal_error, so requestPresence must skip them.
static const User kBotUser = User{
    .id            = UserId{"B1"},
    .name          = "helperbot",
    .displayName   = "Helper Bot",
    .avatarUrl     = "",
    .isBot         = true,
    .isActive      = false,
    .isDeactivated = false,
};
static const User kSlackbot = User{
    .id            = UserId{"USLACKBOT"},
    .name          = "slackbot",
    .displayName   = "Slackbot",
    .avatarUrl     = "",
    .isBot         = false,
    .isActive      = false,
    .isDeactivated = false, // Slack reports is_bot=false for Slackbot
};
static const User kGone = User{
    .id            = UserId{"U3"},
    .name          = "ghost",
    .displayName   = "Departed Person",
    .avatarUrl     = "",
    .isBot         = false,
    .isActive      = false,
    .isDeactivated = true,
};
static const Conversation kGeneral = Conversation{
    .id       = ConversationId{"C1"},
    .kind     = ConvKind::PublicChannel,
    .name     = "general",
    .isMember = true,
    .lastRead = "0",
    .unread   = 2,
};
static const Conversation kRandom = Conversation{
    .id       = ConversationId{"C2"},
    .kind     = ConvKind::PublicChannel,
    .name     = "random",
    .isMember = true,
    .lastRead = "0",
    .unread   = 0,
};

struct SessionFixture {
    const QString            teamId = "T_SESSION_TEST";
    QString                  baseDir;
    StubBackend             *stub; // non-owning; owned by session
    std::unique_ptr<Session> session;

    SessionFixture() {
        baseDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;

        auto backend = std::make_unique<StubBackend>();
        stub         = backend.get();
        stub->_meId  = UserId{"U1"};
        stub->_convs = std::vector<Conversation>{kGeneral, kRandom};
        stub->_users = std::vector<User>{kAlice, kBob};

        session = std::make_unique<Session>(std::move(backend), teamId);
        session->start();
    }
    ~SessionFixture() {
        session.reset();
        QDir(baseDir).removeRecursively();
    }

    // Tear down the current session (its destructor flushes any debounced cache
    // saves) and start a fresh one against the same on-disk cache — a simulated
    // app restart.
    void restartSession(std::vector<Conversation> convs, std::vector<User> users) {
        auto  backend = std::make_unique<StubBackend>();
        auto *fresh   = backend.get();
        fresh->_meId  = UserId{"U1"};
        fresh->_convs = std::move(convs);
        fresh->_users = std::move(users);
        session.reset();
        stub    = fresh;
        session = std::make_unique<Session>(std::move(backend), teamId);
        session->start();
    }

    // Collect events fired after this helper is called.
    // Usage: auto [events, lt] = collectEvents();  stub->fireEvent(...);
    struct EventCollector {
        std::vector<Event> events;
        rpl::lifetime      lt;
    };
    EventCollector collectEvents() {
        EventCollector c;
        session->events() | rpl::on_next([&c](Event e) { c.events.push_back(std::move(e)); }, c.lt);
        return c;
    }
};

// ── start() — initial state ───────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "start() populates conversations from backend", "[session]") {
    REQUIRE(session->findConversation(ConversationId{"C1"}) != nullptr);
    REQUIRE(session->findConversation(ConversationId{"C2"}) != nullptr);
    CHECK(session->findConversation(ConversationId{"C1"})->name == "general");
    CHECK(session->findConversation(ConversationId{"C1"})->unread == 2);
}

TEST_CASE_METHOD(SessionFixture, "start() populates users from backend", "[session]") {
    REQUIRE(session->findUser(UserId{"U1"}) != nullptr);
    REQUIRE(session->findUser(UserId{"U2"}) != nullptr);
    CHECK(session->findUser(UserId{"U1"})->displayName == "Alice Wonder");
    CHECK(session->findUser(UserId{"U2"})->isActive == true);
}

TEST_CASE_METHOD(SessionFixture, "start() sets meUserId from loadMe", "[session]") {
    CHECK(session->meUserId() == UserId{"U1"});
}

// A DM peer absent from users.list (Slack system accounts like USLACK, Slack
// Connect partners, deactivated users) must be resolved on demand via users.info
// so the conversation list shows a real name instead of the raw id + "?" avatar.
TEST_CASE("DM peer missing from users.list is resolved via users.info", "[session]") {
    const QString teamId = "T_SESSION_MISSING_DM";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    Conversation slackDm{
        .id       = ConversationId{"D_SLACK"},
        .kind     = ConvKind::Im,
        .name     = "USLACK", // raw id fallback, exactly as conversations.list returns it
        .isMember = true,
        .lastRead = "0",
        .unread   = 0,
        .dmUser   = UserId{"USLACK"},
    };
    User slackUser{
        .id          = UserId{"USLACK"},
        .name        = "slack",
        .displayName = "Slack",
        .avatarUrl   = "https://a.slack-edge.com/slack_logo_mark.svg",
    };

    auto  backend = std::make_unique<StubBackend>();
    auto *stub    = backend.get();
    stub->_meId   = UserId{"U1"};
    stub->_convs  = std::vector<Conversation>{slackDm};
    stub->_users  = std::vector<User>{kAlice}; // USLACK deliberately NOT in users.list
    stub->userInfoResults["USLACK"] = slackUser;

    Session session(std::move(backend), teamId);
    session.start();

    CHECK(stub->userInfoRequested.contains("USLACK"));
    const User *u = session.findUser(UserId{"USLACK"});
    REQUIRE(u != nullptr);
    CHECK(u->displayName == "Slack");

    QDir(baseDir).removeRecursively();
}

// An external Slack Connect collaborator (W-prefixed enterprise id, absent from
// users.list) surfacing as a message author/mention is resolved on demand via
// users.info, and userInfoLoaded fires so the message view can re-render it.
TEST_CASE(
    "external collaborator resolved via fetchUserIfNeeded fires userInfoLoaded", "[session]"
) {
    const QString teamId = "T_SESSION_EXTERNAL";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    User ext{
        .id          = UserId{"W0EXTERNAL"},
        .name        = "ext.partner",
        .displayName = "External Partner",
        .avatarUrl   = "https://avatars.example/ext_72.png",
    };

    auto  backend = std::make_unique<StubBackend>();
    auto *stub    = backend.get();
    stub->_meId   = UserId{"U1"};
    stub->_users  = std::vector<User>{kAlice}; // collaborator deliberately absent
    stub->userInfoResults["W0EXTERNAL"] = ext;

    Session session(std::move(backend), teamId);
    session.start();

    QList<UserId> resolved;
    rpl::lifetime lt;
    session.userInfoLoaded() | rpl::on_next([&](UserId id) { resolved.append(id); }, lt);

    session.fetchUserIfNeeded(UserId{"W0EXTERNAL"});

    CHECK(stub->userInfoRequested.contains("W0EXTERNAL"));
    const User *u = session.findUser(UserId{"W0EXTERNAL"});
    REQUIRE(u != nullptr);
    CHECK(u->displayName == "External Partner");
    CHECK(u->avatarUrl == "https://avatars.example/ext_72.png");
    REQUIRE(resolved.size() == 1);
    CHECK(resolved.first() == UserId{"W0EXTERNAL"});

    // Already cached → no second fetch, no second signal.
    session.fetchUserIfNeeded(UserId{"W0EXTERNAL"});
    CHECK(stub->userInfoRequested.count("W0EXTERNAL") == 1);
    CHECK(resolved.size() == 1);

    QDir(baseDir).removeRecursively();
}

// A resolved external peer must survive a restart: users.list never returns
// Slack Connect users, so the cache written by fetchUserIfNeeded is the only
// startup source of their name, and the users.list merge must retain known
// users the snapshot omits instead of treating it as complete. Regression:
// names flickered to "Unknown user" for a few seconds after every start while
// users.info re-resolved each external peer one by one.
TEST_CASE("cached external peer survives restart and users.list reload", "[session]") {
    const QString teamId = "T_SESSION_EXTERNAL_RESTART";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    User ext{
        .id          = UserId{"W0EXTERNAL"},
        .name        = "ext.partner",
        .displayName = "External Partner",
        .avatarUrl   = "https://avatars.example/ext_72.png",
    };

    { // First run: resolve the external peer on demand; must persist to cache.
        auto  backend                       = std::make_unique<StubBackend>();
        auto *stub                          = backend.get();
        stub->_meId                         = UserId{"U1"};
        stub->_users                        = std::vector<User>{kAlice}; // peer deliberately absent
        stub->userInfoResults["W0EXTERNAL"] = ext;

        Session session(std::move(backend), teamId);
        session.start();
        session.fetchUserIfNeeded(UserId{"W0EXTERNAL"});
        REQUIRE(session.findUser(UserId{"W0EXTERNAL"}) != nullptr);
    }

    { // Second run: users.info deliberately yields nothing — the cached entry
      // must resolve the name by itself, and the users.list snapshot (which
      // still omits the peer) must not evict it.
        auto  backend = std::make_unique<StubBackend>();
        auto *stub    = backend.get();
        stub->_meId   = UserId{"U1"};
        stub->_users  = std::vector<User>{kAlice};

        Session session(std::move(backend), teamId);
        session.start();

        const User *u = session.findUser(UserId{"W0EXTERNAL"});
        REQUIRE(u != nullptr);
        CHECK(u->displayName == "External Partner");
        CHECK(session.userDisplayName(UserId{"W0EXTERNAL"}) == "External Partner");
        CHECK_FALSE(stub->userInfoRequested.contains("W0EXTERNAL"));
    }

    QDir(baseDir).removeRecursively();
}

// userDisplayName never returns the raw id: it resolves the cached name, and for
// an unknown user returns a placeholder while kicking off a users.info fetch.
TEST_CASE("userDisplayName never leaks a raw id and resolves in the background", "[session]") {
    const QString teamId = "T_SESSION_DISPLAYNAME";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    User ext{
        .id          = UserId{"W0PARTNER"},
        .name        = "partner",
        .displayName = "Partner Co",
    };

    auto  backend                      = std::make_unique<StubBackend>();
    auto *stub                         = backend.get();
    stub->_meId                        = UserId{"U1"};
    stub->_users                       = std::vector<User>{kAlice};
    stub->userInfoResults["W0PARTNER"] = ext;

    Session session(std::move(backend), teamId);
    session.start();

    // Known user → cached display label.
    CHECK(session.userDisplayName(kAlice.id) == kAlice.displayLabel());

    // Unknown user → never the raw id; a fetch is kicked off.
    const QString placeholder = session.userDisplayName(UserId{"W0PARTNER"});
    CHECK(placeholder != "W0PARTNER");
    CHECK_FALSE(placeholder.isEmpty());
    CHECK(stub->userInfoRequested.contains("W0PARTNER"));

    // After the (synchronous, in this stub) fetch lands, the real name is returned.
    REQUIRE(session.findUser(UserId{"W0PARTNER"}) != nullptr);
    CHECK(session.userDisplayName(UserId{"W0PARTNER"}) == "Partner Co");

    QDir(baseDir).removeRecursively();
}

// No DM peers to resolve (channels only) must not trigger any users.info call.
TEST_CASE_METHOD(
    SessionFixture, "no users.info fetch when there are no missing peers", "[session]"
) {
    CHECK(stub->userInfoRequested.isEmpty());
}

// isAppConversation() identifies bot/app DMs (the conv-list "Agents & apps"
// section), including the Slack system accounts that report is_bot=false. Such
// IMs are the ones whose canvas tab + probe MainWindow skips, so this must match
// ConvListWidget::isAppConv exactly.
TEST_CASE("isAppConversation identifies bot system and human conversations", "[session]") {
    const QString teamId = "T_SESSION_APP_CONV";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    auto im = [](const char *id, const char *peer) {
        return Conversation{
            .id       = ConversationId{id},
            .kind     = ConvKind::Im,
            .name     = peer,
            .isMember = true,
            .lastRead = "0",
            .dmUser   = UserId{peer},
        };
    };
    const Conversation botIm    = im("D_BOT", "B1");             // kBotUser: is_bot=true
    const Conversation slackbot = im("D_SLACKBOT", "USLACKBOT"); // system, is_bot=false
    const Conversation uslackIm = im("D_USLACK", "USLACK"); // system id, absent from users.list
    const Conversation humanDm  = im("D_BOB", "U2");        // kBob: human
    const Conversation externDm = im("D_EXT", "U_EXT");     // unknown peer, not a system id

    auto  backend = std::make_unique<StubBackend>();
    auto *stub    = backend.get();
    stub->_meId   = UserId{"U1"};
    stub->_convs =
        std::vector<Conversation>{kGeneral, botIm, slackbot, uslackIm, humanDm, externDm};
    stub->_users = std::vector<User>{kAlice, kBob, kBotUser, kSlackbot}; // USLACK/U_EXT absent

    Session session(std::move(backend), teamId);
    session.start();

    // Bot DM and the two Slack system accounts → app conversations.
    CHECK(session.isAppConversation(botIm));
    CHECK(session.isAppConversation(slackbot)); // is_bot=false but matched by fixed id
    CHECK(session.isAppConversation(uslackIm)); // matched by id even when not in users.list

    // Human DM, a channel, and an unresolved non-system peer → not app conversations.
    CHECK_FALSE(session.isAppConversation(humanDm));
    CHECK_FALSE(session.isAppConversation(kGeneral));
    CHECK_FALSE(session.isAppConversation(externDm)); // findUser null → assume human

    QDir(baseDir).removeRecursively();
}

TEST_CASE_METHOD(SessionFixture, "successful loadMe persists meUserId to cache", "[session]") {
    CHECK(WorkspaceCache(teamId).loadMeUserId() == UserId{"U1"});
}

// auth.test fired at start() can race the startup token refresh and fail.
// Without recovery, meUserId stays empty for the whole run and every send
// turns into a permanent duplicate: the optimistic ghost has no author, so
// the realtime echo is never recognized as "own" and never replaces it.

TEST_CASE(
    "loadMe is retried once users.list lands if auth.test raced the token refresh", "[session]"
) {
    const QString teamId = "T_SESSION_ME_RETRY";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively(); // pristine first run — nothing cached

    auto  backend              = std::make_unique<StubBackend>();
    auto *stub                 = backend.get();
    stub->loadMeFirstCallFails = true;
    stub->_meId                = UserId{"U1"};
    stub->_convs               = std::vector<Conversation>{kGeneral};
    stub->_users               = std::vector<User>{kAlice, kBob};

    Session session(std::move(backend), teamId);
    session.start();

    CHECK(stub->loadMeCalls == 2);
    CHECK(session.meUserId() == UserId{"U1"});

    QDir(baseDir).removeRecursively();
}

TEST_CASE(
    "cached meUserId keeps optimistic sends working when auth.test fails all run", "[session][send]"
) {
    const QString teamId = "T_SESSION_ME_CACHED";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();
    {
        WorkspaceCache cache(teamId);
        cache.saveMeUserId(UserId{"U1"}); // a previous run learned the id
    }

    auto  backend           = std::make_unique<StubBackend>();
    auto *stub              = backend.get();
    stub->loadMeAlwaysFails = true;
    stub->_convs            = std::vector<Conversation>{kGeneral};
    stub->_users            = std::vector<User>{kAlice, kBob};

    Session session(std::move(backend), teamId);
    session.start();
    CHECK(session.meUserId() == UserId{"U1"});

    std::vector<Event> events;
    rpl::lifetime      lt;
    session.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);

    session.sendMessage(ConversationId{"C1"}, "hello");
    REQUIRE(events.size() == 1);
    // Author comes from the cached id — renders with the right name/avatar.
    CHECK(std::get<EvMessageNew>(events[0]).msg.author == UserId{"U1"});
    const Ts fakeTs = std::get<EvMessageNew>(events[0]).msg.ts;

    // The realtime echo of our own message must still replace the ghost.
    Message real;
    real.ts     = "200.000";
    real.author = UserId{"U1"};
    stub->fireEvent(EvMessageNew{ConversationId{"C1"}, real});

    REQUIRE(events.size() == 3);
    REQUIRE(std::holds_alternative<EvMessageDeleted>(events[1]));
    CHECK(std::get<EvMessageDeleted>(events[1]).ts == fakeTs);

    QDir(baseDir).removeRecursively();
}

// ── findUser / findConversation ───────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "findUser returns null for unknown id", "[session]") {
    CHECK(session->findUser(UserId{"U_GHOST"}) == nullptr);
}

TEST_CASE_METHOD(SessionFixture, "findConversation returns null for unknown id", "[session]") {
    CHECK(session->findConversation(ConversationId{"C_GHOST"}) == nullptr);
}

// ── EvPresenceChanged ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "EvPresenceChanged updates user isActive", "[session][events]") {
    REQUIRE(!session->findUser(UserId{"U1"})->isActive);
    stub->fireEvent(EvPresenceChanged{UserId{"U1"}, true});
    REQUIRE(session->findUser(UserId{"U1"}) != nullptr);
    CHECK(session->findUser(UserId{"U1"})->isActive == true);
}

TEST_CASE_METHOD(
    SessionFixture, "EvPresenceChanged is forwarded to subscribers", "[session][events]"
) {
    auto col = collectEvents();
    stub->fireEvent(EvPresenceChanged{UserId{"U1"}, true});
    REQUIRE(col.events.size() == 1);
    CHECK(std::holds_alternative<EvPresenceChanged>(col.events[0]));
    CHECK(std::get<EvPresenceChanged>(col.events[0]).active == true);
}

// ── EvUserChanged ─────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "EvUserChanged updates avatar and display name", "[session][events]"
) {
    REQUIRE(session->findUser(UserId{"U1"})->avatarUrl == "https://av/alice.png");
    User updated        = kAlice;
    updated.avatarUrl   = "https://av/alice-v2.png";
    updated.displayName = "Alice Newname";
    stub->fireEvent(EvUserChanged{updated});
    const User *u = session->findUser(UserId{"U1"});
    REQUIRE(u != nullptr);
    CHECK(u->avatarUrl == "https://av/alice-v2.png");
    CHECK(u->displayName == "Alice Newname");
}

TEST_CASE_METHOD(
    SessionFixture, "EvUserChanged preserves live presence and DND", "[session][events]"
) {
    // Bring U1 online and mark DND, then let a profile update arrive whose
    // payload carries no presence info (user_change never does).
    stub->fireEvent(EvPresenceChanged{UserId{"U1"}, true});
    stub->fireEvent(EvDndChanged{UserId{"U1"}, true});
    User updated      = kAlice; // isActive=false, dndEnabled=false
    updated.avatarUrl = "https://av/alice-v3.png";
    stub->fireEvent(EvUserChanged{updated});
    const User *u = session->findUser(UserId{"U1"});
    REQUIRE(u != nullptr);
    CHECK(u->avatarUrl == "https://av/alice-v3.png");
    CHECK(u->isActive == true);   // not clobbered by the merge
    CHECK(u->dndEnabled == true); // not clobbered by the merge
}

TEST_CASE_METHOD(SessionFixture, "EvUserChanged is forwarded to subscribers", "[session][events]") {
    auto col = collectEvents();
    stub->fireEvent(EvUserChanged{kAlice});
    REQUIRE(col.events.size() == 1);
    CHECK(std::holds_alternative<EvUserChanged>(col.events[0]));
}

// ── EvUsersChanged (bulk) ─────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "EvUsersChanged merges the whole batch in one roster update",
    "[session][events]"
) {
    // Live presence set before the batch must survive the merge (the batch
    // payload never carries presence, same as EvUserChanged).
    stub->fireEvent(EvPresenceChanged{UserId{"U1"}, true});

    User alice      = kAlice; // isActive=false in the payload
    alice.avatarUrl = "https://brand/acme.svg";
    User bob        = kBob;
    bob.avatarUrl   = "https://brand/globex.png";
    User carol{.id = UserId{"U3"}, .name = "carol", .avatarUrl = "https://brand/initech.png"};

    // Count users() re-emissions: the whole point of the bulk event is ONE
    // rebuild for N users instead of N.
    int  emissions = 0;
    auto lt        = rpl::lifetime();
    session->users() | rpl::on_next([&](const std::vector<User> &) { ++emissions; }, lt);
    emissions = 0; // discard the initial replay emission

    stub->fireEvent(EvUsersChanged{{alice, bob, carol}});

    CHECK(emissions == 1);
    const User *a = session->findUser(UserId{"U1"});
    REQUIRE(a != nullptr);
    CHECK(a->avatarUrl == "https://brand/acme.svg");
    CHECK(a->isActive == true); // live presence not clobbered
    const User *b = session->findUser(UserId{"U2"});
    REQUIRE(b != nullptr);
    CHECK(b->avatarUrl == "https://brand/globex.png");
    const User *c = session->findUser(UserId{"U3"}); // unknown user appended
    REQUIRE(c != nullptr);
    CHECK(c->avatarUrl == "https://brand/initech.png");
}

TEST_CASE_METHOD(
    SessionFixture, "EvUsersChanged is forwarded to subscribers", "[session][events]"
) {
    auto col = collectEvents();
    stub->fireEvent(EvUsersChanged{{kAlice, kBob}});
    REQUIRE(col.events.size() == 1);
    REQUIRE(std::holds_alternative<EvUsersChanged>(col.events[0]));
    CHECK(std::get<EvUsersChanged>(col.events[0]).users.size() == 2);
}

// ── EvConvMarked ──────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "EvConvMarked updates conversation unread and lastRead", "[session][events]"
) {
    stub->fireEvent(EvConvMarked{ConversationId{"C1"}, "999.000", 0});
    auto *c = session->findConversation(ConversationId{"C1"});
    REQUIRE(c != nullptr);
    CHECK(c->unread == 0);
    CHECK(c->lastRead == "999.000");
}

// ── EvMemberJoined ────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "EvMemberJoined for self adds a previously-untracked channel to the list",
    "[session][events]"
) {
    // We get added to C9, which isn't in our conversation list. The
    // conversations.info fixture reports it as a member channel.
    Conversation joined{
        .id       = ConversationId{"C9"},
        .kind     = ConvKind::PublicChannel,
        .name     = "newcomers",
        .isMember = true,
    };
    stub->infoResults["C9"] = joined;

    REQUIRE(session->findConversation(ConversationId{"C9"}) == nullptr);
    stub->fireEvent(EvMemberJoined{ConversationId{"C9"}, UserId{"U1"}});

    CHECK(stub->infoRequested.contains("C9"));
    auto *c = session->findConversation(ConversationId{"C9"});
    REQUIRE(c != nullptr);
    CHECK(c->name == "newcomers");
    CHECK(c->isMember);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMemberJoined for a channel we already track as a member is a no-op",
    "[session][events]"
) {
    // C2 (kRandom) is already in the list as a member, so a member_joined echo
    // for it must not trigger a redundant conversations.info fetch.
    stub->fireEvent(EvMemberJoined{ConversationId{"C2"}, UserId{"U1"}});
    CHECK_FALSE(stub->infoRequested.contains("C2"));
}

TEST_CASE_METHOD(
    SessionFixture, "EvMemberJoined for another user is ignored", "[session][events]"
) {
    stub->infoResults["C9"] = Conversation{.id = ConversationId{"C9"}, .isMember = true};
    stub->fireEvent(EvMemberJoined{ConversationId{"C9"}, UserId{"U2"}});
    CHECK_FALSE(stub->infoRequested.contains("C9"));
    CHECK(session->findConversation(ConversationId{"C9"}) == nullptr);
}

// ── EvMessageNew unread logic ─────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew increments unread for other-user message in non-reading conv",
    "[session][events]"
) {
    // C2 starts at unread=0; U2 sends a message; unread should become 1.
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
}

TEST_CASE_METHOD(
    SessionFixture, "EvMessageNew does not increment unread for own message", "[session][events]"
) {
    // meUserId is U1; U1 sends a message — unread stays at 0.
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U1"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

// ── EvMessageNew for an unknown conversation (new MPDM) ───────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew for an unknown MPDM fetches it, adds it to the list, and badges",
    "[session][events]"
) {
    // A brand-new group DM (G9) we don't track yet: Slack sends no
    // channel_created/member_joined_channel for MPDMs, only this message. The
    // conversations.info fixture reports it (with a stale server unread that we
    // must NOT trust — the replayed message does the authoritative counting).
    Conversation mpdm{
        .id       = ConversationId{"G9"},
        .kind     = ConvKind::Mpim,
        .name     = "mpdm-alice--bob--u1-1",
        .isMember = true,
        .lastRead = "0",
        .unread   = 5, // server count; zeroed on insert so no double-count
        .members  = {UserId{"U1"}, UserId{"U2"}, UserId{"U3"}},
    };
    stub->infoResults["G9"] = mpdm;

    REQUIRE(session->findConversation(ConversationId{"G9"}) == nullptr);

    auto [events, lt] = collectEvents();
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"G9"}, msg});

    // Conversation was fetched and folded in.
    CHECK(stub->infoRequested.contains("G9"));
    auto *c = session->findConversation(ConversationId{"G9"});
    REQUIRE(c != nullptr);
    CHECK(c->kind == ConvKind::Mpim);
    // Counted from the replayed message, not stacked on the server's 5.
    CHECK(c->unread == 1);
    CHECK(c->mentionCount == 1); // an MPDM message is always a red-badge mention
    CHECK(c->latestTs == "500.000");

    // The message is forwarded to the UI so it renders and notifies.
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<EvMessageNew>(events[0]));
    CHECK(std::get<EvMessageNew>(events[0]).conv == ConversationId{"G9"});
    CHECK(std::get<EvMessageNew>(events[0]).msg.ts == "500.000");
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew for an unknown conversation that can't be fetched is dropped, not stuck",
    "[session][events]"
) {
    // No conversations.info fixture for G9 → the fetch completes with no value
    // (Slack's channel_not_found; e.g. another workspace's event off the shared
    // socket). The conversation is not added, nothing is forwarded, and the
    // in-flight marker is cleared so a later message re-attempts the fetch.
    auto [events, lt] = collectEvents();

    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"G9"}, msg});

    CHECK(session->findConversation(ConversationId{"G9"}) == nullptr);
    CHECK(events.empty());
    CHECK(stub->infoRequested.count("G9") == 1);

    // A second message re-attempts (the backlog wasn't left stuck).
    Message msg2;
    msg2.ts     = "600.000";
    msg2.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"G9"}, msg2});
    CHECK(stub->infoRequested.count("G9") == 2);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew for a channel_not_found conversation is remembered and not re-fetched",
    "[session][events]"
) {
    // conversations.info answers channel_not_found (the not-found sentinel):
    // this conv does not exist for this workspace — e.g. a busy channel in
    // another workspace whose events arrive over the shared socket. The first
    // message fetches once; every later message must be dropped WITHOUT another
    // conversations.info call, so a chatty foreign channel can't spam the API.
    stub->infoResults["C77"] = Conversation{.id = ConversationId{"C77"}, .notFound = true};

    auto [events, lt] = collectEvents();

    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C77"}, msg});

    CHECK(session->findConversation(ConversationId{"C77"}) == nullptr);
    CHECK(events.empty());
    REQUIRE(stub->infoRequested.count("C77") == 1);

    // Two more messages — neither triggers a fetch now that the id is known dead.
    Message msg2;
    msg2.ts     = "600.000";
    msg2.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C77"}, msg2});
    Message msg3;
    msg3.ts     = "700.000";
    msg3.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C77"}, msg3});

    CHECK(stub->infoRequested.count("C77") == 1); // still just the one call
    CHECK(events.empty());
}

TEST_CASE_METHOD(
    SessionFixture,
    "a channel_not_found conversation becomes fetchable again once we join it",
    "[session][events]"
) {
    // Blacklist C9 via channel_not_found, then get invited (member_joined_channel
    // for us). Joining must clear the negative cache so the channel is fetched
    // and folded in, and subsequent messages notify normally — no lost messages
    // for a conversation that legitimately becomes ours.
    stub->infoResults["C9"] = Conversation{.id = ConversationId{"C9"}, .notFound = true};

    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C9"}, msg});
    REQUIRE(stub->infoRequested.count("C9") == 1);
    REQUIRE(session->findConversation(ConversationId{"C9"}) == nullptr);

    // We're added to C9. From now conversations.info reports a real channel.
    stub->infoResults["C9"] = Conversation{
        .id       = ConversationId{"C9"},
        .kind     = ConvKind::PublicChannel,
        .name     = "just-joined",
        .isMember = true,
        .lastRead = "0",
    };
    stub->fireEvent(EvMemberJoined{ConversationId{"C9"}, UserId{"U1"}});

    // markConvAlive dropped the dead mark, so the join fetch went through and the
    // channel is now tracked.
    CHECK(stub->infoRequested.count("C9") == 2);
    auto *c = session->findConversation(ConversationId{"C9"});
    REQUIRE(c != nullptr);
    CHECK(c->name == "just-joined");

    // A later message now flows through the normal path and is forwarded.
    auto [events, lt] = collectEvents();
    Message msg2;
    msg2.ts     = "800.000";
    msg2.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C9"}, msg2});
    REQUIRE(events.size() == 1);
    CHECK(std::get<EvMessageNew>(events[0]).conv == ConversationId{"C9"});
    // And no extra conversations.info call — it's tracked, so no unknown-conv fetch.
    CHECK(stub->infoRequested.count("C9") == 2);
}

TEST_CASE_METHOD(
    SessionFixture,
    "the channel_not_found negative cache persists across restarts",
    "[session][events]"
) {
    // First run learns C77 is foreign. The set is persisted, so the next run
    // must NOT re-probe conversations.info for it — previously every launch
    // re-learned the whole set from scratch, a startup burst of
    // channel_not_found per workspace.
    stub->infoResults["C77"] = Conversation{.id = ConversationId{"C77"}, .notFound = true};
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C77"}, msg});
    REQUIRE(stub->infoRequested.count("C77") == 1);

    restartSession({kGeneral, kRandom}, {kAlice, kBob});

    Message msg2;
    msg2.ts     = "600.000";
    msg2.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C77"}, msg2});
    CHECK(stub->infoRequested.count("C77") == 0); // remembered from the previous run
    CHECK(session->findConversation(ConversationId{"C77"}) == nullptr);
}

TEST_CASE_METHOD(
    SessionFixture,
    "a persisted dead mark clears when the conversation shows up in conversations.list",
    "[session][events]"
) {
    // C77 went dead in a previous run, then we were invited while the app was
    // closed — no realtime join event ever reached us, but the next run's
    // conversations.list includes it. The stale mark must not survive the list
    // (reconcileDeadConvIds), or the channel would stay excluded from the
    // sweeps despite being ours.
    stub->infoResults["C77"] = Conversation{.id = ConversationId{"C77"}, .notFound = true};
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C77"}, msg});
    REQUIRE(stub->infoRequested.count("C77") == 1);

    const Conversation joined{
        .id       = ConversationId{"C77"},
        .kind     = ConvKind::PublicChannel,
        .name     = "now-ours",
        .isMember = true,
        .lastRead = "0",
    };
    restartSession({kGeneral, kRandom, joined}, {kAlice, kBob});

    session.reset(); // flush the reconciled set to disk
    CHECK(!WorkspaceCache(teamId).loadDeadConvIds().contains("C77"));
}

TEST_CASE_METHOD(
    SessionFixture,
    "a dead DM survives list presence but clears when its peer is reactivated",
    "[session][events]"
) {
    // Slack lists a DM forever even when its peer is deactivated and
    // conversations.info answers channel_not_found, so list presence alone must
    // NOT clear a DM's dead mark — that would re-probe (and re-mark) every dead
    // DM on every launch. The peer coming back to life is the real signal.
    const Conversation dmGone{
        .id       = ConversationId{"D9"},
        .kind     = ConvKind::Im,
        .name     = "ghost",
        .isMember = true,
        .lastRead = "0",
        .dmUser   = UserId{"U3"},
    };
    session.reset();
    WorkspaceCache(teamId).saveDeadConvIds({"D9"});

    restartSession({kGeneral, dmGone}, {kAlice, kGone});
    SECTION("deactivated peer: the mark stays") {
        session.reset();
        CHECK(WorkspaceCache(teamId).loadDeadConvIds().contains("D9"));
    }
    SECTION("peer reactivated (EvUserChanged): the mark clears") {
        User revived          = kGone;
        revived.isDeactivated = false;
        stub->fireEvent(EvUserChanged{revived});
        session.reset();
        CHECK(!WorkspaceCache(teamId).loadDeadConvIds().contains("D9"));
    }
}

TEST_CASE_METHOD(
    SessionFixture, "EvMessageNew does not increment unread for muted conv", "[session][events]"
) {
    Conversation muted;
    muted.id      = ConversationId{"C3"};
    muted.kind    = ConvKind::PublicChannel;
    muted.name    = "muted-chan";
    muted.isMuted = true;
    stub->_convs  = std::vector<Conversation>{kGeneral, kRandom, muted};

    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C3"}, msg});
    CHECK(session->findConversation(ConversationId{"C3"})->unread == 0);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew with @mention in muted channel still badges (official-client behavior)",
    "[session][events]"
) {
    Conversation muted;
    muted.id       = ConversationId{"C3"};
    muted.kind     = ConvKind::PublicChannel;
    muted.name     = "muted-chan";
    muted.isMember = true;
    muted.isMuted  = true;
    stub->_convs   = std::vector<Conversation>{kGeneral, kRandom, muted};

    Message msg;
    msg.ts      = "500.000";
    msg.author  = UserId{"U2"};
    msg.rawText = "hey <@U1> look at this";
    stub->fireEvent(EvMessageNew{ConversationId{"C3"}, msg});
    CHECK(session->findConversation(ConversationId{"C3"})->unread == 1);
    CHECK(session->findConversation(ConversationId{"C3"})->mentionCount == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew for a plain channel thread reply does not mark the channel unread",
    "[session][events]"
) {
    Message msg;
    msg.ts         = "501.000";
    msg.threadRoot = Ts{"500.000"};
    msg.author     = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);

    // …but a mention inside a thread still badges.
    msg.ts      = "502.000";
    msg.rawText = "<@U1> ping";
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
    CHECK(session->findConversation(ConversationId{"C2"})->mentionCount == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew for a reply to a thread I started badges like a mention",
    "[session][events]"
) {
    // meUserId is U1. U2 replies to a thread whose root I authored — Slack tags
    // the reply with parent_user_id=U1. Even without an @mention, this must badge
    // the channel (red mention badge), matching "replies to threads you follow".
    Message msg;
    msg.ts           = "501.000";
    msg.threadRoot   = Ts{"500.000"};
    msg.parentUserId = UserId{"U1"};
    msg.author       = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    auto *c = session->findConversation(ConversationId{"C2"});
    CHECK(c->unread == 1);
    CHECK(c->mentionCount == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew for a reply to someone else's thread does not badge",
    "[session][events]"
) {
    // Reply to a thread U3 started (parent_user_id=U3, not me) with no mention:
    // stays in the Threads view, no channel badge — unchanged behavior.
    Message msg;
    msg.ts           = "501.000";
    msg.threadRoot   = Ts{"500.000"};
    msg.parentUserId = UserId{"U3"};
    msg.author       = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

// ── Thread mute ───────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "muting a thread suppresses its followed-reply badge", "[session][thread]"
) {
    const ConversationId conv{"C2"};
    const Ts             root{"500.000"};
    CHECK_FALSE(session->isThreadMuted(conv, root));
    session->setThreadMuted(conv, root, true);
    CHECK(session->isThreadMuted(conv, root));

    // A reply to my own thread would normally badge (isFollowedThreadReply); the
    // mute suppresses it.
    Message reply;
    reply.ts           = "501.000";
    reply.threadRoot   = root;
    reply.parentUserId = UserId{"U1"}; // I started the thread
    reply.author       = UserId{"U2"};
    stub->fireEvent(EvMessageNew{conv, reply});
    CHECK(session->findConversation(conv)->unread == 0);

    // Unmuting lets the next reply badge again.
    session->setThreadMuted(conv, root, false);
    Message reply2 = reply;
    reply2.ts      = "502.000";
    stub->fireEvent(EvMessageNew{conv, reply2});
    CHECK(session->findConversation(conv)->unread == 1);
}

TEST_CASE_METHOD(
    SessionFixture, "a muted thread still badges an explicit @mention", "[session][thread]"
) {
    const ConversationId conv{"C2"};
    const Ts             root{"500.000"};
    session->setThreadMuted(conv, root, true);

    Message reply;
    reply.ts         = "501.000";
    reply.threadRoot = root;
    reply.author     = UserId{"U2"};
    reply.rawText    = "<@U1> please look"; // mentions get through a muted thread
    stub->fireEvent(EvMessageNew{conv, reply});
    auto *c = session->findConversation(conv);
    CHECK(c->unread == 1);
    CHECK(c->mentionCount == 1);
}

TEST_CASE("thread mute persists across sessions", "[session][thread]") {
    const QString teamId = "T_SESSION_THREAD_MUTE";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    const ConversationId conv{"C2"};
    const Ts             root{"500.000"};
    {
        auto backend    = std::make_unique<StubBackend>();
        backend->_meId  = UserId{"U1"};
        backend->_convs = std::vector<Conversation>{kRandom};
        Session session(std::move(backend), teamId);
        session.start();
        session.setThreadMuted(conv, root, true);
    }
    // A fresh Session on the same workspace loads the mute from cache.
    {
        auto backend    = std::make_unique<StubBackend>();
        backend->_meId  = UserId{"U1"};
        backend->_convs = std::vector<Conversation>{kRandom};
        Session session(std::move(backend), teamId);
        session.start();
        CHECK(session.isThreadMuted(conv, root));
        CHECK_FALSE(session.isThreadMuted(conv, Ts{"999.000"}));
    }
    QDir(baseDir).removeRecursively();
}

TEST_CASE_METHOD(
    SessionFixture, "EvMessageNew counts @here broadcast as a mention", "[session][events]"
) {
    Message msg;
    msg.ts      = "500.000";
    msg.author  = UserId{"U2"};
    msg.rawText = "<!here> standup time";
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
    CHECK(session->findConversation(ConversationId{"C2"})->mentionCount == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew does not increment unread for currently reading conv",
    "[session][events]"
) {
    session->setReading(ConversationId{"C2"});
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

// ── setReading ────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "setReading zeroes unread badge for conversation", "[session]") {
    // C1 starts at unread=2.
    REQUIRE(session->findConversation(ConversationId{"C1"})->unread == 2);
    session->setReading(ConversationId{"C1"});
    CHECK(session->findConversation(ConversationId{"C1"})->unread == 0);
}

TEST_CASE_METHOD(SessionFixture, "setReading with empty id does not crash", "[session]") {
    CHECK_NOTHROW(session->setReading(ConversationId{""}));
}

TEST_CASE_METHOD(SessionFixture, "setReading syncs the read cursor to the backend", "[session]") {
    // A message arrives in C2, then the user opens it — Slack must be told
    // it's read so other clients (and the next restart) agree.
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});

    session->setReading(ConversationId{"C2"});
    REQUIRE(stub->markReadCalls.size() == 1);
    CHECK(stub->markReadCalls[0].conv == ConversationId{"C2"});
    CHECK(stub->markReadCalls[0].ts == "500.000");
    CHECK(session->findConversation(ConversationId{"C2"})->lastRead == "500.000");
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew in the currently-reading conv marks it read on the backend",
    "[session][events]"
) {
    session->setReading(ConversationId{"C2"});
    stub->markReadCalls.clear();

    Message msg;
    msg.ts     = "600.000";
    msg.author = UserId{"U2"};
    stub->fireEvent(EvMessageNew{ConversationId{"C2"}, msg});
    REQUIRE(stub->markReadCalls.size() == 1);
    CHECK(stub->markReadCalls[0].ts == "600.000");
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

// ── mrkdwnMentions ────────────────────────────────────────────────────────────

TEST_CASE("mrkdwnMentions matches direct and piped mentions", "[session][mentions]") {
    const UserId me{"U1"};
    CHECK(mrkdwnMentions("hi <@U1> there", me));
    CHECK(mrkdwnMentions("hi <@U1|vladimir> there", me));
    CHECK_FALSE(mrkdwnMentions("hi <@U12> there", me)); // prefix must not match
    CHECK_FALSE(mrkdwnMentions("plain text", me));
    CHECK_FALSE(mrkdwnMentions("hi <@U2>", me));
}

TEST_CASE("mrkdwnMentions matches broadcast keywords", "[session][mentions]") {
    const UserId me{"U1"};
    CHECK(mrkdwnMentions("<!here> hello", me));
    CHECK(mrkdwnMentions("<!channel> hello", me));
    CHECK(mrkdwnMentions("<!everyone> hello", me));
    CHECK_FALSE(mrkdwnMentions("here channel everyone", me));
}

// ── sendMessage ───────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "sendMessage fires optimistic EvMessageNew", "[session][send]") {
    auto col = collectEvents();
    session->sendMessage(ConversationId{"C1"}, "hello");
    REQUIRE(col.events.size() == 1);
    REQUIRE(std::holds_alternative<EvMessageNew>(col.events[0]));
    const auto &ev = std::get<EvMessageNew>(col.events[0]);
    CHECK(ev.conv == ConversationId{"C1"});
    CHECK(ev.msg.text.text == "hello");
    CHECK(ev.msg.author == UserId{"U1"});
}

TEST_CASE_METHOD(
    SessionFixture, "sendMessage includes threadRoot in optimistic event", "[session][send]"
) {
    auto col = collectEvents();
    session->sendMessage(ConversationId{"C1"}, "reply", Ts{"100.000"});
    REQUIRE(col.events.size() == 1);
    const auto &ev = std::get<EvMessageNew>(col.events[0]);
    REQUIRE(ev.msg.threadRoot.has_value());
    CHECK(*ev.msg.threadRoot == "100.000");
}

TEST_CASE_METHOD(SessionFixture, "sendMessage delegates to backend", "[session][send]") {
    session->sendMessage(ConversationId{"C1"}, "hi");
    REQUIRE(stub->sentMessages.size() == 1);
    CHECK(stub->sentMessages[0].conv == ConversationId{"C1"});
    CHECK(stub->sentMessages[0].msg.text.text == "hi");
}

TEST_CASE_METHOD(SessionFixture, "sendMessage optimistic copy is pending", "[session][send]") {
    auto col = collectEvents();
    session->sendMessage(ConversationId{"C1"}, "hello");
    REQUIRE(col.events.size() == 1);
    CHECK(std::get<EvMessageNew>(col.events[0]).msg.pending);
}

TEST_CASE_METHOD(
    SessionFixture, "own realtime message removes the optimistic copy", "[session][send]"
) {
    auto col = collectEvents();
    session->sendMessage(ConversationId{"C1"}, "hello");
    const Ts fakeTs = std::get<EvMessageNew>(col.events[0]).msg.ts;

    Message real;
    real.ts     = "200.000";
    real.author = UserId{"U1"};
    stub->fireEvent(EvMessageNew{ConversationId{"C1"}, real});

    // optimistic new + ghost delete + real new
    REQUIRE(col.events.size() == 3);
    REQUIRE(std::holds_alternative<EvMessageDeleted>(col.events[1]));
    CHECK(std::get<EvMessageDeleted>(col.events[1]).ts == fakeTs);
    CHECK_FALSE(std::get<EvMessageNew>(col.events[2]).msg.pending);
}

// ── uploadFiles ───────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "uploadFiles fires optimistic pending message with files", "[session][upload]"
) {
    QTemporaryDir dir;
    const QString path = dir.path() + "/notes.txt";
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("hello world");
    }

    auto col = collectEvents();
    session->uploadFiles(ConversationId{"C1"}, {path}, "see attached");

    REQUIRE(col.events.size() == 1);
    const auto &ev = std::get<EvMessageNew>(col.events[0]);
    CHECK(ev.msg.pending);
    CHECK(ev.msg.text.text == "see attached");
    REQUIRE(ev.msg.files.size() == 1);
    CHECK(ev.msg.files[0].name == "notes.txt");
    CHECK(ev.msg.files[0].size == 11);

    REQUIRE(stub->uploadCalls.size() == 1);
    CHECK(stub->uploadCalls[0].conv == ConversationId{"C1"});
    CHECK(stub->uploadCalls[0].comment == "see attached");
}

TEST_CASE_METHOD(
    SessionFixture, "failed upload removes the ghost and reports an error", "[session][upload]"
) {
    auto          col = collectEvents();
    QString       error;
    rpl::lifetime errLt;
    session->errors() | rpl::on_next([&](QString e) { error = std::move(e); }, errLt);

    session->uploadFiles(ConversationId{"C1"}, {"/nonexistent/big.bin"}, "");
    const Ts fakeTs = std::get<EvMessageNew>(col.events[0]).msg.ts;

    REQUIRE(stub->uploadCalls.size() == 1);
    stub->uploadCalls[0].done(false, "boom");

    REQUIRE(col.events.size() == 2);
    REQUIRE(std::holds_alternative<EvMessageDeleted>(col.events[1]));
    CHECK(std::get<EvMessageDeleted>(col.events[1]).ts == fakeTs);
    CHECK(error.contains("boom"));
}

TEST_CASE_METHOD(
    SessionFixture,
    "text confirmation does not displace a pending upload ghost",
    "[session][upload]"
) {
    auto col = collectEvents();
    session->uploadFiles(ConversationId{"C1"}, {"/nonexistent/big.bin"}, "slow upload");
    session->sendMessage(ConversationId{"C1"}, "quick text");
    const Ts textFakeTs = std::get<EvMessageNew>(col.events[1]).msg.ts;

    // The quick text confirms first; it must remove the text ghost, not the upload's.
    Message real;
    real.ts     = "300.000";
    real.author = UserId{"U1"};
    stub->fireEvent(EvMessageNew{ConversationId{"C1"}, real});

    REQUIRE(col.events.size() == 4);
    REQUIRE(std::holds_alternative<EvMessageDeleted>(col.events[2]));
    CHECK(std::get<EvMessageDeleted>(col.events[2]).ts == textFakeTs);
}

TEST_CASE_METHOD(
    SessionFixture,
    "uploadFiles in a thread tags the ghost and forwards the thread root",
    "[session][upload]"
) {
    QTemporaryDir dir;
    const QString path = dir.path() + "/pic.png";
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("img");
    }

    auto col = collectEvents();
    session->uploadFiles(ConversationId{"C1"}, {path}, "in thread", Ts{"100.500"});

    REQUIRE(col.events.size() == 1);
    const auto &ev = std::get<EvMessageNew>(col.events[0]);
    REQUIRE(ev.msg.threadRoot.has_value());
    CHECK(*ev.msg.threadRoot == "100.500");

    REQUIRE(stub->uploadCalls.size() == 1);
    REQUIRE(stub->uploadCalls[0].threadRoot.has_value());
    CHECK(*stub->uploadCalls[0].threadRoot == "100.500");
}

// ── requestPresence ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "requestPresence updates user and fires EvPresenceChanged", "[session]"
) {
    stub->presenceResult = true;
    auto col             = collectEvents();
    session->requestPresence(UserId{"U1"});
    // Presence updated in-memory
    CHECK(session->findUser(UserId{"U1"})->isActive == true);
    // Event forwarded to subscribers
    REQUIRE(col.events.size() == 1);
    REQUIRE(std::holds_alternative<EvPresenceChanged>(col.events[0]));
    CHECK(std::get<EvPresenceChanged>(col.events[0]).active == true);
}

TEST_CASE_METHOD(
    SessionFixture, "requestPresence skips users with no observable presence", "[session]"
) {
    // Bot/app users, Slackbot (is_bot=false but fixed id), deactivated accounts,
    // and users not in this workspace all make users.getPresence return
    // internal_error — never call it for them.
    stub->_users = std::vector<User>{kAlice, kBob, kBotUser, kSlackbot, kGone};
    QCoreApplication::processEvents(); // propagate the user-list update into the cache

    stub->loadPresenceCalls = 0;
    session->requestPresence(UserId{"B1"});        // bot
    session->requestPresence(UserId{"USLACKBOT"}); // Slackbot
    session->requestPresence(UserId{"U3"});        // deactivated
    session->requestPresence(UserId{"U_UNKNOWN"}); // not in users.list (e.g. Slack Connect)
    CHECK(stub->loadPresenceCalls == 0);

    // A real, known, human member is still queried.
    session->requestPresence(UserId{"U1"});
    CHECK(stub->loadPresenceCalls == 1);
    CHECK(stub->lastPresenceUser == UserId{"U1"});
}

// ── refreshSelfPresence ───────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "refreshSelfPresence stores snapshot and syncs own isActive", "[session]"
) {
    stub->selfPresenceResult =
        SelfPresence{.loaded = true, .active = true, .online = true, .connectionCount = 1};
    auto col = collectEvents();
    session->refreshSelfPresence();

    CHECK(session->currentSelfPresence().active == true);
    CHECK_FALSE(session->currentSelfPresence().phantomAway());
    // Me (U1) starts inactive in the fixture → snapshot flips it and fires the event.
    CHECK(session->findUser(UserId{"U1"})->isActive == true);
    REQUIRE(col.events.size() == 1);
    REQUIRE(std::holds_alternative<EvPresenceChanged>(col.events[0]));
    CHECK(std::get<EvPresenceChanged>(col.events[0]).user == UserId{"U1"});
}

TEST_CASE_METHOD(
    SessionFixture, "refreshSelfPresence detects phantom away (no client connected)", "[session]"
) {
    stub->selfPresenceResult = SelfPresence{.loaded = true};
    session->refreshSelfPresence();
    CHECK(session->currentSelfPresence().phantomAway());
}

TEST_CASE_METHOD(SessionFixture, "manual away is not phantom away", "[session]") {
    stub->selfPresenceResult = SelfPresence{.loaded = true, .manualAway = true};
    session->refreshSelfPresence();
    CHECK_FALSE(session->currentSelfPresence().phantomAway());
}

TEST_CASE_METHOD(
    SessionFixture,
    "self presence is re-polled after users load when the first call fails",
    "[session]"
) {
    // Simulates the startup race: the immediate poll in start() can fail while
    // the token is being refreshed; the snapshot must still land right after
    // users.list arrives, not a full timer interval later.
    auto backend2                        = std::make_unique<StubBackend>();
    backend2->selfPresenceFirstCallFails = true;
    backend2->selfPresenceResult =
        SelfPresence{.loaded = true, .active = true, .online = true, .connectionCount = 1};
    backend2->_meId  = UserId{"U1"};
    backend2->_convs = std::vector<Conversation>{kGeneral};
    backend2->_users = std::vector<User>{kAlice};
    auto *stub2      = backend2.get();

    Session session2(std::move(backend2), teamId);
    session2.start();

    CHECK(stub2->selfPresenceCalls == 2);
    CHECK(session2.currentSelfPresence().loaded);
    CHECK(session2.currentSelfPresence().active);
}

TEST_CASE_METHOD(SessionFixture, "default backend yields no self presence snapshot", "[session]") {
    // SessionFixture's stub overrides loadSelfPresence; a fresh Session over a
    // backend without the override must keep the unloaded default.
    struct NoSelfPresenceStub : StubBackend {
        rpl::producer<SelfPresence> loadSelfPresence() override {
            return Backend::loadSelfPresence();
        }
    };
    auto    backend2 = std::make_unique<NoSelfPresenceStub>();
    Session session2(std::move(backend2), teamId);
    session2.start();
    CHECK_FALSE(session2.currentSelfPresence().loaded);
    CHECK_FALSE(session2.currentSelfPresence().phantomAway());
}

// ── cache passthrough ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "cacheMessages/cachedMessages round-trip", "[session][cache]") {
    Message m;
    m.ts     = "100.000";
    m.author = UserId{"U1"};
    m.text   = TextWithEntities{"cached msg", {}};
    ConversationId conv{"C1"};
    session->cacheMessages(conv, {m});
    auto loaded = session->cachedMessages(conv);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].ts == "100.000");
    CHECK(loaded[0].text.text == "cached msg");
}

TEST_CASE_METHOD(SessionFixture, "saveLastConv/loadLastConv round-trip", "[session][cache]") {
    session->saveLastConv(ConversationId{"C1"}, "general");
    auto [conv, name] = session->loadLastConv();
    CHECK(conv == ConversationId{"C1"});
    CHECK(name == "general");
}

TEST_CASE_METHOD(SessionFixture, "cacheImage/cachedImage round-trip", "[session][cache]") {
    const QString    url  = "https://example.com/avatar.png";
    const QByteArray data = "PNG_DATA";
    session->cacheImage(url, data);
    CHECK(session->cachedImage(url) == data);
}

// ── fetchBotIfNeeded ──────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "fetchBotIfNeeded resolves unknown bot via loadBotInfo", "[session][bot]"
) {
    stub->botInfoResult = User{
        UserId{"B001"}, "jenkins", "Jenkins CI", "https://cdn.example.com/jenkins_72.png", true
    };
    session->fetchBotIfNeeded(UserId{"B001"});
    CHECK(stub->botInfoCallCount == 1);
    const auto *u = session->findUser(UserId{"B001"});
    REQUIRE(u != nullptr);
    CHECK(u->displayName == "Jenkins CI");
    CHECK(u->avatarUrl == "https://cdn.example.com/jenkins_72.png");
}

TEST_CASE_METHOD(
    SessionFixture, "fetchBotIfNeeded is no-op for non-B prefixed id", "[session][bot]"
) {
    session->fetchBotIfNeeded(UserId{"U999"});
    CHECK(stub->botInfoCallCount == 0);
    CHECK(session->findUser(UserId{"U999"}) == nullptr);
}

TEST_CASE_METHOD(
    SessionFixture, "fetchBotIfNeeded does not issue duplicate requests", "[session][bot]"
) {
    stub->botInfoResult = User{UserId{"B002"}, "deploy-bot", "Deploy Bot", "", true};
    session->fetchBotIfNeeded(UserId{"B002"});
    session->fetchBotIfNeeded(UserId{"B002"});
    session->fetchBotIfNeeded(UserId{"B002"});
    CHECK(stub->botInfoCallCount == 1);
}

TEST_CASE_METHOD(SessionFixture, "fetchBotIfNeeded skips already-known bot", "[session][bot]") {
    stub->botInfoResult = User{UserId{"B003"}, "bot3", "Bot Three", "", true};
    session->fetchBotIfNeeded(UserId{"B003"});
    REQUIRE(stub->botInfoCallCount == 1);
    // Second call — bot is now in _botUsers, should not trigger another request.
    session->fetchBotIfNeeded(UserId{"B003"});
    CHECK(stub->botInfoCallCount == 1);
}

TEST_CASE_METHOD(
    SessionFixture, "fetchBotIfNeeded fires botInfoLoaded on success", "[session][bot]"
) {
    stub->botInfoResult = User{UserId{"B004"}, "notifier", "Notifier", "", true};
    std::vector<UserId> fired;
    rpl::lifetime       lt;
    session->botInfoLoaded() | rpl::on_next([&fired](UserId id) { fired.push_back(id); }, lt);
    session->fetchBotIfNeeded(UserId{"B004"});
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == UserId{"B004"});
}

TEST_CASE_METHOD(
    SessionFixture,
    "fetchBotIfNeeded does not fire botInfoLoaded when backend returns empty",
    "[session][bot]"
) {
    stub->botInfoResult = User{}; // empty id — simulates 404 / not found
    std::vector<UserId> fired;
    rpl::lifetime       lt;
    session->botInfoLoaded() | rpl::on_next([&fired](UserId id) { fired.push_back(id); }, lt);
    session->fetchBotIfNeeded(UserId{"B005"});
    CHECK(fired.empty());
    CHECK(session->findUser(UserId{"B005"}) == nullptr);
}

TEST_CASE_METHOD(
    SessionFixture,
    "start() restores bots from cache so findUser works immediately",
    "[session][bot]"
) {
    // Pre-populate the cache with a bot from a previous session.
    {
        WorkspaceCache       cache(teamId);
        QHash<QString, User> bots;
        bots["B099"] =
            User{UserId{"B099"}, "ci-bot", "CI Bot", "https://cdn.example.com/ci_72.png", true};
        cache.saveBots(bots);
    }

    // Start a fresh session against the same team — bot should be visible immediately.
    auto backend2    = std::make_unique<StubBackend>();
    backend2->_meId  = UserId{"U1"};
    backend2->_convs = std::vector<Conversation>{kGeneral};
    backend2->_users = std::vector<User>{kAlice};
    Session session2(std::move(backend2), teamId);
    session2.start();

    const auto *u = session2.findUser(UserId{"B099"});
    REQUIRE(u != nullptr);
    CHECK(u->displayName == "CI Bot");
    CHECK(u->avatarUrl == "https://cdn.example.com/ci_72.png");
}

TEST_CASE_METHOD(SessionFixture, "fetchBotIfNeeded persists new bot to cache", "[session][bot]") {
    stub->botInfoResult = User{
        UserId{"B100"}, "release-bot", "Release Bot", "https://cdn.example.com/release_72.png", true
    };
    session->fetchBotIfNeeded(UserId{"B100"});

    // Read the cache directly to verify the bot was written.
    WorkspaceCache cache(teamId);
    const auto     bots = cache.loadBots();
    REQUIRE(bots.contains("B100"));
    CHECK(bots["B100"].displayName == "Release Bot");
    CHECK(bots["B100"].avatarUrl == "https://cdn.example.com/release_72.png");
}

// ── Notification filtering invariants ────────────────────────────────────────
// These cover the bugs fixed in the notification / unread-badge session:
//   • non-member channels must never accumulate local unreads
//   • muted member channels must never accumulate local unreads
//   • an API reload (loadConversations firing again) must not wipe locally-
//     incremented unreads — we keep max(api, local)
//   • EvConvMarked is always authoritative and resets the count
//   • persistUnreads() saves to cache; a fresh session restores them via the
//     same max-merge so a normal restart preserves the badge state

static const Conversation kNonMember{
    .id       = ConversationId{"C_NM"},
    .kind     = ConvKind::PublicChannel,
    .name     = "non-member-chan",
    .isMember = false,
    .lastRead = "0",
    .unread   = 0,
};

static const Conversation kMutedMember{
    .id       = ConversationId{"C_MUTED"},
    .kind     = ConvKind::PublicChannel,
    .name     = "muted-chan",
    .isMember = true,
    .lastRead = "0",
    .unread   = 0,
    .isMuted  = true,
};

// Helper: fire an EvMessageNew from U2 into the given channel.
static void fireIncoming(StubBackend &stub, const QString &convId) {
    Message msg;
    msg.ts     = "500.000";
    msg.author = UserId{"U2"};
    stub.fireEvent(EvMessageNew{ConversationId{convId}, msg});
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew does not increment unread for non-member channel",
    "[session][notif]"
) {
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom, kNonMember};
    fireIncoming(*stub, "C_NM");
    CHECK(session->findConversation(ConversationId{"C_NM"})->unread == 0);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew does not increment unread for muted member channel",
    "[session][notif]"
) {
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom, kMutedMember};
    fireIncoming(*stub, "C_MUTED");
    CHECK(session->findConversation(ConversationId{"C_MUTED"})->unread == 0);
}

TEST_CASE_METHOD(
    SessionFixture, "EvMessageNew increments unread for unmuted member channel", "[session][notif]"
) {
    // Baseline: C2 starts at unread=0 and is a member channel.
    REQUIRE(session->findConversation(ConversationId{"C2"})->unread == 0);
    fireIncoming(*stub, "C2");
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
}

// ── API reload must not wipe locally-incremented unreads ──────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "API reload preserves locally-incremented unread via max merge",
    "[session][notif]"
) {
    // A message arrives before the API responds: local unread becomes 1.
    fireIncoming(*stub, "C2");
    REQUIRE(session->findConversation(ConversationId{"C2"})->unread == 1);

    // Simulate a conversations.list API response returning unread=0 (Slack's
    // list endpoint does not include unread counts).
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom}; // kRandom.unread == 0
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
}

TEST_CASE_METHOD(
    SessionFixture, "API reload uses api unread when it is greater than local", "[session][notif]"
) {
    // API reports 5 unread (e.g. from conversations.info in a future implementation).
    Conversation c1High = kGeneral;
    c1High.unread       = 5;
    stub->_convs        = std::vector<Conversation>{c1High, kRandom};
    CHECK(session->findConversation(ConversationId{"C1"})->unread == 5);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvConvMarked overrides locally-incremented unread with server value",
    "[session][notif]"
) {
    // Message arrives on another device; local counter goes up.
    fireIncoming(*stub, "C2");
    REQUIRE(session->findConversation(ConversationId{"C2"})->unread == 1);

    // User reads on another device; server sends channel_marked with unread=0.
    stub->fireEvent(EvConvMarked{ConversationId{"C2"}, "500.000", 0});
    CHECK(session->findConversation(ConversationId{"C2"})->unread == 0);
}

// ── persistUnreads ────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "persistUnreads saves accumulated unreads; fresh session restores them",
    "[session][notif]"
) {
    // Accumulate an unread that the API would not know about.
    fireIncoming(*stub, "C2");
    REQUIRE(session->findConversation(ConversationId{"C2"})->unread == 1);

    session->persistUnreads();

    // Fresh session: API still returns unread=0, but cache has 1 → max wins.
    auto backend2    = std::make_unique<StubBackend>();
    backend2->_meId  = UserId{"U1"};
    backend2->_convs = std::vector<Conversation>{kGeneral, kRandom}; // unread=0
    backend2->_users = std::vector<User>{kAlice};
    Session session2(std::move(backend2), teamId);
    session2.start();

    CHECK(session2.findConversation(ConversationId{"C2"})->unread == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "persistUnreads after setReading stores zero; fresh session sees zero",
    "[session][notif]"
) {
    fireIncoming(*stub, "C2");
    REQUIRE(session->findConversation(ConversationId{"C2"})->unread == 1);

    // User reads the channel — badge clears.
    session->setReading(ConversationId{"C2"});
    REQUIRE(session->findConversation(ConversationId{"C2"})->unread == 0);

    session->persistUnreads();

    auto backend2    = std::make_unique<StubBackend>();
    backend2->_meId  = UserId{"U1"};
    backend2->_convs = std::vector<Conversation>{kGeneral, kRandom};
    backend2->_users = std::vector<User>{kAlice};
    Session session2(std::move(backend2), teamId);
    session2.start();

    CHECK(session2.findConversation(ConversationId{"C2"})->unread == 0);
}

TEST_CASE_METHOD(
    SessionFixture,
    "EvMessageNew into non-member channel never inflates unread even alongside member channels",
    "[session][notif]"
) {
    // Both a member and a non-member channel receive a message; only the
    // member channel's unread must rise.
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom, kNonMember};

    fireIncoming(*stub, "C2");   // member
    fireIncoming(*stub, "C_NM"); // non-member

    CHECK(session->findConversation(ConversationId{"C2"})->unread == 1);
    CHECK(session->findConversation(ConversationId{"C_NM"})->unread == 0);
}

// ── starConversation ──────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "starConversation optimistically sets isStarred", "[session][star]"
) {
    REQUIRE(!session->findConversation(ConversationId{"C1"})->isStarred);
    session->starConversation(ConversationId{"C1"}, true);
    CHECK(session->findConversation(ConversationId{"C1"})->isStarred == true);
}

TEST_CASE_METHOD(
    SessionFixture, "starConversation optimistically clears isStarred", "[session][star]"
) {
    session->starConversation(ConversationId{"C1"}, true);
    session->starConversation(ConversationId{"C1"}, false);
    CHECK(session->findConversation(ConversationId{"C1"})->isStarred == false);
}

TEST_CASE_METHOD(SessionFixture, "starConversation delegates to backend", "[session][star]") {
    session->starConversation(ConversationId{"C1"}, true);
    REQUIRE(stub->starCalls.size() == 1);
    CHECK(stub->starCalls[0].id == ConversationId{"C1"});
    CHECK(stub->starCalls[0].star == true);

    session->starConversation(ConversationId{"C1"}, false);
    REQUIRE(stub->starCalls.size() == 2);
    CHECK(stub->starCalls[1].star == false);
}

// ── leaveConversation ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "leaveConversation optimistically removes conversation", "[session][leave]"
) {
    REQUIRE(session->findConversation(ConversationId{"C1"}) != nullptr);
    session->leaveConversation(ConversationId{"C1"});
    CHECK(session->findConversation(ConversationId{"C1"}) == nullptr);
}

TEST_CASE_METHOD(
    SessionFixture, "leaveConversation only removes the target conversation", "[session][leave]"
) {
    session->leaveConversation(ConversationId{"C1"});
    CHECK(session->findConversation(ConversationId{"C2"}) != nullptr);
}

TEST_CASE_METHOD(SessionFixture, "leaveConversation delegates to backend", "[session][leave]") {
    session->leaveConversation(ConversationId{"C1"});
    REQUIRE(stub->leaveCalls.size() == 1);
    CHECK(stub->leaveCalls[0] == ConversationId{"C1"});
}

TEST_CASE_METHOD(SessionFixture, "leaveConversation unknown id is a no-op", "[session][leave]") {
    session->leaveConversation(ConversationId{"C_GHOST"});
    CHECK(session->findConversation(ConversationId{"C1"}) != nullptr);
    CHECK(session->findConversation(ConversationId{"C2"}) != nullptr);
}

// ── setNotificationLevel ──────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "setNotificationLevel updates notifLevel", "[session][notif]") {
    session->setNotificationLevel(ConversationId{"C1"}, NotificationLevel::All);
    CHECK(session->findConversation(ConversationId{"C1"})->notifLevel == NotificationLevel::All);
}

TEST_CASE_METHOD(
    SessionFixture, "setNotificationLevel Mute also sets isMuted", "[session][notif]"
) {
    session->setNotificationLevel(ConversationId{"C1"}, NotificationLevel::Mute);
    const auto *c = session->findConversation(ConversationId{"C1"});
    CHECK(c->notifLevel == NotificationLevel::Mute);
    CHECK(c->isMuted == true);
}

TEST_CASE_METHOD(
    SessionFixture, "setNotificationLevel non-Mute clears isMuted", "[session][notif]"
) {
    session->setNotificationLevel(ConversationId{"C1"}, NotificationLevel::Mute);
    REQUIRE(session->findConversation(ConversationId{"C1"})->isMuted == true);
    session->setNotificationLevel(ConversationId{"C1"}, NotificationLevel::Mentions);
    const auto *c = session->findConversation(ConversationId{"C1"});
    CHECK(c->notifLevel == NotificationLevel::Mentions);
    CHECK(c->isMuted == false);
}

// ── API reload merge: isStarred and notifLevel ────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "API reload preserves locally-starred state when API returns false",
    "[session][star][merge]"
) {
    session->starConversation(ConversationId{"C1"}, true);
    REQUIRE(session->findConversation(ConversationId{"C1"})->isStarred == true);

    // API reload returns is_starred=false (field absent / not yet reflected by Slack).
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom};
    CHECK(session->findConversation(ConversationId{"C1"})->isStarred == true);
}

TEST_CASE_METHOD(
    SessionFixture,
    "API reload does not restore isStarred after explicit unstar",
    "[session][star][merge]"
) {
    session->starConversation(ConversationId{"C1"}, true);
    session->starConversation(ConversationId{"C1"}, false);
    REQUIRE(session->findConversation(ConversationId{"C1"})->isStarred == false);

    stub->_convs = std::vector<Conversation>{kGeneral, kRandom};
    CHECK(session->findConversation(ConversationId{"C1"})->isStarred == false);
}

TEST_CASE_METHOD(
    SessionFixture,
    "API reload preserves locally-set notifLevel when API returns Default",
    "[session][notif][merge]"
) {
    session->setNotificationLevel(ConversationId{"C1"}, NotificationLevel::All);
    REQUIRE(session->findConversation(ConversationId{"C1"})->notifLevel == NotificationLevel::All);

    // API reload returns Default (field absent in conversations.list).
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom};
    CHECK(session->findConversation(ConversationId{"C1"})->notifLevel == NotificationLevel::All);
}

TEST_CASE_METHOD(
    SessionFixture,
    "API reload uses API notifLevel when it carries an explicit value",
    "[session][notif][merge]"
) {
    session->setNotificationLevel(ConversationId{"C1"}, NotificationLevel::All);

    Conversation c1Mentions = kGeneral;
    c1Mentions.notifLevel   = NotificationLevel::Mentions;
    stub->_convs            = std::vector<Conversation>{c1Mentions, kRandom};
    CHECK(
        session->findConversation(ConversationId{"C1"})->notifLevel == NotificationLevel::Mentions
    );
}

// ── createChannel ─────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "createChannel delegates name and flag to backend", "[session][createChannel]"
) {
    stub->createChannelResultId = ConversationId{"C_NEW"};
    session->createChannel("new-channel", false);
    REQUIRE(stub->createChannelCalls.size() == 1);
    CHECK(stub->createChannelCalls[0].name == "new-channel");
    CHECK(stub->createChannelCalls[0].isPrivate == false);
}

TEST_CASE_METHOD(
    SessionFixture, "createChannel private flag forwarded to backend", "[session][createChannel]"
) {
    stub->createChannelResultId = ConversationId{"C_SEC"};
    session->createChannel("secret", true);
    REQUIRE(stub->createChannelCalls.size() == 1);
    CHECK(stub->createChannelCalls[0].isPrivate == true);
}

TEST_CASE_METHOD(
    SessionFixture,
    "createChannel success fires onSuccess with returned id",
    "[session][createChannel]"
) {
    stub->createChannelResultId = ConversationId{"C_NEW"};
    ConversationId received;
    session->createChannel("new-channel", false, [&](ConversationId id) { received = id; });
    CHECK(received == ConversationId{"C_NEW"});
}

TEST_CASE_METHOD(
    SessionFixture, "createChannel success refreshes conversation list", "[session][createChannel]"
) {
    const Conversation newChan{
        .id       = ConversationId{"C_NEW"},
        .kind     = ConvKind::PublicChannel,
        .name     = "new-channel",
        .isMember = true,
        .lastRead = "0",
        .unread   = 0,
    };
    stub->createChannelResultId = newChan.id;
    stub->_convs                = std::vector<Conversation>{kGeneral, kRandom, newChan};
    session->createChannel("new-channel", false);
    CHECK(session->findConversation(ConversationId{"C_NEW"}) != nullptr);
}

TEST_CASE_METHOD(
    SessionFixture,
    "createChannel success does not remove existing conversations",
    "[session][createChannel]"
) {
    stub->createChannelResultId = ConversationId{"C_NEW"};
    session->createChannel("new-channel", false);
    CHECK(session->findConversation(ConversationId{"C1"}) != nullptr);
    CHECK(session->findConversation(ConversationId{"C2"}) != nullptr);
}

TEST_CASE_METHOD(
    SessionFixture, "createChannel error fires onError callback", "[session][createChannel]"
) {
    stub->createChannelShouldFail = true;
    stub->createChannelError      = "name_taken";
    QString gotErr;
    session->createChannel("taken", false, {}, [&](const QString &err) { gotErr = err; });
    CHECK(gotErr == "name_taken");
}

TEST_CASE_METHOD(
    SessionFixture,
    "createChannel error without callback fires error hub",
    "[session][createChannel]"
) {
    stub->createChannelShouldFail = true;
    stub->createChannelError      = "invalid_name";
    QString       hubErr;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { hubErr = e; }, lt);
    session->createChannel("bad name", false);
    CHECK(hubErr == "invalid_name");
}

// ── joinChannel ────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "joinChannel delegates id to backend", "[session][joinChannel]") {
    stub->joinChannelResultId = ConversationId{"C_PUB"};
    session->joinChannel(ConversationId{"C_PUB"});
    REQUIRE(stub->joinChannelCalls.size() == 1);
    CHECK(stub->joinChannelCalls[0].id == ConversationId{"C_PUB"});
}

TEST_CASE_METHOD(
    SessionFixture, "joinChannel success fires onSuccess with returned id", "[session][joinChannel]"
) {
    stub->joinChannelResultId = ConversationId{"C_PUB"};
    ConversationId received;
    session->joinChannel(ConversationId{"C_PUB"}, [&](ConversationId id) { received = id; });
    CHECK(received == ConversationId{"C_PUB"});
}

TEST_CASE_METHOD(
    SessionFixture, "joinChannel success refreshes conversation list", "[session][joinChannel]"
) {
    const Conversation newChan{
        .id       = ConversationId{"C_PUB"},
        .kind     = ConvKind::PublicChannel,
        .name     = "public-chan",
        .isMember = true,
        .lastRead = "0",
        .unread   = 0,
    };
    stub->joinChannelResultId = newChan.id;
    stub->_convs              = std::vector<Conversation>{kGeneral, kRandom, newChan};
    session->joinChannel(ConversationId{"C_PUB"});
    CHECK(session->findConversation(ConversationId{"C_PUB"}) != nullptr);
}

TEST_CASE_METHOD(
    SessionFixture,
    "joinChannel success does not remove existing conversations",
    "[session][joinChannel]"
) {
    stub->joinChannelResultId = ConversationId{"C_PUB"};
    session->joinChannel(ConversationId{"C_PUB"});
    CHECK(session->findConversation(ConversationId{"C1"}) != nullptr);
    CHECK(session->findConversation(ConversationId{"C2"}) != nullptr);
}

TEST_CASE_METHOD(
    SessionFixture, "joinChannel error fires onError callback", "[session][joinChannel]"
) {
    stub->joinChannelShouldFail = true;
    stub->joinChannelError      = "already_in_channel";
    QString gotErr;
    session->joinChannel(ConversationId{"C1"}, {}, [&](const QString &err) { gotErr = err; });
    CHECK(gotErr == "already_in_channel");
}

TEST_CASE_METHOD(
    SessionFixture, "joinChannel error without callback fires error hub", "[session][joinChannel]"
) {
    stub->joinChannelShouldFail = true;
    stub->joinChannelError      = "channel_not_found";
    QString       hubErr;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { hubErr = e; }, lt);
    session->joinChannel(ConversationId{"C_GHOST"});
    CHECK(hubErr == "channel_not_found");
}

// ── DM activity sweep (conversations.info enrichment) ─────────────────────────
// conversations.list no longer returns last_read/latest, so Session fetches
// them for IMs/MPDMs via loadConversationInfo after each conversations merge.
// Invariants:
//   • only IMs and MPDMs are fetched — channels get stamps through normal use
//   • only the activity cursors merge; unread from the info response is ignored
//   • cursors survive an API reload (max-merge) and persist to cache
//   • the sweep is throttled across loadConversations() re-fires

static const Conversation kDmBob{
    .id       = ConversationId{"D1"},
    .kind     = ConvKind::Im,
    .name     = "U2",
    .isMember = true,
    .dmUser   = UserId{"U2"},
};

static const Conversation kMpdm{
    .id       = ConversationId{"G1"},
    .kind     = ConvKind::Mpim,
    .name     = "mpdm-alice--bob-1",
    .isMember = true,
};

TEST_CASE_METHOD(
    SessionFixture,
    "activity sweep fetches conversations.info for DMs and MPDMs only",
    "[session][sweep]"
) {
    stub->_convs = std::vector<Conversation>{kGeneral, kRandom, kDmBob, kMpdm};
    CHECK(stub->infoRequested.contains("D1"));
    CHECK(stub->infoRequested.contains("G1"));
    CHECK_FALSE(stub->infoRequested.contains("C1"));
    CHECK_FALSE(stub->infoRequested.contains("C2"));
}

TEST_CASE_METHOD(
    SessionFixture,
    "activity sweep merges last_read/latest cursors but not unread",
    "[session][sweep]"
) {
    Conversation info = kMpdm;
    info.lastRead     = "1700000000.000100";
    info.latestTs     = "1700000005.000200";
    info.unread       = 7; // mapper fallback may fabricate this — must be ignored
    stub->infoResults.insert("G1", info);

    stub->_convs = std::vector<Conversation>{kGeneral, kMpdm};

    const auto *c = session->findConversation(ConversationId{"G1"});
    REQUIRE(c != nullptr);
    CHECK(c->lastRead == "1700000000.000100");
    CHECK(c->latestTs == "1700000005.000200");
    CHECK(c->unread == 0);
}

TEST_CASE_METHOD(
    SessionFixture, "API reload preserves enriched activity cursors", "[session][sweep]"
) {
    Conversation info = kDmBob;
    info.lastRead     = "1700000000.000100";
    info.latestTs     = "1700000005.000200";
    stub->infoResults.insert("D1", info);
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob};

    // Second list response: cursors absent again (Slack dropped these fields).
    // The sweep is now throttled, so only the max-merge can preserve them.
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob};

    const auto *c = session->findConversation(ConversationId{"D1"});
    REQUIRE(c != nullptr);
    CHECK(c->lastRead == "1700000000.000100");
    CHECK(c->latestTs == "1700000005.000200");
}

TEST_CASE_METHOD(SessionFixture, "activity sweep is throttled across reloads", "[session][sweep]") {
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob};
    REQUIRE(stub->infoRequested.count("D1") == 1);

    // The completed sweep stamped the cache; a reload must not re-fetch.
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob, kMpdm};
    CHECK(stub->infoRequested.count("D1") == 1);
    CHECK(stub->infoRequested.count("G1") == 0);
}

TEST_CASE_METHOD(SessionFixture, "activity sweep analyzes DMs before MPDMs", "[session][sweep]") {
    stub->_convs = std::vector<Conversation>{kMpdm, kGeneral, kDmBob};
    REQUIRE(stub->infoRequested.size() == 2);
    CHECK(stub->infoRequested[0] == "D1");
    CHECK(stub->infoRequested[1] == "G1");
}

// ── openDm ────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "openDm short-circuits to an existing IM conversation", "[session][openDm]"
) {
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob};
    ConversationId got;
    session->openDm(UserId{"U2"}, [&](ConversationId id) { got = id; });
    CHECK(got == ConversationId{"D1"});
    CHECK(stub->openDmCalls.empty());
}

TEST_CASE_METHOD(
    SessionFixture, "openDm creates and inserts a new IM conversation", "[session][openDm]"
) {
    stub->openDmResultId = ConversationId{"D_NEW"};
    ConversationId got;
    session->openDm(UserId{"U9"}, [&](ConversationId id) { got = id; });

    REQUIRE(stub->openDmCalls.size() == 1);
    CHECK(stub->openDmCalls[0] == UserId{"U9"});
    CHECK(got == ConversationId{"D_NEW"});

    const auto *c = session->findConversation(ConversationId{"D_NEW"});
    REQUIRE(c != nullptr);
    CHECK(c->kind == ConvKind::Im);
    CHECK(c->isMember);
    REQUIRE(c->dmUser.has_value());
    CHECK(*c->dmUser == UserId{"U9"});
}

TEST_CASE_METHOD(SessionFixture, "openDm error fires onError callback", "[session][openDm]") {
    stub->openDmShouldFail = true;
    stub->openDmError      = "user_not_found";
    QString gotErr;
    session->openDm(UserId{"U_GHOST"}, {}, [&](const QString &err) { gotErr = err; });
    CHECK(gotErr == "user_not_found");
    CHECK(session->findConversation(ConversationId{"D_NEW"}) == nullptr);
}

// ── Slash commands ────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture, "built-in slash commands are available after start", "[session][commands]"
) {
    // The stub's listCommands completes empty (unsupported), so the built-in
    // fallback set must serve the composer.
    REQUIRE(session->findCommand("shrug") != nullptr);
    REQUIRE(session->findCommand("status") != nullptr);
    CHECK(session->findCommand("SHRUG") != nullptr); // case-insensitive
    CHECK(session->findCommand("definitely-not-a-command") == nullptr);
    // Built-ins are limited to natively-executable commands: /remind would
    // need chat.command, which the fallback path can't call.
    CHECK(session->findCommand("remind") == nullptr);
}

TEST_CASE("a backend with no native commands surfaces none (e.g. Teams)", "[session][commands]") {
    // A backend that declares no native commands (and no commands.list) shows no
    // slash commands at all — the Slack built-ins (/shrug, /away, …) are Slack's
    // OWN nativeCommands and must never leak into another service's composer. The
    // app-level set is empty until AI commands are added.
    struct NoNativeStub : StubBackend {
        std::vector<SlashCommand>                nativeCommands() const override { return {}; }
        rpl::producer<std::vector<SlashCommand>> listCommands() override {
            return [](auto c) {
                c.put_done();
                return rpl::lifetime();
            };
        }
    };
    const QString teamId = "T_NO_CMDS";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    Session session(std::make_unique<NoNativeStub>(), teamId);
    session.start();

    CHECK(session.findCommand("shrug") == nullptr); // Slack-native — not in Teams
    CHECK(session.findCommand("away") == nullptr);
    CHECK(session.currentCommands().empty());
}

TEST_CASE(
    "commands.list result replaces built-ins but keeps unlisted ones", "[session][commands]"
) {
    const QString teamId = "T_SESSION_CMDS";
    const QString baseDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId;
    QDir(baseDir).removeRecursively();

    auto  backend        = std::make_unique<StubBackend>();
    auto *stub           = backend.get();
    stub->commandsResult = {
        {"deploy", "Deploy a service", "[service]", "A012"},
        {"remind", "Set a reminder (server copy)", "", ""},
    };

    Session session(std::move(backend), teamId);
    session.start();

    const auto *deploy = session.findCommand("deploy");
    REQUIRE(deploy != nullptr);
    CHECK(deploy->appId == "A012");
    // Server copy wins over the built-in duplicate…
    const auto *remind = session.findCommand("remind");
    REQUIRE(remind != nullptr);
    CHECK(remind->desc == "Set a reminder (server copy)");
    // …and built-ins the server didn't mention survive the merge.
    CHECK(session.findCommand("shrug") != nullptr);

    QDir(baseDir).removeRecursively();
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /shrug sends the kaomoji as a message", "[session][commands]"
) {
    session->runCommand(ConversationId{"C1"}, "shrug", "oh well");
    REQUIRE(stub->sentMessages.size() == 1);
    CHECK(stub->sentMessages[0].conv == ConversationId{"C1"});
    CHECK(stub->sentMessages[0].msg.rawText == QString("oh well ¯\\_(ツ)_/¯"));
    CHECK(stub->runCommandCalls.empty()); // native — never hits chat.command
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /leave leaves the conversation natively", "[session][commands]"
) {
    session->runCommand(ConversationId{"C1"}, "leave", "");
    REQUIRE(stub->leaveCalls.size() == 1);
    CHECK(stub->leaveCalls[0] == ConversationId{"C1"});
    CHECK(stub->runCommandCalls.empty());
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /msg opens a DM and sends the text", "[session][commands]"
) {
    stub->openDmResultId = ConversationId{"D_BOB"};
    session->runCommand(ConversationId{"C1"}, "msg", "@bob hello there");
    REQUIRE(stub->openDmCalls.size() == 1);
    CHECK(stub->openDmCalls[0] == UserId{"U2"}); // resolved by username
    REQUIRE(stub->sentMessages.size() == 1);
    CHECK(stub->sentMessages[0].conv == ConversationId{"D_BOB"});
    CHECK(stub->sentMessages[0].msg.rawText == "hello there");
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /msg with unknown user reports an error", "[session][commands]"
) {
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    session->runCommand(ConversationId{"C1"}, "msg", "@nosuchuser hi");
    CHECK(!err.isEmpty());
    CHECK(stub->openDmCalls.empty());
    CHECK(stub->sentMessages.empty());
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand delegates unknown commands to the backend", "[session][commands]"
) {
    session->runCommand(ConversationId{"C1"}, "remind", "me to stretch in 1 hour");
    REQUIRE(stub->runCommandCalls.size() == 1);
    CHECK(stub->runCommandCalls[0].conv == ConversationId{"C1"});
    CHECK(stub->runCommandCalls[0].command == "/remind"); // leading slash added
    CHECK(stub->runCommandCalls[0].text == "me to stretch in 1 hour");
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand backend failure surfaces on the error hub", "[session][commands]"
) {
    stub->runCommandShouldFail = true;
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    session->runCommand(ConversationId{"C1"}, "remind", "me");
    CHECK(err.contains("remind"));
    CHECK(err.contains("missing_scope"));
}

// ── Self presence / status ────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "setPresence forwards to backend", "[session][presence]") {
    session->setPresence(true);
    REQUIRE(stub->presenceCalls == std::vector<bool>{true});
    session->setPresence(false);
    REQUIRE(stub->presenceCalls == (std::vector<bool>{true, false}));
}

TEST_CASE_METHOD(
    SessionFixture, "setPresence success patches own user and fires event", "[session][presence]"
) {
    auto collector = collectEvents();
    session->setPresence(true); // me = U1 (Alice)
    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK_FALSE(me->isActive);
    REQUIRE(collector.events.size() == 1);
    const auto &ev = std::get<EvPresenceChanged>(collector.events[0]);
    CHECK(ev.user == UserId{"U1"});
    CHECK_FALSE(ev.active);
}

TEST_CASE_METHOD(
    SessionFixture, "setPresence failure reports with re-auth hint", "[session][presence]"
) {
    stub->selfActionError = "missing_scope";
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    session->setPresence(true);
    CHECK(err.contains("missing_scope"));
    CHECK(err.contains("sign in"));
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /away toggles based on manualAway", "[session][commands]"
) {
    // Not manually away → /away sets away.
    session->runCommand(ConversationId{"C1"}, "away", "");
    REQUIRE(stub->presenceCalls == std::vector<bool>{true});

    // Manually away → /away returns to auto.
    stub->selfPresenceResult.manualAway = true;
    session->refreshSelfPresence();
    session->runCommand(ConversationId{"C1"}, "away", "");
    REQUIRE(stub->presenceCalls == (std::vector<bool>{true, false}));
}

TEST_CASE_METHOD(SessionFixture, "runCommand /active sets auto presence", "[session][commands]") {
    session->runCommand(ConversationId{"C1"}, "active", "");
    REQUIRE(stub->presenceCalls == std::vector<bool>{false});
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /status parses emoji, text, and clear", "[session][commands]"
) {
    session->runCommand(ConversationId{"C1"}, "status", ":palm_tree: On vacation");
    REQUIRE(stub->statusCalls.size() == 1);
    CHECK(stub->statusCalls[0].emoji == ":palm_tree:");
    CHECK(stub->statusCalls[0].text == "On vacation");

    session->runCommand(ConversationId{"C1"}, "status", "just text");
    REQUIRE(stub->statusCalls.size() == 2);
    CHECK(stub->statusCalls[1].emoji.isEmpty());
    CHECK(stub->statusCalls[1].text == "just text");

    session->runCommand(ConversationId{"C1"}, "status", "clear");
    REQUIRE(stub->statusCalls.size() == 3);
    CHECK(stub->statusCalls[2].emoji.isEmpty());
    CHECK(stub->statusCalls[2].text.isEmpty());

    // Success patches our own user entry (bare emoji name, no colons).
    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK(me->statusEmoji.isEmpty());
    CHECK(me->statusText.isEmpty());
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /status success patches own status", "[session][commands]"
) {
    session->runCommand(ConversationId{"C1"}, "status", ":coffee: brb");
    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK(me->statusEmoji == "coffee");
    CHECK(me->statusText == "brb");
}

TEST_CASE_METHOD(
    SessionFixture, "setStatus forwards emoji, text and expiration", "[session][status]"
) {
    session->setStatus(":palm_tree:", "On vacation", 1750000000);
    REQUIRE(stub->statusCalls.size() == 1);
    CHECK(stub->statusCalls[0].emoji == ":palm_tree:");
    CHECK(stub->statusCalls[0].text == "On vacation");
    CHECK(stub->statusCalls[0].expirationTs == 1750000000);

    // Success patches our own entry with the bare emoji name (colons stripped).
    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK(me->statusEmoji == "palm_tree");
    CHECK(me->statusText == "On vacation");
}

TEST_CASE_METHOD(SessionFixture, "setStatus defaults expiration to zero", "[session][status]") {
    session->setStatus(":coffee:", "brb");
    REQUIRE(stub->statusCalls.size() == 1);
    CHECK(stub->statusCalls[0].expirationTs == 0);
}

TEST_CASE_METHOD(SessionFixture, "setStatus clears status with empty args", "[session][status]") {
    // Seed a status, then clear it.
    session->setStatus(":coffee:", "brb", 0);
    session->setStatus({}, {}, 0);
    REQUIRE(stub->statusCalls.size() == 2);
    CHECK(stub->statusCalls[1].emoji.isEmpty());
    CHECK(stub->statusCalls[1].text.isEmpty());
    CHECK(stub->statusCalls[1].expirationTs == 0);

    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK(me->statusEmoji.isEmpty());
    CHECK(me->statusText.isEmpty());
}

TEST_CASE_METHOD(
    SessionFixture,
    "setStatus failure surfaces error and leaves entry unchanged",
    "[session][status]"
) {
    stub->selfActionError = "missing_scope";
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);

    session->setStatus(":coffee:", "brb", 1750000000);
    REQUIRE(stub->statusCalls.size() == 1);
    CHECK(err.contains("status"));

    // The optimistic own-entry patch must NOT happen on failure.
    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK(me->statusEmoji.isEmpty());
    CHECK(me->statusText.isEmpty());
}

TEST_CASE_METHOD(SessionFixture, "runCommand /dnd parses durations", "[session][commands]") {
    session->runCommand(ConversationId{"C1"}, "dnd", "30");
    session->runCommand(ConversationId{"C1"}, "dnd", "45m");
    session->runCommand(ConversationId{"C1"}, "dnd", "2h");
    session->runCommand(ConversationId{"C1"}, "dnd", "1h 30m");
    session->runCommand(ConversationId{"C1"}, "dnd", "1 hour");
    session->runCommand(ConversationId{"C1"}, "dnd", "off");
    REQUIRE(stub->dndCalls == (std::vector<int>{30, 45, 120, 90, 60, 0}));
}

TEST_CASE_METHOD(
    SessionFixture, "runCommand /dnd rejects unparseable durations", "[session][commands]"
) {
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    session->runCommand(ConversationId{"C1"}, "dnd", "until tomorrow");
    CHECK(stub->dndCalls.empty());
    CHECK(err.contains("/dnd"));
}

TEST_CASE_METHOD(
    SessionFixture, "setDndSnooze success patches own dnd flag and fires event", "[session][dnd]"
) {
    auto collector = collectEvents();
    session->setDndSnooze(30);
    const User *me = session->findUser(UserId{"U1"});
    REQUIRE(me != nullptr);
    CHECK(me->dndEnabled);
    REQUIRE(collector.events.size() == 1);
    const auto &ev = std::get<EvDndChanged>(collector.events[0]);
    CHECK(ev.user == UserId{"U1"});
    CHECK(ev.dndEnabled);
}

// ── Canvases ──────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(SessionFixture, "loadChannelCanvas reports no canvas", "[session][canvas]") {
    QString fileId = "sentinel";
    session->loadChannelCanvas(ConversationId{"C1"}, [&](QString id, bool) { fileId = id; });
    CHECK(fileId.isEmpty());
}

TEST_CASE_METHOD(
    SessionFixture, "create then load round-trips canvas content", "[session][canvas]"
) {
    QString fileId;
    session->createChannelCanvas(ConversationId{"C1"}, "hello canvas", [&](QString id) {
        fileId = id;
    });
    REQUIRE(!fileId.isEmpty());

    QString viaLookup;
    session->loadChannelCanvas(ConversationId{"C1"}, [&](QString id, bool) { viaLookup = id; });
    CHECK(viaLookup == fileId);

    QString html;
    session->loadCanvasContent(fileId, [&](QString h) { html = h; });
    CHECK(html.contains("hello canvas"));
}

TEST_CASE_METHOD(SessionFixture, "loadCanvasContent failure fires errors()", "[session][canvas]") {
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    session->loadCanvasContent("F-NOPE", [](QString) {});
    CHECK(err.contains("canvas_not_found"));
}

TEST_CASE_METHOD(SessionFixture, "editCanvas passes changes to backend", "[session][canvas]") {
    session->editCanvas(
        "F1", {{.op = CanvasChange::Op::InsertAtEnd, .sectionId = {}, .markdown = "- item"}}
    );
    REQUIRE(stub->editCanvasCalls.size() == 1);
    CHECK(stub->editCanvasCalls[0].canvasId == "F1");
    REQUIRE(stub->editCanvasCalls[0].changes.size() == 1);
    CHECK(stub->editCanvasCalls[0].changes[0].markdown == "- item");
}

TEST_CASE_METHOD(
    SessionFixture, "editCanvas failure without handler fires errors()", "[session][canvas]"
) {
    stub->editCanvasOk = false;
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    session->editCanvas("F1", {{.op = CanvasChange::Op::InsertAtEnd, .markdown = "x"}});
    CHECK(err.contains("canvas_editing_failed"));
}

TEST_CASE_METHOD(SessionFixture, "loadCanvasMeta passes title and permalink", "[session][canvas]") {
    stub->canvasHtml["F1"]  = "<p>x</p>";
    stub->canvasTitle["F1"] = "Roadmap";
    QString title, permalink;
    auto    state = CanvasMetaState::Gone;
    session->loadCanvasMeta("F1", [&](QString t, QString p, CanvasMetaState s) {
        title     = t;
        permalink = p;
        state     = s;
    });
    CHECK(title == "Roadmap");
    CHECK(permalink.contains("F1"));
    CHECK(state == CanvasMetaState::Ok);
}

TEST_CASE_METHOD(
    SessionFixture, "loadCanvasMeta reports a deleted canvas as gone", "[session][canvas]"
) {
    auto state = CanvasMetaState::Ok;
    session->loadCanvasMeta("F-GONE", [&](QString, QString, CanvasMetaState s) { state = s; });
    CHECK(state == CanvasMetaState::Gone);
}

TEST_CASE_METHOD(SessionFixture, "deleteCanvas reaches the backend", "[session][canvas]") {
    bool ok = false;
    session->deleteCanvas("F1", [&](bool r) { ok = r; });
    CHECK(ok);
    REQUIRE(stub->deleteCanvasCalls.size() == 1);
    CHECK(stub->deleteCanvasCalls[0] == "F1");
}

TEST_CASE_METHOD(SessionFixture, "deleteCanvas failure fires errors()", "[session][canvas]") {
    stub->deleteCanvasOk = false;
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    bool ok = true;
    session->deleteCanvas("F-NOPE", [&](bool r) { ok = r; });
    CHECK(!ok);
    CHECK(err.contains("canvas_not_found"));
}

TEST_CASE_METHOD(
    SessionFixture,
    "createChannelCanvas failure fires errors() even with a handler",
    "[session][canvas]"
) {
    session->createChannelCanvas(ConversationId{"C1"}, "first"); // canvas now exists
    QString       err;
    rpl::lifetime lt;
    session->errors() | rpl::on_next([&](const QString &e) { err = e; }, lt);
    bool handlerCalled = false;
    session->createChannelCanvas(ConversationId{"C1"}, "second", {}, [&](const QString &) {
        handlerCalled = true;
    });
    CHECK(handlerCalled);
    CHECK(err.contains("channel_canvas_already_exists"));
}

// ── Realtime safety-net poll: deletion recovery ─────────────────────────────────

namespace {
Message pollMsg(const QString &ts) {
    Message m;
    m.ts     = ts;
    m.date   = decimalTsToMicros(ts);
    m.author = UserId{"U2"};
    m.text   = TextWithEntities{"hi", {}};
    return m;
}
} // namespace

TEST_CASE_METHOD(
    SessionFixture,
    "realtime safety poll recovers the NEWEST message deleted from another client",
    "[session][events][delete]"
) {
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001");
    const Message        m2 = pollMsg("1000.000002"); // the newest — deleted below

    // The poll bails without a latestTs baseline, so seed one by delivering the
    // two messages first.
    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1});
    stub->fireEvent(EvMessageNew{conv, m2});
    REQUIRE(session->findConversation(conv)->latestTs == m2.ts);

    stub->historyPage = {m1, m2};
    session->runRealtimeHealthCheckForTest(); // first tick → baseline snapshot

    // Delete the NEWEST message elsewhere: the head page loses m2 while m2's ts is
    // still newer than everything that remains. The old upper-bounded window
    // skipped exactly this case.
    stub->historyPage = {m1};
    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest();

    bool sawDelete = false;
    for (const auto &e : events)
        if (auto *d = std::get_if<EvMessageDeleted>(&e); d && d->conv == conv && d->ts == m2.ts)
            sawDelete = true;
    CHECK(sawDelete);
}

TEST_CASE_METHOD(
    SessionFixture,
    "realtime safety poll does not synthesize a deletion when nothing changed",
    "[session][events][delete]"
) {
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001");
    const Message        m2 = pollMsg("1000.000002");

    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1});
    stub->fireEvent(EvMessageNew{conv, m2});

    stub->historyPage = {m1, m2};
    session->runRealtimeHealthCheckForTest(); // baseline

    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest(); // identical page → no deletion

    int deletes = 0;
    for (const auto &e : events)
        if (std::holds_alternative<EvMessageDeleted>(e))
            ++deletes;
    CHECK(deletes == 0);
}

TEST_CASE_METHOD(
    SessionFixture,
    "realtime safety poll keeps a message that merely fell below the head window",
    "[session][events][delete]"
) {
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001"); // oldest; will drop out of the page
    const Message        m2 = pollMsg("1000.000002");
    const Message        m3 = pollMsg("1000.000003"); // newer traffic pushes the window up

    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1});
    stub->fireEvent(EvMessageNew{conv, m2});

    stub->historyPage = {m1, m2};
    session->runRealtimeHealthCheckForTest(); // snapshot {m1, m2}

    // New traffic arrives; the (capped-size) head page now starts at m2 — m1 is
    // simply below the window, NOT deleted. The poll must not report m1 deleted.
    stub->fireEvent(EvMessageNew{conv, m3});
    stub->historyPage = {m2, m3};
    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest();

    bool m1Deleted = false;
    for (const auto &e : events)
        if (auto *d = std::get_if<EvMessageDeleted>(&e); d && d->ts == m1.ts)
            m1Deleted = true;
    CHECK_FALSE(m1Deleted); // m1 fell below the window, it wasn't deleted
}

// ── Rate-limit avoidance: reconnect/reestablish throttling ──────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "repeated realtime reconnects coalesce into one conversation reload",
    "[session][events][ratelimit]"
) {
    // conversations.list is heavily rate-limited; a flapping socket firing
    // EvRealtimeReconnected repeatedly must not trigger a reload each time.
    const int before = stub->loadConversationsCalls;
    stub->fireEvent(EvRealtimeReconnected{});
    stub->fireEvent(EvRealtimeReconnected{}); // immediately again — within the window
    stub->fireEvent(EvRealtimeReconnected{});
    CHECK(stub->loadConversationsCalls == before + 1);
}

// ── Reconnect unread recovery (resyncUnreads) ───────────────────────────────────
// When the shared socket silently stalls it drops EvMessageNew across many
// conversations; the reconnect reload (conversations.list) can't recover those
// unreads (returns 0). On EvRealtimeReconnected, Session re-derives DM/MPDM unread
// badges from conversations.info, which reports the authed user's real unread count.

// The conversations.info replies are merged in one batched _conversations
// reassignment ~300 ms after the first reply (Session::applyPendingUnreadInfos)
// instead of one full conv-list rebuild per DM — spin the loop past the window.
static void waitForUnreadMergeWindow() {
    QEventLoop loop;
    QTimer::singleShot(400, &loop, &QEventLoop::quit);
    loop.exec();
}

TEST_CASE_METHOD(
    SessionFixture,
    "reconnect recovers a DM unread badge the stalled socket dropped",
    "[session][events]"
) {
    // The DM has no local unread (its message was dropped while the socket was
    // silent), but conversations.info reports the server's unread count. The
    // startup activity sweep already fetched D1's cursors and — by design — ignored
    // this unread; only the reconnect resync trusts it.
    Conversation info = kDmBob;
    info.lastRead     = "1700000000.000100";
    info.latestTs     = "1700000009.000200";
    info.unread       = 3; // coworker sent 3 messages the socket never delivered
    stub->infoResults.insert("D1", info);
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob};

    REQUIRE(session->findConversation(ConversationId{"D1"})->unread == 0); // sweep ignored it

    stub->fireEvent(EvRealtimeReconnected{});
    waitForUnreadMergeWindow();

    const auto *c = session->findConversation(ConversationId{"D1"});
    REQUIRE(c != nullptr);
    CHECK(c->unread == 3);       // badge recovered
    CHECK(c->mentionCount == 3); // DM unread is a red-badge mention
    // Channels are not resynced — a missed @mention there needs a history scan.
    CHECK_FALSE(stub->infoRequested.contains("C1"));
}

TEST_CASE_METHOD(
    SessionFixture,
    "reconnect unread resync merges upward and never lowers a local count",
    "[session][events]"
) {
    // We already counted 5 unread locally (realtime delivered some before the
    // stall); a momentarily stale server count must not erase them.
    Conversation info = kDmBob;
    info.lastRead     = "1700000000.000100";
    info.latestTs     = "1700000009.000200";
    info.unread       = 2; // server lags behind what we counted
    stub->infoResults.insert("D1", info);

    Conversation dm = kDmBob;
    dm.unread       = 5;
    stub->_convs    = std::vector<Conversation>{kGeneral, dm};

    stub->fireEvent(EvRealtimeReconnected{});
    waitForUnreadMergeWindow(); // without it the merge hasn't run and the CHECK is vacuous

    CHECK(session->findConversation(ConversationId{"D1"})->unread == 5);
}

TEST_CASE_METHOD(
    SessionFixture,
    "reconnect unread resync is throttled to one DM sweep per window",
    "[session][events][ratelimit]"
) {
    // A flapping socket fires EvRealtimeReconnected repeatedly; a per-DM
    // conversations.info sweep each time would be a rate-limit storm.
    stub->infoResults.insert("D1", kDmBob);
    stub->_convs = std::vector<Conversation>{kGeneral, kDmBob};

    const int before = stub->infoRequested.count("D1"); // startup sweep ran once
    stub->fireEvent(EvRealtimeReconnected{});
    const int afterFirst = stub->infoRequested.count("D1");
    CHECK(afterFirst == before + 1); // resync swept once

    stub->fireEvent(EvRealtimeReconnected{}); // again, within the window
    stub->fireEvent(EvRealtimeReconnected{});
    CHECK(stub->infoRequested.count("D1") == afterFirst); // no further sweeps
}

TEST_CASE_METHOD(
    SessionFixture,
    "safety poll re-establishes the socket at most once per window",
    "[session][events][ratelimit]"
) {
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001");
    const Message        m2 = pollMsg("1000.000002");
    const Message        m3 = pollMsg("1000.000003");

    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1}); // baseline latestTs

    // First tick sees m2 as a message realtime never delivered → re-establish.
    stub->historyPage = {m1, m2};
    session->runRealtimeHealthCheckForTest();
    REQUIRE(stub->reestablishRealtimeCalls == 1);

    // A second miss (m3) in the same window must NOT trigger another reconnect —
    // otherwise a persistently sick socket drives a reconnect → reload storm.
    stub->historyPage = {m1, m2, m3};
    session->runRealtimeHealthCheckForTest();
    CHECK(stub->reestablishRealtimeCalls == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "realtime safety poll still recovers messages while the window is unfocused",
    "[session][events]"
) {
    // Regression: a reply to an open-but-unfocused conversation must still arrive.
    // The window blur fires setReading({}) to stop marking-read, but the chat is
    // still on screen, so the safety poll must keep covering it. Previously the
    // poll gated on _readingConv and went dark on blur — so a message Slack
    // silently stopped routing never showed until the user manually refetched
    // (switched chats and back). The poll now gates on _openConv instead.
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001"); // baseline
    const Message        m2 = pollMsg("1000.000002"); // arrives while unfocused

    session->setReading(conv); // open + focused
    stub->fireEvent(EvMessageNew{conv, m1});
    REQUIRE(session->findConversation(conv)->latestTs == m1.ts);

    session->setReading({}); // window loses focus — must NOT silence the poll

    stub->historyPage = {m1, m2};
    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest();

    bool recovered = false;
    for (const auto &e : events)
        if (auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == conv && n->msg.ts == m2.ts)
            recovered = true;
    CHECK(recovered);
}

TEST_CASE_METHOD(
    SessionFixture,
    "background safety poll recovers a missed message when no conversation is open",
    "[session][events]"
) {
    // Hardening for a background (inactive) workspace: the shared socket is one
    // app-wide connection, so a silent stall hits every workspace at once — but
    // the foreground poll only watches the open chat, so a workspace with nothing
    // open would never notice unless some OTHER workspace's open chat happened to
    // catch it. With no conversation open, the health check polls the
    // most-recently-active conversation instead and recovers the miss.
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001"); // baseline
    const Message        m2 = pollMsg("1000.000002"); // arrived while we weren't looking

    // Seed C1's latestTs baseline, then leave with no conversation open.
    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1});
    session->setOpenConversation({});

    stub->historyPage = {m1, m2};
    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest();

    CHECK(stub->lastHistoryConv == conv); // polled the most-recently-active conv
    bool recovered = false;
    for (const auto &e : events)
        if (auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == conv && n->msg.ts == m2.ts)
            recovered = true;
    CHECK(recovered);
    CHECK(stub->reestablishRealtimeCalls == 1); // a hit heals the shared socket
}

TEST_CASE_METHOD(
    SessionFixture,
    "background safety poll is throttled to one history fetch per window",
    "[session][events][ratelimit]"
) {
    // The background poll runs on a slow cadence so an idle workspace can't hammer
    // conversations.history. Two ticks back-to-back must issue only one fetch.
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001");

    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1});
    session->setOpenConversation({});

    stub->historyPage      = {m1};
    stub->loadHistoryCalls = 0;
    session->runRealtimeHealthCheckForTest();
    REQUIRE(stub->loadHistoryCalls == 1);
    session->runRealtimeHealthCheckForTest(); // within the window — must not refetch
    CHECK(stub->loadHistoryCalls == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "foreground safety poll targets the open conversation, throttled per window",
    "[session][events][ratelimit]"
) {
    // When a conversation IS open, the health check polls THAT conversation — but
    // only once per window. conversations.history is capped at 1 req/min for
    // non-Marketplace apps, so an unthrottled per-tick poll would 429-storm. The
    // same tick may additionally spend the (2-minute) background rotation on ONE
    // other conversation; what must not happen is a second fetch of either while
    // their windows are still open.
    const ConversationId conv{"C2"};
    const Message        m1 = pollMsg("1000.000001");

    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, m1});

    stub->historyPage      = {m1};
    stub->loadHistoryCalls = 0;
    stub->historyConvIds.clear();
    session->runRealtimeHealthCheckForTest(/*resetForegroundGap=*/false);
    CHECK(stub->historyConvIds.count(conv.value) == 1); // the open chat, exactly once
    CHECK(stub->loadHistoryCalls <= 2);                 // + at most one rotation pick
    const int afterFirstTick = stub->loadHistoryCalls;
    session->runRealtimeHealthCheckForTest(/*resetForegroundGap=*/false); // within window
    CHECK(stub->loadHistoryCalls == afterFirstTick);
}

TEST_CASE_METHOD(
    SessionFixture,
    "foreground safety poll hands the whole head page to the UI to fill a buried gap",
    "[session][events]"
) {
    // The socket's round-robin steal can drop a run of messages that a later,
    // normally-delivered message then buries under it. The per-message poll path
    // only replays messages NEWER than the latest ts we already hold, so it can
    // NEVER recover the buried run — this was "messages lost even after hours".
    // The poll must therefore also hand the whole head page to the open
    // conversation (EvHeadRefresh) so the MessageList merges the gap back in.
    const ConversationId conv{"C1"};
    const Message        gap   = pollMsg("1000.000004"); // dropped by realtime
    const Message        newer = pollMsg("1000.000009"); // arrived normally, buries the gap

    session->setReading(conv);
    stub->fireEvent(EvMessageNew{conv, newer}); // latestTs jumps past the gap
    REQUIRE(session->findConversation(conv)->latestTs == newer.ts);

    // The server's head page holds both; `gap` is older than latestTs.
    stub->historyPage = {gap, newer};
    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest();

    // The per-message path can't recover the gap (it's older than latestTs)…
    for (const auto &e : events)
        if (auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == conv)
            CHECK(n->msg.ts != gap.ts);

    // …but the full head page IS delivered so the UI can merge it in.
    const EvHeadRefresh *refresh = nullptr;
    for (const auto &e : events)
        if (auto *r = std::get_if<EvHeadRefresh>(&e); r && r->conv == conv)
            refresh = r;
    REQUIRE(refresh != nullptr);
    bool carriesGap = false;
    for (const auto &m : refresh->messages)
        if (m.ts == gap.ts)
            carriesGap = true;
    CHECK(carriesGap);
}

// ── Activity diff: how a poll-only (session-auth) workspace discovers messages ──
// Without a push transport, an EvMessageNew can only ever come from a poll — so
// what gets polled decides what notifies. These cover the whole-workspace activity
// snapshot that picks the targets, and its fallbacks.

TEST_CASE_METHOD(
    SessionFixture,
    "activity diff polls only the conversation that moved",
    "[session][events][counts]"
) {
    const ConversationId c1{"C1"}, c2{"C2"};
    const Message        m1 = pollMsg("1000.000001");
    const Message        m2 = pollMsg("1000.000002");

    // The FIRST snapshot only primes. It reports where every conversation stands,
    // not what changed, so polling off it would replay every pre-existing unread as
    // a brand-new message — a burst of notifications on every launch.
    stub->countsResult = std::vector<ConvCounts>{
        ConvCounts{.id = c1, .latestTs = m1.ts}, ConvCounts{.id = c2, .latestTs = m1.ts}
    };
    stub->loadHistoryCalls      = 0;
    auto [primeEvents, primeLt] = collectEvents();
    session->pollUnreadCountsForTest();
    CHECK(stub->loadHistoryCalls == 0);
    CHECK(primeEvents.empty());

    // Second snapshot: C1 moved on, C2 is where it was. Only C1 may be fetched.
    stub->countsResult = std::vector<ConvCounts>{
        ConvCounts{.id = c1, .latestTs = m2.ts}, ConvCounts{.id = c2, .latestTs = m1.ts}
    };
    stub->historyPage = {m1, m2};
    auto [events, lt] = collectEvents();
    session->pollUnreadCountsForTest();

    CHECK(stub->loadHistoryCalls == 1);
    CHECK(stub->lastHistoryConv == c1);
    // The diff hands the poll the ts C1 stood at before it moved, so exactly the
    // messages that arrived since are delivered (and can notify).
    bool delivered = false;
    for (const auto &e : events)
        if (auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == c1 && n->msg.ts == m2.ts)
            delivered = true;
    CHECK(delivered);
}

TEST_CASE_METHOD(
    SessionFixture,
    "activity diff detects a rising unread count with no latest ts",
    "[session][events][counts]"
) {
    // A source that reports counts but no cursor (conversations.list) must still
    // trigger a poll — the count rise alone says something arrived.
    const ConversationId c1{"C1"};
    stub->countsResult     = std::vector<ConvCounts>{ConvCounts{.id = c1, .unread = 0}};
    stub->loadHistoryCalls = 0;
    session->pollUnreadCountsForTest(); // prime

    stub->countsResult = std::vector<ConvCounts>{ConvCounts{.id = c1, .unread = 2}};
    session->pollUnreadCountsForTest();
    CHECK(stub->loadHistoryCalls == 1);
    CHECK(stub->lastHistoryConv == c1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "activity diff caps polls per tick and drains the rest on later ticks",
    "[session][events][counts][ratelimit]"
) {
    // Waking from suspend reports many moved conversations at once. One history
    // call each would burst into a 429, so the diff spends a fixed budget per tick
    // — and must leave the leftovers looking "moved" so they are not lost.
    const Message             m1 = pollMsg("1000.000001");
    const Message             m2 = pollMsg("1000.000002");
    std::vector<Conversation> many;
    for (int i = 1; i <= 12; ++i) {
        Conversation c = kGeneral;
        c.id           = ConversationId{QStringLiteral("C%1").arg(i)};
        c.name         = QStringLiteral("chan%1").arg(i);
        many.push_back(c);
    }
    restartSession(many, {kAlice, kBob});
    stub->pushEnabled = false;

    std::vector<ConvCounts> before, after;
    for (const auto &c : many) {
        before.push_back(ConvCounts{.id = c.id, .latestTs = m1.ts});
        after.push_back(ConvCounts{.id = c.id, .latestTs = m2.ts});
    }
    stub->countsResult = before;
    session->pollUnreadCountsForTest(); // prime

    stub->countsResult     = after;
    stub->historyPage      = {m1, m2};
    stub->loadHistoryCalls = 0;
    session->pollUnreadCountsForTest();
    CHECK(stub->loadHistoryCalls == 8); // kMaxDiffPollsPerTick

    // The 4 that missed the budget kept their old snapshot entry, so the next tick
    // still sees them as moved and finishes the job.
    session->pollUnreadCountsForTest();
    CHECK(stub->loadHistoryCalls == 12);
    // …and nothing is re-polled once everything has caught up.
    session->pollUnreadCountsForTest();
    CHECK(stub->loadHistoryCalls == 12);
}

TEST_CASE_METHOD(
    SessionFixture,
    "unread-counts snapshot is only asked for when the backend has no push",
    "[session][events][counts]"
) {
    stub->countsResult = std::vector<ConvCounts>{}; // supported, nothing to report
    stub->countsCalls  = 0;
    session->runRealtimeHealthCheckForTest();
    CHECK(stub->countsCalls == 0); // push transport delivers; a snapshot is waste

    stub->pushEnabled = false;
    session->runRealtimeHealthCheckForTest();
    CHECK(stub->countsCalls == 1);
}

TEST_CASE_METHOD(
    SessionFixture,
    "unread-counts polling degrades gracefully when the backend cannot serve it",
    "[session][events][counts]"
) {
    // client.counts is undocumented: a workspace/token that rejects it must not
    // cost us a request every tick forever. A few empty attempts are tolerated
    // (offline blips), then we stop asking and fall back to the roster diff.
    stub->pushEnabled  = false;
    stub->countsResult = std::nullopt; // completes without a value
    session->pollUnreadCountsForTest();
    session->pollUnreadCountsForTest();
    CHECK_FALSE(session->unreadCountsDisabledForTest()); // still trying
    session->pollUnreadCountsForTest();
    CHECK(session->unreadCountsDisabledForTest());

    stub->countsCalls = 0;
    session->runRealtimeHealthCheckForTest();
    CHECK(stub->countsCalls == 0); // and the tick stops spending the request
}

TEST_CASE_METHOD(
    SessionFixture, "a successful snapshot resets the degrade counter", "[session][events][counts]"
) {
    // Two offline blips either side of a success must not add up to a give-up.
    stub->pushEnabled  = false;
    stub->countsResult = std::nullopt;
    session->pollUnreadCountsForTest();
    session->pollUnreadCountsForTest();
    stub->countsResult = std::vector<ConvCounts>{ConvCounts{.id = ConversationId{"C1"}}};
    session->pollUnreadCountsForTest();
    stub->countsResult = std::nullopt;
    session->pollUnreadCountsForTest();
    session->pollUnreadCountsForTest();
    CHECK_FALSE(session->unreadCountsDisabledForTest());
}

TEST_CASE_METHOD(
    SessionFixture,
    "roster reload diffs the SERVER's counts once the snapshot is unavailable",
    "[session][events][counts]"
) {
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001");
    const Message        m2 = pollMsg("1000.000002");

    stub->pushEnabled  = false;
    stub->countsResult = std::nullopt;
    for (int i = 0; i < 3; ++i)
        session->pollUnreadCountsForTest();
    REQUIRE(session->unreadCountsDisabledForTest());

    // Prime from the server's own numbers. They are NOT our merged numbers: the
    // fixture already counted 2 unread locally, and diffing the merge against
    // itself would poll for messages we had already injected.
    Conversation served = kGeneral;
    served.latestTs     = m1.ts;
    served.unread       = 1;
    stub->_convs        = std::vector<Conversation>{served, kRandom};
    session->reloadConversationsForTest();

    // conversations.list carries no `latest`, so a rising unread_count is the only
    // movement signal this fallback source has. Everything the poll will need is
    // armed BEFORE the roster changes: assigning the stub's rpl::variable re-fires
    // every reload subscription taken so far, so the diff runs on assignment too
    // (a stub artifact — a real backend answers one call at a time).
    stub->historyPage      = {m1, m2};
    stub->loadHistoryCalls = 0;
    auto [events, lt]      = collectEvents();
    served.unread          = 3;
    stub->_convs           = std::vector<Conversation>{served, kRandom};
    session->reloadConversationsForTest();

    // Exactly one fetch however many reload handlers ran: the first to see the
    // rise records the new snapshot, so the rest find nothing moved.
    CHECK(stub->loadHistoryCalls == 1);
    CHECK(stub->lastHistoryConv == conv);
    bool delivered = false;
    for (const auto &e : events)
        if (auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == conv && n->msg.ts == m2.ts)
            delivered = true;
    CHECK(delivered);
}

// ── Poll baseline: a cursor only a poll may move ────────────────────────────────

TEST_CASE_METHOD(
    SessionFixture,
    "background poll now covers a conversation that never had a latest cursor",
    "[session][events]"
) {
    // Regression: the rotation used to require a latestTs. conversations.list never
    // returns `latest`, and the info sweep that backfills it covers DMs only — so
    // on a poll-only workspace, where the poll IS delivery, channels were never
    // polled at all until a message happened to arrive while one was open.
    session->setOpenConversation({});
    stub->historyPage      = {pollMsg("1000.000001")};
    stub->loadHistoryCalls = 0;
    auto [events, lt]      = collectEvents();
    session->runRealtimeHealthCheckForTest();

    CHECK(stub->loadHistoryCalls == 1); // was 0 — no candidate had a cursor
    // That first fetch only primes the baseline: a page of history the user has
    // already lived through must not badge or notify.
    for (const auto &e : events)
        CHECK_FALSE(std::holds_alternative<EvMessageNew>(e));
}

TEST_CASE_METHOD(
    SessionFixture,
    "a cursor bump from a sweep cannot swallow a message nobody has seen",
    "[session][events]"
) {
    const ConversationId conv{"C1"};
    const Message        m1 = pollMsg("1000.000001");
    const Message        m2 = pollMsg("1000.000002");

    // Establish the poll's own baseline at m1 by letting it scan a head page.
    session->setReading(conv);
    stub->historyPage = {m1};
    session->runRealtimeHealthCheckForTest();

    // A badge/activity sweep (conversations.info, or the roster's cursor as modeled
    // here) now reports the conversation sitting at m2 — while nobody has seen m2:
    // those sweeps raise no EvMessageNew. Reading Conversation::latestTs as the poll
    // baseline meant the `ts > baseline` filter skipped m2 forever after: the badge
    // appeared, the notification never did.
    Conversation bumped = kGeneral;
    bumped.latestTs     = m2.ts;
    stub->_convs        = std::vector<Conversation>{bumped, kRandom};
    session->reloadConversationsForTest();
    REQUIRE(session->findConversation(conv)->latestTs == m2.ts);

    stub->historyPage = {m1, m2};
    auto [events, lt] = collectEvents();
    session->runRealtimeHealthCheckForTest();

    bool delivered = false;
    for (const auto &e : events)
        if (auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == conv && n->msg.ts == m2.ts)
            delivered = true;
    CHECK(delivered);
}
