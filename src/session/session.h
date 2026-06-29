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
    void sendMessage(
        ConversationId    conv,
        const QString    &text,
        std::optional<Ts> threadRoot = {},
        const QString    &subject    = {}
    );

    // Edit an existing message.
    void editMessage(ConversationId conv, Ts ts, const QString &newText);

    // Email (Model-D): channels are labels, so forwarding a message to a channel
    // labels it rather than re-posting. The UI gates the forward path on this.
    bool channelsAreLabels() const;
    void labelMessage(
        ConversationId                            sourceConv,
        Ts                                        ts,
        ConversationId                            targetChannel,
        std::function<void(bool ok, QString err)> done = {}
    );

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
    // API's ":name:" form. `expirationTs` is an absolute Unix timestamp (seconds)
    // after which Slack auto-clears the status; 0 means "Don't clear".
    void setStatus(const QString &emoji, const QString &text, qint64 expirationTs = 0);
    // Pause notifications for `minutes`; minutes <= 0 resumes them.
    void setDndSnooze(int minutes);

    // --- Own profile ---
    // Load the authed user's editable profile (users.profile.get).
    void loadMyProfile(std::function<void(MyProfile)> done);
    // Update profile fields (users.profile.set); `fields` maps Slack profile
    // keys to new values. On success patches our own user entry so the UI
    // (footer, conv list) updates without a poll; failures fire errors() and
    // also notify done(false, err).
    void updateProfile(
        const QHash<QString, QString> &fields, std::function<void(bool ok, QString err)> done = {}
    );
    // Upload a new avatar from a local image file (users.setPhoto). On success
    // patches our own avatar URL; failures fire errors() and notify done.
    void setPhoto(const QString &filePath, std::function<void(bool ok, QString err)> done = {});

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
    // Resolve and download a canvas-embedded image by its Slack file id (the
    // trailing segment of the relative /collab-slack-blob/ URL in canvas HTML).
    void loadCanvasImage(
        const QString                  &fileId,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
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
    void    setMe(UserId id) { _meUserId = std::move(id); }
    UserId  meUserId() const { return _meUserId; }
    bool    meIsAdmin() const { return _meIsAdmin; }
    // Workspace web base URL (e.g. "https://nisdos.slack.com/"); empty until auth.test answers.
    QString teamUrl() const;

    // Call when the user opens a conversation — zeroes its unread count and marks it read on Slack.
    void setReading(ConversationId conv);

    // Which conversation the UI currently has open, INDEPENDENT of window focus.
    // Drives the realtime safety poll (checkRealtimeHealth): the open conversation
    // must keep being polled for messages Slack silently stopped routing even while
    // the window is in the background — otherwise a reply to an open-but-unfocused
    // chat never arrives until a manual refetch. Distinct from setReading(), which
    // also means "focused, so mark read" and is correctly cleared on blur.
    // Reading a conversation implies it is open, so setReading(conv) with a
    // non-empty id sets this too; pass {} only when no conversation is open at all
    // (workspace switch / logout), NOT on a mere focus change.
    void setOpenConversation(ConversationId conv);

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
    // Toggle the local "mute this person" switch (no backend support; cache-only).
    void setConvMuted(ConversationId conv, bool muted);

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

    // Fires a user id whenever fetchUserIfNeeded resolves a user that users.list
    // omitted (Slack Connect / system / deactivated). Lets the message list
    // re-render that author's header + avatar and any baked-in @mentions.
    rpl::producer<UserId> userInfoLoaded() const;

    // Flush current unread counts to cache so they survive a restart.
    // Synchronous — call on shutdown or when durability is required now.
    void persistUnreads();
    // Debounced variant: coalesces bursts and keeps the (potentially large)
    // conversation-list serialization + file write off latency-sensitive paths
    // such as the workspace switch. A pending save is flushed by persistUnreads()
    // and by the destructor.
    void scheduleSaveUnreads();

    // --- Persistent cache ---
    std::vector<Message> cachedMessages(ConversationId conv) const;
    void                 cacheMessages(ConversationId conv, const std::vector<Message> &msgs);
    void                 saveLastConv(ConversationId conv, const QString &displayName);
    std::pair<ConversationId, QString> loadLastConv() const;

    void       cacheImage(const QString &url, const QByteArray &data);
    QByteArray cachedImage(const QString &url) const;

    const User         *findUser(UserId) const;
    const Conversation *findConversation(ConversationId) const;

    // A human-readable name for a user id that is NEVER the raw id. Returns the
    // cached display label when known; otherwise kicks off a users.info fetch in
    // the background (so a later repaint shows the real name) and returns a
    // neutral placeholder. Use this at every UI surface that shows a user name
    // so external Slack Connect / system / deactivated users — absent from
    // users.list — never surface as a cryptic "U…/W…" id. Non-const: it may
    // trigger a fetch.
    QString userDisplayName(UserId);

    // True for a DM with a bot/app user (the conv-list "Agents & apps" section),
    // including the Slack system accounts that report is_bot=false. Such IMs
    // never own a user-editable channel canvas — any canvas conversations.info
    // advertises for them is app-owned and answers not_visible — so the canvas
    // tab and its probe are skipped. Mirrors ConvListWidget::isAppConv.
    bool isAppConversation(const Conversation &) const;

    // What the active backend supports; the UI gates Slack-only affordances
    // (canvas tab, huddle controls) on these so a future service that lacks them
    // shows a clean surface. See Capabilities.
    Capabilities capabilities() const;

    // ID-shape questions delegated to the backend so the UI/Session never parse
    // an id's prefix. isSyntheticUser → a system/pseudo account (rendered as an
    // app); isUnresolvedUserId → a raw id surfacing where a name is expected.
    bool isSyntheticUser(UserId) const;
    bool isUnresolvedUserId(const QString &) const;

    // Synchronous snapshot accessors (for autocomplete, etc.)
    const std::vector<User>         &currentUsers() const;
    const std::vector<Conversation> &currentConversations() const;

    Backend *backend() const;

    // Test hook: run one realtime safety-net tick synchronously (normally fired
    // by the 15 s _realtimeSafetyTimer). Exposed so the missed-message and
    // deletion-recovery logic is unit-testable without waiting on the timer.
    void runRealtimeHealthCheckForTest() { checkRealtimeHealth(); }

private:
    // Resolve our own user id via auth.test; persists the result to cache.
    // Called at start() and retried from the loadUsers handler if the first
    // call raced the startup token refresh and failed — without meUserId every
    // optimistic send turns into a permanent duplicate ghost.
    void fetchMe();

    // Fetch the conversation list and fold it into _conversations, preserving
    // locally-tracked state the API can't report (unread/mention counts, star/
    // mute/notif prefs, last_read/latest cursors, live huddles). Runs at start()
    // (refreshEmoji=true, which chains the emoji-list load) and again whenever
    // the realtime socket reconnects after a gap (refreshEmoji=false) so unread
    // badges catch up on events Slack didn't replay.
    void reloadConversations(bool refreshEmoji);

    // Fetch a single channel's conversations.info and slot it into
    // _conversations — used when a member_joined_channel event tells us we were
    // added to a channel we weren't already tracking as a member.
    void fetchJoinedConversation(ConversationId id);

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

    // Apply a new message to in-memory state (latest cursor, unread/mention
    // badges, mark-read while reading, optimistic-ghost removal) and report
    // whether it should be forwarded to the UI. Returns false for a duplicate
    // (firstSighting). Shared by the backend event firehose and the periodic
    // safety poll, which injects messages the realtime stream missed.
    bool handleNewMessage(const ConversationId &conv, const Message &msg);

    // Periodic safety net (every 15 s) for the realtime socket: re-verify the
    // subscription and poll for messages the realtime stream silently failed to
    // deliver — the open conversation when one is on screen, otherwise (background
    // workspace) the most-recently-active conversation on a slower cadence. See
    // the implementation for why the socket's own liveness watchdog can't cover
    // this, and why a background workspace needs its own poll.
    void checkRealtimeHealth();

    // Poll one conversation's head history for messages the realtime stream
    // dropped; inject any and reestablish the shared socket on a hit. foreground
    // also runs deletion detection (needs the open chat's snapshot) and ties its
    // staleness check to _openConv; background yields the moment a chat is opened.
    void pollConversationForMissed(ConversationId conv, bool foreground);

    // The member conversation with the newest activity (greatest latestTs) — the
    // likeliest place a reply lands, used as the background workspace's poll target.
    ConversationId mostRecentlyActiveConv() const;

    std::unique_ptr<Backend>        _backend;
    std::unique_ptr<WorkspaceCache> _cache;

    rpl::variable<std::vector<Conversation>> _conversations;
    rpl::variable<std::vector<User>>         _users;
    rpl::variable<SelfPresence>              _selfPresence;
    QTimer                                   _selfPresenceTimer;
    QTimer                                   _realtimeSafetyTimer; // 15 s; checkRealtimeHealth()
    QTimer                                   _saveUnreadsTimer; // debounces scheduleSaveUnreads()
    rpl::event_stream<Event>                 _eventHub;
    rpl::event_stream<QString>               _errorHub;
    rpl::event_stream<>                      _emojiMapLoadedHub;

    UserId                    _meUserId;          // set via setMe() once auth.test result is known
    bool                      _meIsAdmin = false; // is_admin || is_owner from auth.test
    ConversationId            _readingConv;       // open AND focused (drives mark-read)
    ConversationId            _openConv;          // open in the UI regardless of focus
    // Authoritative ts → threadRoot ("" if none) from the previous safety poll
    // of the open conversation. checkRealtimeHealth diffs the next poll against
    // it to catch a message deleted from another client that the realtime stream
    // never delivered. Reset whenever the open conversation changes
    // (setOpenConversation).
    ConversationId            _pollSnapshotConv;
    QHash<QString, QString>   _pollSnapshotTs;
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

    // Throttle the rate-limit notice banner (429s cluster; don't spam).
    static constexpr qint64 kRateLimitNoticeGapMs  = 15'000;
    qint64                  _lastRateLimitNoticeMs = 0;

    // Throttle reconnect-driven full conversation reloads. conversations.list is
    // heavily rate-limited, and a flapping socket can fire EvRealtimeReconnected
    // repeatedly — coalesce those into at most one reload per window.
    static constexpr qint64   kReconnectReloadGapMs  = 2 * 60'000;
    qint64                    _lastReconnectReloadMs = 0;
    // Throttle the safety poll's forced socket re-establish so a persistently
    // sick socket can't drive a reconnect → reload storm.
    static constexpr qint64   kReestablishGapMs      = 60'000;
    qint64                    _lastReestablishMs     = 0;
    // Cadence for the background-workspace stall poll (no conversation open). Much
    // slower than the 15 s foreground poll: it's a safety net for a workspace the
    // user isn't looking at, and one history call per workspace per window is
    // plenty to catch a silently-stalled shared socket without risking rate limits.
    static constexpr qint64   kBackgroundPollGapMs   = 2 * 60'000;
    qint64                    _lastBackgroundPollMs  = 0;
    QHash<QString, User>      _botUsers;           // bot_id → User; for bots not in users.list
    QSet<QString>             _pendingBotFetches;  // bot_ids with an in-flight bots.info request
    QSet<QString>             _pendingUserFetches; // user ids with an in-flight users.info request
    rpl::event_stream<UserId> _botInfoHub;
    rpl::event_stream<UserId> _userInfoHub;
    rpl::lifetime             _lifetime;
};
