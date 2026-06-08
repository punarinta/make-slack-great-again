// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/backend.h"
#include "auth/token_store.h"
#include "network/web_api_client.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"

#include <QNetworkAccessManager>
#include <functional>
#include <vector>

class SocketModeRealtime;

// Real Slack backend using the public API + Socket Mode.
// xappToken is optional; if empty, connectRealtime() is a no-op.
// appCfg and creds.refreshToken are used for transparent token refresh when
// the workspace has token rotation enabled.
class PublicBackend : public Backend {
public:
    explicit PublicBackend(
        const TokenStore::Credentials &creds,
        const TokenStore::AppConfig   &appCfg,
        const QString                 &xappToken = {}
    );
    ~PublicBackend() override;

    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;

    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<User>                      loadBotInfo(UserId botId) override;
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

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) override;
    rpl::producer<QHash<QString, QString>>   loadEmojiList() override;
    void uploadFile(ConversationId, const QString &filePath) override;
    void deleteFile(const QString &fileId) override;
    void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    ) override;

    void subscribePresence(std::vector<UserId> userIds) override;

    rpl::producer<Event> events() const override;

private:
    void
    setupTokenRefresh(const TokenStore::Credentials &creds, const TokenStore::AppConfig &appCfg);
    void doRefresh(const TokenStore::AppConfig &appCfg, std::function<void(bool)> done);

    QString             _xappToken;
    QString             _teamId;
    QString             _refreshToken;
    WebApiClient       *_api;
    WebApiClient       *_historyApi; // dedicated client for loadHistory/loadThread
    SocketModeRealtime *_realtime = nullptr;

    // Token refresh deduplication
    bool                                   _refreshInProgress = false;
    std::vector<std::function<void(bool)>> _refreshWaiters;

    rpl::variable<AuthState> _authState{AuthState::LoggedIn};
    rpl::event_stream<Event> _events;
};
