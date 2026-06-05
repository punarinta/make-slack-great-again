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
    virtual rpl::producer<AuthState> authState() const = 0;
    virtual Capabilities capabilities() const = 0;
    virtual void connectRealtime() = 0;
    virtual void disconnectRealtime() = 0;

    // --- Snapshot loads (produce one page then complete) ---
    virtual rpl::producer<UserId>                    loadMe() = 0;
    virtual rpl::producer<std::vector<Conversation>> loadConversations() = 0;
    virtual rpl::producer<std::vector<User>>         loadUsers() = 0;
    // Fetch current presence for one user; emits true=active/false=away then completes.
    virtual rpl::producer<bool>                      loadPresence(UserId) = 0;
    virtual rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString> cursor) = 0;
    virtual rpl::producer<MessagePage> loadThread(ConversationId, Ts root, std::optional<QString> cursor) = 0;

    // --- Commands (fire-and-reconcile; optimistic UI lives in Session) ---
    virtual void sendMessage(ConversationId, OutgoingMessage) = 0;
    virtual void editMessage(ConversationId, Ts, TextWithEntities) = 0;
    virtual void deleteMessage(ConversationId, Ts) = 0;
    virtual void addReaction(ConversationId, Ts, QString emoji) = 0;
    virtual void removeReaction(ConversationId, Ts, QString emoji) = 0;
    virtual void markRead(ConversationId, Ts) = 0;

    // --- Phase 3: search, emoji, files ---
    virtual rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) = 0;
    virtual rpl::producer<QHash<QString,QString>>    loadEmojiList() = 0;
    virtual void uploadFile(ConversationId, const QString &filePath) = 0;
    // Download arbitrary Slack file URL with auth credentials.
    virtual void downloadFile(const QString &url,
                              std::function<void(QByteArray)> onData,
                              std::function<void(QString)>    onError = {}) = 0;

    // --- Unified normalized event firehose ---
    // Both Socket Mode and internal-ws frames are normalized to Event here.
    // The Session and UI never know which transport produced an event.
    virtual rpl::producer<Event> events() const = 0;
};
