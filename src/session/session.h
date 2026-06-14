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
#include <QQueue>
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

    // --- Slash commands ---
    // Workspace slash commands: the built-in Slack set, replaced/extended by
    // whatever the backend reports (commands.list) once it answers.
    const std::vector<SlashCommand> &currentCommands() const { return _commands; }
    // Look up a command by name (no leading slash, case-insensitive); nullptr if unknown.
    const SlashCommand              *findCommand(const QString &name) const;
    // Execute "/name args" in a conversation. Commands with documented
    // public-API equivalents run natively; the rest go through
    // Backend::runCommand (undocumented chat.command) and report failures
    // via errors().
    void runCommand(ConversationId conv, const QString &name, const QString &args);

    // --- Self presence / status (documented public APIs) ---
    // Failures fire errors() (with a re-auth hint when the token lacks the
    // scope); successes patch our own entry in users() so the UI updates
    // without waiting for a poll.
    // Force away (true) or return to automatic presence detection (false);
    // also refreshes selfPresence().
    void setPresence(bool away);
    // Set — or clear, when both args are empty — the status. `emoji` uses the
    // API's ":name:" form.
    void setStatus(const QString &emoji, const QString &text);
    // Pause notifications for `minutes`; minutes <= 0 resumes them.
    void setDndSnooze(int minutes);

    // --- Phase 3 ---
    void uploadFiles(ConversationId conv, const QStringList &filePaths, const QString &text);
    void
    searchMessages(const QString &query, std::function<void(std::vector<SearchResult>)> callback);
    void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    );

    // --- Canvases ---
    // Channel canvas lookup: done(fileId, isEmpty); empty fileId = no canvas.
    void loadChannelCanvas(ConversationId conv, std::function<void(QString, bool)> done);
    // Canvas content as HTML (Slack serves canvases as HTML via url_private).
    void loadCanvasContent(
        const QString                    &fileId,
        std::function<void(QString html)> onHtml,
        std::function<void(QString)>      onError = {}
    );
    // Create the conversation's channel canvas from canvas markdown.
    void createChannelCanvas(
        ConversationId                      conv,
        const QString                      &markdown,
        std::function<void(QString fileId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    );
    // Section-based canvas edits; failures always fire errors() (done, if
    // given, is additionally notified either way).
    void editCanvas(
        const QString                            &canvasId,
        const std::vector<CanvasChange>          &changes,
        std::function<void(bool ok, QString err)> done = {}
    );
    // Canvas title (tab label) + permalink ("Copy link"); empty on failure.
    // exists=false when the file is deleted — conversations.info keeps
    // referencing deleted channel canvases, so callers must drop theirs.
    void loadCanvasMeta(
        const QString                                                               &fileId,
        std::function<void(QString title, QString permalink, CanvasMetaState state)> done
    );
    // Permanent canvas deletion; failures fire errors().
    void deleteCanvas(const QString &canvasId, std::function<void(bool ok)> done = {});

    // Custom emoji map: name → URL (custom) or "alias:name" (alias). Empty until loaded.
    const QHash<QString, QString> &emojiMap() const { return _emojiMap; }
    // Fires after the emoji map is replaced by a fresh emoji.list response —
    // anything that rendered :codes: before that must re-resolve them.
    rpl::producer<>                emojiMapLoaded() const { return _emojiMapLoadedHub.events(); }

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
    // Open a 1:1 DM with a user. Short-circuits to the existing IM conversation
    // when one is known; otherwise calls conversations.open and inserts the new
    // conversation so the UI can select it immediately.
    void openDm(
        UserId                              user,
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

    // Fetch a single user (users.info) and merge into the live user list if not
    // already known; no-op if cached or in-flight. Resolves DM peers that
    // users.list omits (Slack system accounts, Slack Connect, deactivated users)
    // so the conversation list can show a real name + avatar instead of the raw id.
    void fetchUserIfNeeded(UserId userId);

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
    // Resolve our own user id via auth.test; persists the result to cache.
    // Called at start() and retried from the loadUsers handler if the first
    // call raced the startup token refresh and failed — without meUserId every
    // optimistic send turns into a permanent duplicate ghost.
    void fetchMe();

    // Background conversations.info sweep over IMs/MPDMs that refreshes their
    // last_read/latest cursors (used for conversation-list relevance). Throttled
    // via the workspace cache; called after each loadConversations() merge.
    void enrichDmActivity();

    // Resolve any DM peer absent from the loaded user list via users.info. No-op
    // until users.list has loaded (so we don't mistake "not loaded yet" for
    // "missing"); called from both the users and conversations load handlers.
    void fetchMissingDmUsers();

    // Apply `fn` to our own entry in _users (no-op while meUserId is unknown);
    // reassigning the variable notifies users() subscribers.
    void patchMeUser(const std::function<void(User &)> &fn);

    // False when this (conv, ts) was already delivered once — a duplicate
    // echo (chat.postMessage response + realtime) or a Socket Mode envelope
    // redelivery. Remembers the last 512 sightings.
    bool firstSighting(const ConversationId &conv, const Ts &ts);

    std::unique_ptr<Backend>        _backend;
    std::unique_ptr<WorkspaceCache> _cache;

    rpl::variable<std::vector<Conversation>> _conversations;
    rpl::variable<std::vector<User>>         _users;
    rpl::variable<SelfPresence>              _selfPresence;
    QTimer                                   _selfPresenceTimer;
    rpl::event_stream<Event>                 _eventHub;
    rpl::event_stream<QString>               _errorHub;
    rpl::event_stream<>                      _emojiMapLoadedHub;

    UserId                    _meUserId;          // set via setMe() once auth.test result is known
    bool                      _meIsAdmin = false; // is_admin || is_owner from auth.test
    ConversationId            _readingConv;       // currently open conversation
    QHash<QString, QString>   _emojiMap;
    std::vector<SlashCommand> _commands; // built-ins + commands.list result
    // One entry per in-flight optimistic message (text send or file upload).
    // withFiles disambiguates which ghost a confirming server message replaces:
    // a slow upload must not be displaced by a quick text sent after it.
    struct PendingSend {
        QString ts; // fake client-side ts of the optimistic copy
        bool    withFiles = false;
    };
    QHash<QString, QList<PendingSend>> _pendingSends; // conv.value → FIFO queue
    QSet<QString>                      _seenMsgKeys;  // "conv|ts" of delivered messages
    QQueue<QString>                    _seenMsgOrder; // FIFO eviction for _seenMsgKeys
    QHash<QString, User>               _botUsers;     // bot_id → User; for bots not in users.list
    QSet<QString>             _pendingBotFetches;     // bot_ids with an in-flight bots.info request
    QSet<QString>             _pendingUserFetches; // user ids with an in-flight users.info request
    rpl::event_stream<UserId> _botInfoHub;
    rpl::lifetime             _lifetime;
};
