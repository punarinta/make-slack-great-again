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
    // Rich presence for the authed user (users.getPresence with no user arg).
    // Default no-op for backends that don't support this.
    virtual rpl::producer<SelfPresence>              loadSelfPresence() {
        return [](auto consumer) {
            consumer.put_done();
            return rpl::lifetime();
        };
    }
    // Fetch display name + avatar for a bot by its bot_id (e.g. "B4URAF31U").
    // Default no-op for backends that don't support this.
    virtual rpl::producer<User> loadBotInfo(UserId /*botId*/) {
        return [](auto consumer) {
            consumer.put_done();
            return rpl::lifetime();
        };
    }
    // Fetch a single user (users.info) by id. Resolves DM peers that users.list
    // omits — Slack system accounts (USLACK / USLACKBOT), Slack Connect partners,
    // deactivated users. Default no-op for backends that don't support this.
    virtual rpl::producer<User> loadUser(UserId /*userId*/) {
        return [](auto consumer) {
            consumer.put_done();
            return rpl::lifetime();
        };
    }
    // Authoritative per-conversation state (conversations.info): last_read and
    // latest message ts, which conversations.list no longer returns. Used by the
    // Session's background activity sweep to seed conversation-list relevance.
    // Default no-op for backends that don't support this.
    virtual rpl::producer<Conversation> loadConversationInfo(ConversationId) {
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
    // Create a new channel (conversations.create). No-op on unsupported backends.
    virtual void createChannel(
        const QString & /*name*/,
        bool /*isPrivate*/,
        std::function<void(ConversationId)> /*onSuccess*/ = {},
        std::function<void(QString)> /*onError*/          = {}
    ) {}
    // Join a public channel (conversations.join). No-op on unsupported backends.
    virtual void joinChannel(
        ConversationId /*id*/,
        std::function<void(ConversationId)> /*onSuccess*/ = {},
        std::function<void(QString)> /*onError*/          = {}
    ) {}
    // Open (or resume) a 1:1 DM with a user (conversations.open). No-op on
    // unsupported backends.
    virtual void openDm(
        UserId /*user*/,
        std::function<void(ConversationId)> /*onSuccess*/ = {},
        std::function<void(QString)> /*onError*/          = {}
    ) {}

    // --- Self presence / status (documented public APIs) ---
    // Set the authed user's presence (users.setPresence): away=true forces
    // "away"; away=false returns to automatic presence detection ("auto").
    virtual void setPresence(bool /*away*/, std::function<void(bool ok, QString err)> done = {}) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }
    // Set — or clear, when emoji and text are both empty — the authed user's
    // status (users.profile.set). `emoji` uses the API's ":name:" form;
    // expirationTs is a Unix timestamp, 0 = no expiration.
    virtual void setStatus(
        const QString & /*emoji*/,
        const QString & /*text*/,
        qint64 /*expirationTs*/                        = 0,
        std::function<void(bool ok, QString err)> done = {}
    ) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }
    // Pause notifications for `minutes` (dnd.setSnooze); minutes <= 0 resumes
    // them (dnd.endSnooze).
    virtual void
    setDndSnooze(int /*minutes*/, std::function<void(bool ok, QString err)> done = {}) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }

    // --- Own profile (documented public APIs) ---
    // Load the authed user's editable profile (users.profile.get).
    virtual void loadMyProfile(std::function<void(MyProfile)> done) {
        if (done)
            done({});
    }
    // Update the authed user's profile (users.profile.set). `fields` maps Slack
    // profile keys (display_name, real_name, email, phone, …) to their new
    // values; only the supplied keys are changed. Note: Slack rejects self
    // email changes (admins only on paid teams) — that surfaces as an error.
    virtual void updateProfile(
        const QHash<QString, QString> & /*fields*/,
        std::function<void(bool ok, QString err)> done = {}
    ) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }
    // Upload a new avatar from a local image file (users.setPhoto). On success
    // `newAvatarUrl` is the freshly-served image URL (may be empty if Slack
    // omits it).
    virtual void setPhoto(
        const QString & /*filePath*/,
        std::function<void(bool ok, QString err, QString newAvatarUrl)> done = {}
    ) {
        if (done)
            done(false, QStringLiteral("not_supported"), {});
    }

    // List the slash commands available in the workspace (undocumented
    // commands.list — official-client API). Backends that can't list commands
    // produce nothing; the Session falls back to the built-in command set.
    virtual rpl::producer<std::vector<SlashCommand>> listCommands() {
        return [](auto consumer) {
            consumer.put_done();
            return rpl::lifetime();
        };
    }
    // Execute a slash command in a conversation (undocumented chat.command —
    // official-client API). `command` carries the leading slash ("/remind").
    // done(ok, message): on failure `message` is the error; on success it is
    // the optional inline response some core commands return (e.g. /who).
    virtual void runCommand(
        ConversationId,
        const QString & /*command*/,
        const QString & /*text*/,
        std::function<void(bool ok, QString message)> done = {}
    ) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }

    // --- Phase 3: search, emoji, files ---
    virtual rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) = 0;
    virtual rpl::producer<QHash<QString, QString>>   loadEmojiList()                      = 0;
    // Upload one or more files and share them in the conversation as a single
    // message; initialComment (may be empty) becomes the message text.
    // `done` (optional) fires once the whole batch settles: ok=true when a
    // message was posted, ok=false (with a reason) when nothing was posted.
    virtual void                                     uploadFiles(
                                            ConversationId,
                                            const QStringList                          &filePaths,
                                            const QString                              &initialComment,
                                            std::function<void(bool ok, QString error)> done = {}
                                        ) = 0;
    // Delete a file by its Slack file ID (files.delete). No-op on unsupported backends.
    virtual void deleteFile(const QString & /*fileId*/) {}
    // Download arbitrary Slack file URL with auth credentials.
    virtual void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    ) = 0;

    // --- Canvases ---
    // Look up the conversation's channel canvas (conversations.info
    // "properties.canvas"). done(fileId, isEmpty); empty fileId = no canvas.
    virtual void
    loadChannelCanvas(ConversationId, std::function<void(QString fileId, bool isEmpty)> done) {
        if (done)
            done({}, true);
    }
    // Fetch a canvas's rendered content. Slack has no read API for canvases;
    // the file's url_private returns the document as HTML (verified: a
    // <div class="quip-canvas-content"> whose blocks carry the section ids
    // canvases.edit operates on). onHtml receives that HTML.
    virtual void loadCanvasContent(
        const QString & /*fileId*/,
        std::function<void(QString html)> /*onHtml*/,
        std::function<void(QString)> onError = {}
    ) {
        if (onError)
            onError(QStringLiteral("not_supported"));
    }
    // Create the conversation's channel canvas (conversations.canvases.create)
    // with initial canvas-markdown content (real markdown, not mrkdwn).
    virtual void createChannelCanvas(
        ConversationId,
        const QString & /*markdown*/,
        std::function<void(QString fileId)> /*onSuccess*/ = {},
        std::function<void(QString)> onError              = {}
    ) {
        if (onError)
            onError(QStringLiteral("not_supported"));
    }
    // Apply section-based edits to a canvas (canvases.edit).
    virtual void editCanvas(
        const QString & /*canvasId*/,
        const std::vector<CanvasChange> & /*changes*/,
        std::function<void(bool ok, QString err)> done = {}
    ) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }
    // Canvas display metadata (files.info): title for the tab label, permalink
    // for "Copy link". See CanvasMetaState for how failures map; transient/
    // unknown errors report empty strings with CanvasMetaState::Ok.
    virtual void loadCanvasMeta(
        const QString & /*fileId*/,
        std::function<void(QString title, QString permalink, CanvasMetaState state)> done
    ) {
        if (done)
            done({}, {}, CanvasMetaState::Ok);
    }
    // Permanently delete a canvas (canvases.delete) — Slack offers no undo.
    virtual void deleteCanvas(
        const QString & /*canvasId*/, std::function<void(bool ok, QString err)> done = {}
    ) {
        if (done)
            done(false, QStringLiteral("not_supported"));
    }

    // Subscribe to presence_change events for the given users via Socket Mode.
    // No-op on backends that don't support live presence.
    virtual void subscribePresence(std::vector<UserId> /*userIds*/) {}

    // --- Unified normalized event firehose ---
    // Both Socket Mode and internal-ws frames are normalized to Event here.
    // The Session and UI never know which transport produced an event.
    virtual rpl::producer<Event> events() const = 0;
};
