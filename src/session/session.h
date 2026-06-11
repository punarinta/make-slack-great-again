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
#include <QSet>
#include <QTimer>
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
    // Fires with a human-readable message whenever a network operation fails
    // without a caller-provided error handler. Subscribe in the UI to show errors.
    rpl::producer<QString>                   errors() const;

    // Send a message (optionally as a thread reply) and optimistically insert it.
    void sendMessage(ConversationId conv, const QString &text, std::optional<Ts> threadRoot = {});

    // Edit an existing message.
    void editMessage(ConversationId conv, Ts ts, const QString &newText);

    // Notify server the user is typing. Internally rate-limited (one call per 3 s).
    void sendTyping(ConversationId conv);

    // Schedule a message to be sent at a future Unix timestamp.
    void scheduleMessage(ConversationId conv, const QString &text, qint64 postAt);

    // --- Phase 3 ---
    void uploadFiles(ConversationId conv, const QStringList &filePaths, const QString &text);
    void
    searchMessages(const QString &query, std::function<void(std::vector<SearchResult>)> callback);
    void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    );

    // Custom emoji map: name → URL (custom) or "alias:name" (alias). Empty until loaded.
    const QHash<QString, QString> &emojiMap() const { return _emojiMap; }

    // Called once the current user's ID is known (e.g., from auth.test).
    void   setMe(UserId id) { _meUserId = std::move(id); }
    UserId meUserId() const { return _meUserId; }
    bool   meIsAdmin() const { return _meIsAdmin; }

    // Call when the user opens a conversation — zeroes its unread count and marks it read on Slack.
    void setReading(ConversationId conv);

    // Star / unstar a conversation (optimistic + API call).
    void starConversation(ConversationId conv, bool star);
    // Leave a conversation (optimistic removal + API call).
    void leaveConversation(ConversationId conv);
    // Create a new channel via the Slack API; refreshes the conversation list on success.
    void createChannel(
        const QString                      &name,
        bool                                isPrivate,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    );
    // Join a public channel; refreshes the conversation list on success.
    void joinChannel(
        ConversationId                      id,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    );
    // Update notification level locally (no public API for per-channel prefs).
    void setNotificationLevel(ConversationId conv, NotificationLevel level);

    // Fetch presence for a user from the network and fire EvPresenceChanged.
    void requestPresence(UserId userId);

    // Rich presence of the authed user — how we appear to others and why
    // (see SelfPresence::phantomAway()). Polled periodically; no realtime
    // event exists for your own connection count.
    rpl::producer<SelfPresence> selfPresence() const;
    SelfPresence                currentSelfPresence() const;
    // Re-poll now (e.g. on window activation).
    void                        refreshSelfPresence();

    // Fetch name + avatar for a bot by bot_id if not already cached; no-op if known.
    void fetchBotIfNeeded(UserId botId);

    // Fires the bot_id whenever a bot's info arrives from the network.
    rpl::producer<UserId> botInfoLoaded() const;

    // Flush current unread counts to cache so they survive a restart.
    void persistUnreads();

    // --- Persistent cache ---
    std::vector<Message> cachedMessages(ConversationId conv) const;
    void                 cacheMessages(ConversationId conv, const std::vector<Message> &msgs);
    void                 saveLastConv(ConversationId conv, const QString &displayName);
    std::pair<ConversationId, QString> loadLastConv() const;

    void       cacheImage(const QString &url, const QByteArray &data);
    QByteArray cachedImage(const QString &url) const;

    const User         *findUser(UserId) const;
    const Conversation *findConversation(ConversationId) const;

    // Synchronous snapshot accessors (for autocomplete, etc.)
    const std::vector<User>         &currentUsers() const;
    const std::vector<Conversation> &currentConversations() const;

    Backend *backend() const;

private:
    std::unique_ptr<Backend>        _backend;
    std::unique_ptr<WorkspaceCache> _cache;

    rpl::variable<std::vector<Conversation>> _conversations;
    rpl::variable<std::vector<User>>         _users;
    rpl::variable<SelfPresence>              _selfPresence;
    QTimer                                   _selfPresenceTimer;
    rpl::event_stream<Event>                 _eventHub;
    rpl::event_stream<QString>               _errorHub;

    UserId                  _meUserId;          // set via setMe() once auth.test result is known
    bool                    _meIsAdmin = false; // is_admin || is_owner from auth.test
    ConversationId          _readingConv;       // currently open conversation
    QHash<QString, QString> _emojiMap;
    // One entry per in-flight optimistic message (text send or file upload).
    // withFiles disambiguates which ghost a confirming server message replaces:
    // a slow upload must not be displaced by a quick text sent after it.
    struct PendingSend {
        QString ts; // fake client-side ts of the optimistic copy
        bool    withFiles = false;
    };
    QHash<QString, QList<PendingSend>> _pendingSends; // conv.value → FIFO queue
    QHash<QString, User>               _botUsers;     // bot_id → User; for bots not in users.list
    QSet<QString>             _pendingBotFetches;     // bot_ids with an in-flight bots.info request
    rpl::event_stream<UserId> _botInfoHub;
    rpl::lifetime             _lifetime;
};
