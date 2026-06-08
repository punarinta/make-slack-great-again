// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// The Backend port — the ONE seam between the UI/domain layer and any Slack transport.
// Implementations: FakeBackend (tests), PublicBackend (Phase 1), InternalBackend (Phase 5).
// RULE: nothing above this include may import Slack JSON, HTTP types, or xoxp tokens.
#pragma once

#include "domain.h"
#include "rpl/producer.h"
#include "rpl/event_stream.h"

#include <QHash>
#include <functional>
#include <memory>

class Backend {
public:
    virtual ~Backend() = default;

    // --- Lifecycle ---
    virtual rpl::producer<AuthState> authState() const    = 0;
    virtual Capabilities             capabilities() const = 0;
    virtual void                     connectRealtime()    = 0;
    virtual void                     disconnectRealtime() = 0;

    // --- Snapshot loads (produce one page then complete) ---
    virtual rpl::producer<UserId>                    loadMe()             = 0;
    virtual rpl::producer<std::vector<Conversation>> loadConversations()  = 0;
    virtual rpl::producer<std::vector<User>>         loadUsers()          = 0;
    // Fetch current presence for one user; emits true=active/false=away then completes.
    virtual rpl::producer<bool>                      loadPresence(UserId) = 0;
    // Fetch display name + avatar for a bot by its bot_id (e.g. "B4URAF31U").
    // Default no-op for backends that don't support this.
    virtual rpl::producer<User>                      loadBotInfo(UserId /*botId*/) {
        return [](auto consumer) {
            consumer.put_done();
            return rpl::lifetime();
        };
    }
    virtual rpl::producer<MessagePage>
    loadHistory(ConversationId, std::optional<QString> cursor) = 0;
    virtual rpl::producer<MessagePage>
    loadThread(ConversationId, Ts root, std::optional<QString> cursor) = 0;

    // --- Commands (fire-and-reconcile; optimistic UI lives in Session) ---
    virtual void sendMessage(ConversationId, OutgoingMessage)      = 0;
    virtual void editMessage(ConversationId, Ts, TextWithEntities) = 0;
    virtual void deleteMessage(ConversationId, Ts)                 = 0;
    virtual void addReaction(ConversationId, Ts, QString emoji)    = 0;
    virtual void removeReaction(ConversationId, Ts, QString emoji) = 0;
    virtual void markRead(ConversationId, Ts)                      = 0;
    // Notify the server the current user is typing. No-op if not supported.
    virtual void sendTyping(ConversationId) {}
    // Send a message at a future Unix timestamp (chat.scheduleMessage).
    virtual void scheduleMessage(ConversationId, OutgoingMessage, qint64 postAt) {}

    // Pin / unpin a message in a channel (pins.add / pins.remove).
    virtual void pinMessage(ConversationId, Ts) {}
    virtual void unpinMessage(ConversationId, Ts) {}

    // Star / unstar a conversation (stars.add / stars.remove).
    virtual void starConversation(ConversationId, bool star) {}
    // Leave a conversation (conversations.leave).
    virtual void leaveConversation(ConversationId) {}

    // --- Phase 3: search, emoji, files ---
    virtual rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) = 0;
    virtual rpl::producer<QHash<QString, QString>>   loadEmojiList()                      = 0;
    virtual void uploadFile(ConversationId, const QString &filePath)                      = 0;
    // Download arbitrary Slack file URL with auth credentials.
    virtual void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    ) = 0;

    // Subscribe to presence_change events for the given users via Socket Mode.
    // No-op on backends that don't support live presence.
    virtual void subscribePresence(std::vector<UserId> /*userIds*/) {}

    // --- Unified normalized event firehose ---
    // Both Socket Mode and internal-ws frames are normalized to Event here.
    // The Session and UI never know which transport produced an event.
    virtual rpl::producer<Event> events() const = 0;
};
