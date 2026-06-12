// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/backend.h"
#include "auth/token_store.h"
#include "network/web_api_client.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"

#include <QNetworkAccessManager>
#include <QTimer>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

class SocketModeRealtime;

// Real Slack backend using the public API + Socket Mode.
// xappToken is optional; if empty, connectRealtime() is a no-op.
// appCfg and creds.refreshToken are used for transparent token refresh when
// the workspace has token rotation enabled.
class PublicBackend : public Backend {
public:
    // refreshUrl: override the oauth.v2.access endpoint (empty = use Slack's default; tests
    // only).
    explicit PublicBackend(
        const TokenStore::Credentials &creds,
        const TokenStore::AppConfig   &appCfg,
        const QString                 &xappToken  = {},
        const QString                 &refreshUrl = {}
    );
    ~PublicBackend() override;

    // Use an app-level Socket Mode connection shared between workspace
    // backends (one socket receives all workspaces' events — see
    // SocketModeRealtime). Call before connectRealtime(); non-owning.
    // Without this, connectRealtime() creates a private connection.
    void setSharedRealtime(SocketModeRealtime *realtime);

    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;

    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<SelfPresence>              loadSelfPresence() override;
    rpl::producer<User>                      loadBotInfo(UserId botId) override;
    rpl::producer<Conversation>              loadConversationInfo(ConversationId) override;
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString> cursor) override;
    rpl::producer<MessagePage>
    loadThread(ConversationId, Ts root, std::optional<QString> cursor) override;

    void sendMessage(ConversationId, OutgoingMessage) override;
    void editMessage(ConversationId, Ts, TextWithEntities) override;
    void deleteMessage(ConversationId, Ts) override;
    void addReaction(ConversationId, Ts, QString emoji) override;
    void removeReaction(ConversationId, Ts, QString emoji) override;
    void markRead(ConversationId, Ts) override;
    void sendTyping(ConversationId) override;
    void scheduleMessage(ConversationId, OutgoingMessage, qint64 postAt) override;

    void pinMessage(ConversationId, Ts) override;
    void unpinMessage(ConversationId, Ts) override;
    void starConversation(ConversationId, bool star) override;
    void leaveConversation(ConversationId) override;
    void createChannel(
        const QString                      &name,
        bool                                isPrivate,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;
    void joinChannel(
        ConversationId                      id,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;
    void openDm(
        UserId                              user,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;

    void setPresence(bool away, std::function<void(bool ok, QString err)> done = {}) override;
    void setStatus(
        const QString                            &emoji,
        const QString                            &text,
        qint64                                    expirationTs = 0,
        std::function<void(bool ok, QString err)> done         = {}
    ) override;
    void setDndSnooze(int minutes, std::function<void(bool ok, QString err)> done = {}) override;

    rpl::producer<std::vector<SlashCommand>> listCommands() override;
    void                                     runCommand(
                                            ConversationId,
                                            const QString                                &command,
                                            const QString                                &text,
                                            std::function<void(bool ok, QString message)> done = {}
                                        ) override;

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) override;
    rpl::producer<QHash<QString, QString>>   loadEmojiList() override;
    void                                     uploadFiles(
                                            ConversationId,
                                            const QStringList                          &filePaths,
                                            const QString                              &initialComment,
                                            std::function<void(bool ok, QString error)> done = {}
                                        ) override;
    void deleteFile(const QString &fileId) override;
    void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    ) override;

    void loadChannelCanvas(ConversationId, std::function<void(QString, bool)> done) override;
    void loadCanvasContent(
        const QString                    &fileId,
        std::function<void(QString html)> onHtml,
        std::function<void(QString)>      onError = {}
    ) override;
    void createChannelCanvas(
        ConversationId,
        const QString                      &markdown,
        std::function<void(QString fileId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;
    void editCanvas(
        const QString                            &canvasId,
        const std::vector<CanvasChange>          &changes,
        std::function<void(bool ok, QString err)> done = {}
    ) override;

private:
    // canvases.edit allows one change per call; sends the queue sequentially.
    void sendNextCanvasChange(
        const QString                            &canvasId,
        std::shared_ptr<std::deque<CanvasChange>> queue,
        std::function<void(bool ok, QString err)> done
    );

public:
    void loadCanvasMeta(
        const QString                                                     &fileId,
        std::function<void(QString title, QString permalink, bool exists)> done
    ) override;
    void deleteCanvas(
        const QString &canvasId, std::function<void(bool ok, QString err)> done = {}
    ) override;

    void subscribePresence(std::vector<UserId> userIds) override;

    rpl::producer<Event> events() const override;

protected:
    // AuthError means Slack definitively rejected the credentials (logout);
    // TransientError (network down, Slack 5xx) keeps the session alive and the
    // periodic check retries.
    enum class RefreshResult { Success, TransientError, AuthError };

    // Overridable in tests to intercept the network call.
    virtual void doRefresh(std::function<void(RefreshResult)> done);
    qint64       _tokenExpiresAt = 0;

private:
    void
    setupTokenRefresh(const TokenStore::Credentials &creds, const TokenStore::AppConfig &appCfg);
    void triggerRefresh(std::function<void(bool)> done);
    void maybeProactiveRefresh();

    QString               _xappToken;
    QString               _teamId;
    QString               _refreshToken;
    QString               _refreshUrl;
    TokenStore::AppConfig _appCfg;
    WebApiClient         *_api;
    WebApiClient         *_historyApi; // dedicated client for loadHistory/loadThread
    WebApiClient         *_infoApi; // low-priority client for background conversations.info sweeps
    SocketModeRealtime   *_realtime              = nullptr; // owned (private connection)
    SocketModeRealtime   *_sharedRealtime        = nullptr; // non-owning (app-level shared)
    QTimer               *_proactiveRefreshTimer = nullptr;

    // Token refresh deduplication
    bool                                   _refreshInProgress = false;
    std::vector<std::function<void(bool)>> _refreshWaiters;

    rpl::variable<AuthState> _authState{AuthState::LoggedIn};
    rpl::event_stream<Event> _events;
};
