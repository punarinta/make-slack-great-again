// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/backend.h"
#include "network/web_api_client.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"

class SocketModeRealtime;

// Real Slack backend using the public API + Socket Mode.
// xappToken is optional; if empty, connectRealtime() is a no-op.
class PublicBackend : public Backend {
public:
    explicit PublicBackend(const QString &xoxpToken, const QString &xappToken = {});
    ~PublicBackend() override;

    rpl::producer<AuthState>               authState() const override;
    Capabilities                           capabilities() const override;
    void                                   connectRealtime() override;
    void                                   disconnectRealtime() override;

    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<MessagePage>               loadHistory(ConversationId, std::optional<QString> cursor) override;
    rpl::producer<MessagePage>               loadThread(ConversationId, Ts root, std::optional<QString> cursor) override;

    void sendMessage(ConversationId, OutgoingMessage) override;
    void editMessage(ConversationId, Ts, TextWithEntities) override;
    void deleteMessage(ConversationId, Ts) override;
    void addReaction(ConversationId, Ts, QString emoji) override;
    void removeReaction(ConversationId, Ts, QString emoji) override;
    void markRead(ConversationId, Ts) override;
    void sendTyping(ConversationId) override;
    void scheduleMessage(ConversationId, OutgoingMessage, qint64 postAt) override;

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) override;
    rpl::producer<QHash<QString,QString>>    loadEmojiList() override;
    void uploadFile(ConversationId, const QString &filePath) override;
    void downloadFile(const QString &url,
                      std::function<void(QByteArray)> onData,
                      std::function<void(QString)>    onError = {}) override;

    rpl::producer<Event> events() const override;

private:
    QString            _xappToken;
    WebApiClient      *_api;
    SocketModeRealtime *_realtime = nullptr;

    rpl::variable<AuthState>  _authState{ AuthState::LoggedIn };
    rpl::event_stream<Event>  _events;
};
