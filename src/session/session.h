// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Slack::Session — in-memory domain store.
// Owns the Backend, populates from it, maintains rpl change hub for UI subscriptions.
// The UI subscribes to Session producers; it never calls Backend directly.
#pragma once

#include "backend/domain.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"
#include "rpl/lifetime.h"

#include <QHash>
#include <QList>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class Backend;
class WorkspaceCache;

class Session {
public:
    explicit Session(std::unique_ptr<Backend> backend, const QString &teamId);
    ~Session();

    // Call once after construction; begins loading data and connecting realtime.
    void start();

    // --- Read interface for UI ---
    rpl::producer<std::vector<Conversation>> conversations() const;
    rpl::producer<std::vector<User>>         users() const;
    rpl::producer<Event>                     events() const;
    rpl::producer<AuthState>                 authState() const;

    // Send a message (optionally as a thread reply) and optimistically insert it.
    void sendMessage(ConversationId conv, const QString &text,
                     std::optional<Ts> threadRoot = {});

    // Edit an existing message.
    void editMessage(ConversationId conv, Ts ts, const QString &newText);

    // Notify server the user is typing. Internally rate-limited (one call per 3 s).
    void sendTyping(ConversationId conv);

    // Schedule a message to be sent at a future Unix timestamp.
    void scheduleMessage(ConversationId conv, const QString &text, qint64 postAt);

    // --- Phase 3 ---
    void uploadFile(ConversationId conv, const QString &filePath);
    void searchMessages(const QString &query,
                        std::function<void(std::vector<SearchResult>)> callback);
    void downloadFile(const QString &url,
                      std::function<void(QByteArray)> onData,
                      std::function<void(QString)>    onError = {});

    // Custom emoji map: name → URL (custom) or "alias:name" (alias). Empty until loaded.
    const QHash<QString,QString>& emojiMap() const { return _emojiMap; }

    // Called once the current user's ID is known (e.g., from auth.test).
    void setMe(UserId id) { _meUserId = std::move(id); }
    UserId meUserId() const { return _meUserId; }

    // Call when the user opens a conversation — zeroes its unread count and marks it read on Slack.
    void setReading(ConversationId conv);

    // Fetch presence for a user from the network and fire EvPresenceChanged.
    void requestPresence(UserId userId);

    // --- Persistent cache ---
    std::vector<Message>               cachedMessages(ConversationId conv) const;
    void                               cacheMessages(ConversationId conv,
                                                     const std::vector<Message> &msgs);
    void                               saveLastConv(ConversationId conv,
                                                    const QString &displayName);
    std::pair<ConversationId, QString> loadLastConv() const;

    void       cacheImage(const QString &url, const QByteArray &data);
    QByteArray cachedImage(const QString &url) const;

    const User*         findUser(UserId) const;
    const Conversation* findConversation(ConversationId) const;

    // Synchronous snapshot accessors (for autocomplete, etc.)
    const std::vector<User>&         currentUsers() const;
    const std::vector<Conversation>& currentConversations() const;

    Backend* backend() const;

private:
    std::unique_ptr<Backend>      _backend;
    std::unique_ptr<WorkspaceCache> _cache;

    rpl::variable<std::vector<Conversation>> _conversations;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _eventHub;

    UserId              _meUserId;    // set via setMe() once auth.test result is known
    ConversationId      _readingConv; // currently open conversation
    QHash<QString,QString> _emojiMap;
    QHash<QString, QList<QString>> _pendingOptimisticTs; // conv.value → queue of fake ts values
    rpl::lifetime       _lifetime;
};
