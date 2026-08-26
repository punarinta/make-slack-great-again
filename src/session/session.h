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
    // Fires when Slack keeps evicting our shared Socket Mode connection because
    // the same compiled-in app keys are running on another device. The UI shows
    // a persistent, dismissable notice (not the transient error banner) — the
    // condition lasts until the user closes the app elsewhere. Throttled.
    rpl::producer<> parallelUsageNotice() const { return _parallelUsageHub.events(); }

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
    void uploadFiles(
        ConversationId     conv,
        const QStringList &filePaths,
        const QString     &text,
        std::optional<Ts>  threadRoot = std::nullopt
    );
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

    // Per-thread mute (local; no public API). A muted thread's replies stop
    // badging and notifying — matching Slack's "Mute thread" — except explicit
    // @mentions, which always get through. `root` is the thread root's ts.
    // Threads are followed ("All new posts") by default, so absence = not muted.
    bool isThreadMuted(const ConversationId &conv, const Ts &root) const;
    void setThreadMuted(const ConversationId &conv, const Ts &root, bool muted);

    // --- Message reminders ("Remind me about this message") ---
    // Local mirror of the backend's saved-item reminders, gated by
    // Capabilities::messageReminders. The server stores the reminder (so it
    // syncs with the official clients), but delivers nothing when it comes due —
    // a Session timer raises EvReminderDue instead, and MainWindow turns that
    // into the OS notification. Set/remove are optimistic: the local entry
    // changes immediately (blue tint, menu state) and rolls back if the server
    // rejects the write. Reminders that came due while the app was closed fire
    // once shortly after start (unless they are stale — see fireDueReminders).
    // 0 = no reminder on that message.
    qint64 messageReminderDue(const ConversationId &conv, const Ts &ts) const;
    bool   hasMessageReminder(const ConversationId &conv, const Ts &ts) const {
        return messageReminderDue(conv, ts) > 0;
    }
    // Every reminder, soonest due first (the "Saved messages" page's data; fired
    // ones stay listed until removed, matching the blue tint in the chat).
    std::vector<MessageReminder> messageReminders() const;
    // `msg` supplies ts plus the local enrichment the server doesn't store:
    // its thread root (routes the notification click into the thread) and a
    // text snippet (the notification body).
    void setMessageReminder(const ConversationId &conv, const Message &msg, qint64 dueAt);
    void removeMessageReminder(const ConversationId &conv, const Ts &ts);
    // Fires whenever the reminder set changes (set/remove/server sync/fired) —
    // the message list re-lays out its rows on this (the due-strip adds height).
    rpl::producer<> remindersChanged() const { return _remindersChangedHub.events(); }

    // Fill in the preview (snippet + author) of every reminder that carries
    // none: one set from another client, or one whose enrichment was lost.
    // Reads the message cache first and only then the network, on the paced
    // background lane, once per reminder per run — so calling it whenever the
    // "Saved messages" page opens is cheap. Fires remindersChanged() as answers
    // land, and persists what it learns.
    void resolveReminderPreviews();

    // Test hook: run the due-reminder check synchronously (normally fired by
    // the single-shot _reminderTimer).
    void fireDueRemindersForTest() { fireDueReminders(); }
    // Test hook: pull the server's reminder list now (normally throttled behind
    // checkRealtimeHealth).
    void refreshRemindersForTest() { refreshReminders(); }
    // Test hook: pull the server's starred-conversation list now (normally
    // throttled behind checkRealtimeHealth).
    void refreshStarredForTest() { refreshStarred(); }

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
    // resetForegroundGap clears the foreground poll's once-per-minute throttle
    // first, so back-to-back ticks each exercise the poll logic; pass false to
    // test the throttle itself.
    void
    runRealtimeHealthCheckForTest(bool resetForegroundGap = true, bool resetBackgroundGap = false) {
        if (resetForegroundGap)
            _lastForegroundPollMs = 0;
        if (resetBackgroundGap)
            _lastBackgroundPollMs = 0;
        checkRealtimeHealth();
    }

    // Test hook: run one conversation-roster reload synchronously, bypassing the
    // kRosterReloadGapMs cadence, so the reload's activity diff (and the merge it
    // feeds) can be exercised twice in a row.
    void reloadConversationsForTest() { reloadConversations(/*refreshEmoji=*/false); }

    // Test hook: run one unread-counts tick synchronously, bypassing the cadence
    // throttle (normally paced by kCountsPollGapMs inside checkRealtimeHealth).
    void pollUnreadCountsForTest() { pollUnreadCounts(); }

    // Test hook: true once the counts snapshot has been given up on and the
    // Session has fallen back to diffing the conversation-roster reload.
    [[nodiscard]] bool unreadCountsDisabledForTest() const { return _countsDisabled; }

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

    // A message (EvMessageNew) arrived for a conversation we don't track yet.
    // The usual cause is a brand-new MPDM/group DM: Slack sends no
    // channel_created or member_joined_channel for those (channels only), so
    // that first message is the ONLY signal the conversation exists. Fetch it
    // via conversations.info, fold it into _conversations, then replay the
    // queued message(s) through handleNewMessage so they badge and notify like
    // any other. Messages that arrive while the fetch is in flight queue in
    // _unknownConvBacklog and drain together on completion. A wrong-workspace id
    // (the shared socket broadcasts every workspace's events to every sink)
    // gets channel_not_found and the backlog is simply dropped.
    void fetchUnknownConversation(ConversationId id, Message msg);

    // Background conversations.info sweep over IMs/MPDMs that refreshes their
    // last_read/latest cursors (used for conversation-list relevance). Throttled
    // via the workspace cache; called after each loadConversations() merge.
    void enrichDmActivity();

    // Reconnect recovery: re-derive unread/mention badges for DMs/MPDMs from
    // conversations.info after the shared realtime socket silently stalled. The
    // stall drops EvMessageNew across many conversations at once; reloadConversations
    // can't recover them (conversations.list reports unread_count=0 and no cursors,
    // so its max-merge keeps the zero count and no badge appears). conversations.info
    // is the only public endpoint that reports the authed user's per-conversation
    // unread count, so sweep the DMs/MPDMs — where a coworker's direct message lands —
    // and fold the server count back in. Scoped to DMs/MPDMs and throttled
    // (kUnreadResyncGapMs): channels need a history scan to tell a "missed" @mention
    // from ordinary unread traffic, and a flapping socket fires EvRealtimeReconnected
    // repeatedly. Unlike enrichDmActivity (the idle 12 h cursor sweep that ignores
    // unread to avoid fabricating badges) we KNOW realtime just dropped events here,
    // so the server count is exactly the signal we want.
    void resyncUnreads();

    // Drop an id from the channel_not_found negative cache (_deadConvIds) —
    // called whenever a conversation legitimately becomes ours (join, member
    // added, DM opened) so a previously-foreign id can be fetched again.
    void markConvAlive(const ConversationId &id);

    // Add an id to the negative cache. The single mutation point, so every
    // insert also schedules the debounced persist.
    void markConvDead(const ConversationId &id);

    // Drop persisted dead marks a fresh conversations.list contradicts: a listed
    // channel/MPDM provably exists again (invited/unarchived while the app was
    // closed — the realtime join event that would have called markConvAlive never
    // reached us). A listed 1:1 DM clears only when its peer is a loaded, active
    // user: Slack lists dead-peer DMs forever (see isDeadDm), so clearing those on
    // list presence alone would re-probe and re-mark them every run.
    void reconcileDeadConvIds(const std::vector<Conversation> &convs);

    // Debounced _deadConvIds persistence: a sweep marks dozens of ids in one
    // burst; coalesce them into a single meta write. Flushed by the destructor.
    void scheduleSaveDeadConvIds();

    // Debounced roster persistence. users.json is the largest cache blob
    // (hundreds of KB on big workspaces) and user_change arrives in bursts
    // (reconnect, profile sweeps); re-serializing the whole roster per event
    // wedged the main thread long enough to trip the hang watchdog and stall
    // the realtime socket. _users is the in-memory truth, so coalescing writes
    // is safe. Flushed by the destructor.
    void scheduleSaveUsers();

    // A DM whose peer left/was deactivated — conversations.info answers
    // channel_not_found for these, so the DM/MPDM sweeps skip them. (MPDMs have no
    // single peer and always sweep.)
    bool isDeadDm(const Conversation &c) const;

    // Live DMs/MPDMs to sweep with conversations.info, 1:1 DMs first (they matter
    // more) then MPDMs. Shared by enrichDmActivity() and resyncUnreads().
    std::vector<ConversationId> dmSweepTargets() const;

    // Resolve any DM peer absent from the loaded user list via users.info. No-op
    // until users.list has loaded (so we don't mistake "not loaded yet" for
    // "missing"); called from both the users and conversations load handlers.
    void fetchMissingDmUsers();

    // Apply `fn` to our own entry in _users (no-op while meUserId is unknown);
    // reassigning the variable notifies users() subscribers.
    void patchMeUser(const std::function<void(User &)> &fn);

    // Apply `fn` to one user's entry in _users WITHOUT notifying users()
    // subscribers. Presence/DND flips arrive in bursts for the whole roster
    // (reconnect, morning login) and reassigning the variable would rebuild
    // every user-derived view once per event; the UI tracks these flips via
    // the targeted EvPresenceChanged/EvDndChanged events instead, and the
    // silent patch keeps later users() snapshots truthful.
    void patchUserSilently(const UserId &id, const std::function<void(User &)> &fn);

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
    // staleness check to _openConv; background yields only if the exact
    // conversation it targeted became the open one mid-flight (the foreground
    // path now covers that one instead).
    //
    // `baselineHint` is the newest ts a caller already knows we have seen for this
    // conversation — the activity diff passes the PREVIOUS snapshot's latest ts, so
    // the poll injects exactly the messages that arrived since. Used only to seed a
    // conversation we have no baseline of our own for yet (see _pollBaseline).
    void pollConversationForMissed(ConversationId conv, bool foreground, Ts baselineHint = {});

    // Round-robins through every member conversation (most-recently-active
    // first), one per background poll tick, skipping `exclude` (normally
    // _openConv — the foreground path already covers it faster). Rotating instead
    // of always re-picking the single busiest conversation guarantees every
    // channel eventually gets checked, not just the top one.
    ConversationId nextBackgroundPollTarget(const ConversationId &exclude);

    // Ask the backend for a whole-workspace unread/activity snapshot and feed it
    // to applyActivitySnapshot. Self-degrading: a backend that can't serve one
    // completes without a value, and after kCountsFailureLimit such attempts we
    // stop asking (_countsDisabled) and let the roster reload's own diff drive
    // discovery instead. Poll-only backends only — a push transport already
    // delivers, so spending a request per tick there would be pure waste.
    void pollUnreadCounts();

    // Diff a fresh activity snapshot against the previous one and poll the
    // conversations that actually moved — the mechanism that makes a mention in a
    // never-opened channel notify on a backend with no push transport. The FIRST
    // snapshot only primes (it says where every conversation stands, not what
    // changed, so injecting off it would replay every pre-existing unread as a new
    // message and fire a burst of notifications).
    void applyActivitySnapshot(const std::vector<ConvCounts> &snapshot);

    std::unique_ptr<Backend>        _backend;
    std::unique_ptr<WorkspaceCache> _cache;

    rpl::variable<std::vector<Conversation>> _conversations;
    rpl::variable<std::vector<User>>         _users;
    rpl::variable<SelfPresence>              _selfPresence;
    QTimer                                   _selfPresenceTimer;
    QTimer                                   _realtimeSafetyTimer; // 15 s; checkRealtimeHealth()
    QTimer                                   _saveUnreadsTimer; // debounces scheduleSaveUnreads()
    QTimer                               _saveDeadConvsTimer; // debounces scheduleSaveDeadConvIds()
    QTimer                               _saveUsersTimer;     // debounces scheduleSaveUsers()
    // Conversation snapshots queued by cacheMessages(): the JSON serialization
    // + file write is deferred off the conversation-switch click path. Flushed
    // by the timer and the destructor; cachedMessages() reads the queue first
    // so an immediate switch-back never sees a stale file.
    QHash<QString, std::vector<Message>> _pendingMsgWrites;
    QTimer                               _saveMsgsTimer;
    void                                 flushPendingMsgWrites();
    // resyncUnreads replies batched: merging each conversations.info response
    // individually reassigned _conversations — one full conv-list rebuild per
    // DM in the sweep. Replies collect here and merge in one reassignment per
    // burst (timer-flushed so early replies still land promptly).
    std::vector<std::pair<ConversationId, Conversation>> _pendingUnreadInfos;
    QTimer                                               _unreadPatchTimer;
    void                                                 applyPendingUnreadInfos();
    rpl::event_stream<Event>                             _eventHub;
    rpl::event_stream<QString>                           _errorHub;
    rpl::event_stream<>                                  _parallelUsageHub;
    rpl::event_stream<>                                  _emojiMapLoadedHub;

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
    // Messages awaiting a conversations.info fetch for a conversation we don't
    // track yet (see fetchUnknownConversation). A present key means a fetch is
    // in flight; the queued messages drain when it completes.
    QHash<QString, QList<Message>>     _unknownConvBacklog; // conv.value → queued msgs
    // Conversation ids that answered channel_not_found from conversations.info —
    // convs that don't exist for this workspace: another workspace's convs
    // (the shared socket broadcasts every workspace's events to every sink) and
    // dead DMs (peer left/deactivated). Remembering them stops a busy foreign
    // conversation re-firing a fetch on every message (fetchUnknownConversation)
    // and stops the reconnect sweeps re-hitting dead DMs every time
    // (dmSweepTargets). Never consulted on the handleNewMessage notify path, so
    // it can't suppress a message for a conversation we actually track.
    // Persisted across restarts (loadDeadConvIds/saveDeadConvIds) so startup
    // doesn't re-probe the whole set. No TTL; instead every way an id can come
    // back to life clears it: join/member-added/DM-open events (markConvAlive),
    // presence in a fresh conversations.list (reconcileDeadConvIds — covers
    // anything that happened while the app was closed), and a DM peer's
    // reactivation (the EvUserChanged handler).
    QSet<QString>                      _deadConvIds;  // conv.value → known channel_not_found
    QSet<QString>                      _seenMsgKeys;  // "conv|ts" of delivered messages
    QQueue<QString>                    _seenMsgOrder; // FIFO eviction for _seenMsgKeys
    // Muted threads, keyed by threadMuteKey(conv, root). Loaded from cache at
    // start(); persisted immediately on change (tiny payload).
    QSet<QString>                      _mutedThreads;
    static QString                     threadMuteKey(const ConversationId &conv, const Ts &root) {
        return conv.value + QLatin1Char('\t') + root;
    }

    // --- Message reminders (see the public reminder API above) ---
    // Arm _reminderTimer for the nearest unfired due time (stopped when none).
    void armReminderTimer();
    // Fire EvReminderDue for every reminder now due, mark it fired, re-arm.
    void fireDueReminders();
    // Pull the authoritative reminder list from the backend (throttled by
    // kRemindersRefreshGapMs from checkRealtimeHealth) — picks up reminders
    // set/removed from other clients. Replaces local state only when the
    // backend actually answers (unsupported backends never emit).
    void refreshReminders();
    // Pull the authoritative starred-conversation list from the backend
    // (throttled by kStarredRefreshGapMs from checkRealtimeHealth). Nothing in
    // the conversation listing reports the star, so this is what makes it
    // survive a restart and what picks up a star set from another client.
    // Replaces local state only when the backend actually answers.
    void refreshStarred();
    // Persist + notify UI + re-arm: every mutation of _reminders funnels here.
    void reminderStoreChanged();
    // Coalesced reminderStoreChanged() for the preview resolver, whose answers
    // trickle in one background request at a time.
    void scheduleReminderStoreFlush();

    // --- Reminder previews (see ReminderPreview in domain.h) ---
    // Copy r's enrichment into the shadow map / back out of it into a record
    // that is missing it. The map is the reason a server snapshot can re-add an
    // item without stranding it as a blank card.
    void rememberReminderPreview(const MessageReminder &r);
    void applyReminderPreview(const QString &key, MessageReminder &r) const;
    // Drop previews for reminders that no longer exist anywhere (keeps the map
    // bounded); no-op — and no cache write — when nothing goes.
    void pruneReminderPreviews(const QHash<QString, MessageReminder> &live);
    // Fetch the reminded message and enrich the record from it, then `done`
    // (called whether or not anything was found).
    void resolveReminderPreview(const QString &key, std::function<void()> done = {});
    // Raise EvReminderDue for `key`, resolving its preview first when it has
    // none — a notification body of "You asked to be reminded about a message"
    // is a poor substitute for the message.
    void announceReminderDue(const QString &key);
    void emitReminderDue(const QString &key);

    static QString reminderKey(const ConversationId &conv, const Ts &ts) {
        return conv.value + QLatin1Char('\t') + ts;
    }
    // Keyed by reminderKey(conv, ts). Loaded from cache at start().
    QHash<QString, MessageReminder> _reminders;
    // Enrichment by the same key, kept whether or not the reminder is currently
    // in _reminders, and persisted separately (see ReminderPreview).
    QHash<QString, ReminderPreview> _reminderPreviews;
    // Keys resolveReminderPreview already tried this run — a deleted message
    // must not be re-fetched every time the Saved messages page opens.
    QSet<QString>                   _reminderResolveTried;
    QTimer                          _reminderFlushTimer; // debounced store-changed
    // When each locally-added reminder was created (ms epoch; in-memory only).
    // A server snapshot requested just before an optimistic add would come back
    // without it — entries younger than the grace window survive such a sync.
    QHash<QString, qint64>          _reminderCreatedMs;
    QTimer                          _reminderTimer; // single-shot, armReminderTimer()
    rpl::event_stream<>             _remindersChangedHub;
    static constexpr qint64         kRemindersRefreshGapMs  = 5 * 60'000;
    qint64                          _lastRemindersRefreshMs = 0;
    static constexpr qint64         kStarredRefreshGapMs    = 5 * 60'000;
    qint64                          _lastStarredRefreshMs   = 0;
    // convId → ms epoch of the last star toggled from THIS client. A snapshot
    // already in flight when the user starred comes back without it; honouring
    // it would flip the row straight back. In-memory only.
    QHash<QString, qint64>          _starChangedMs;
    static constexpr qint64         kStarGraceMs             = 60'000;
    // A reminder overdue by more than this when discovered (app closed for
    // days) is marked fired silently instead of raising a stale notification.
    static constexpr qint64         kMaxReminderLatenessSecs = 7 * 24 * 3600;

    // Throttle the rate-limit notice banner (429s cluster; don't spam).
    static constexpr qint64 kRateLimitNoticeGapMs  = 15'000;
    qint64                  _lastRateLimitNoticeMs = 0;

    // Throttle the realtime-contention banner. SocketModeRealtime already rate-
    // limits how often it raises EvRealtimeContended, but it broadcasts to every
    // workspace sink, so guard here too so a multi-workspace app shows one banner
    // per window rather than one per session.
    static constexpr qint64 kContentionNoticeGapMs  = 5 * 60'000;
    qint64                  _lastContentionNoticeMs = 0;

    // Throttle reconnect-driven full conversation reloads. conversations.list is
    // heavily rate-limited, and a flapping socket can fire EvRealtimeReconnected
    // repeatedly — coalesce those into at most one reload per window.
    static constexpr qint64    kReconnectReloadGapMs  = 2 * 60'000;
    qint64                     _lastReconnectReloadMs = 0;
    // Throttle reconnect-driven DM/MPDM unread recovery (resyncUnreads). Same
    // rationale as the reload throttle: a flapping socket fires EvRealtimeReconnected
    // repeatedly, and a per-DM conversations.info sweep over 100+ DMs each time would
    // be a rate-limit storm. Independent of the reload throttle so either can run.
    static constexpr qint64    kUnreadResyncGapMs     = 2 * 60'000;
    qint64                     _lastUnreadResyncMs    = 0;
    // Number of conversations.info calls from the current unread-resync sweep that
    // have not yet settled. The sweep is paced (~1.2 s/call), so on a busy
    // workspace it can still be draining when the throttle window reopens; a fresh
    // sweep while one is in flight would just re-enqueue the same DMs. Skip until
    // the outstanding batch finishes so the paced lane can't accumulate duplicates.
    int                        _unreadResyncInFlight  = 0;
    // Throttle the safety poll's forced socket re-establish so a persistently
    // sick socket can't drive a reconnect → reload storm.
    static constexpr qint64    kReestablishGapMs      = 60'000;
    qint64                     _lastReestablishMs     = 0;
    // Cadence for the active-chat realtime fallback poll. The safety timer ticks
    // every 15 s (to run the cheap, API-free verifyRealtime check), but the history
    // poll itself must stay within Slack's conversations.history budget: as of
    // 2025-05-29 non-Marketplace apps get just 1 request/min on that method (max 15
    // objects), and it answers 429 Retry-After: 60 above that. Polling every tick
    // was 4x over budget and, with no cooldown guard, piled identical calls into the
    // HttpQueue — a self-inflicted 429 storm even while idle. The socket is the
    // primary delivery; this poll is only a backstop for a silently-stalled socket,
    // so once per minute is plenty. See checkRealtimeHealth().
    static constexpr qint64    kForegroundPollGapMs   = 60'000;
    qint64                     _lastForegroundPollMs  = 0;
    // Cadence for the background-workspace stall poll (no conversation open). Much
    // slower than the foreground poll: it's a safety net for a workspace the user
    // isn't looking at, and one history call per workspace per window is plenty to
    // catch a silently-stalled shared socket without risking rate limits.
    static constexpr qint64    kBackgroundPollGapMs   = 2 * 60'000;
    qint64                     _lastBackgroundPollMs  = 0;
    // Poll-only backends (session auth): cadence for reloading the conversation
    // roster to discover new chats + refresh latestTs baselines. Push backends
    // get this via reconnect events instead. Kept slow (60 s): conversations.list
    // is Tier 2 (as low as ~1/min for a session token), and the open chat streams
    // via the 5 s foreground poll regardless, so this only paces new-chat/badge
    // discovery — a tighter cadence just 429s the endpoint.
    static constexpr qint64    kRosterReloadGapMs     = 60'000;
    qint64                     _lastRosterReloadMs    = 0;
    // Per-conversation poll baseline: the newest ts of the last head page THIS
    // SESSION's poll actually scanned. Keyed by ConversationId string.
    //
    // Deliberately not Conversation::latestTs, which the poll used to read
    // directly: latestTs is also advanced by the badge/activity sweeps
    // (conversations.info), and those raise no EvMessageNew. A sweep that landed
    // first therefore pushed the baseline PAST a message nobody had seen, and the
    // `ts > baseline` filter then skipped it forever — the badge appeared and the
    // notification was lost for good. A baseline only ever advanced by a poll that
    // scanned the page cannot swallow anything: at worst a message is re-scanned
    // and dropped as a duplicate by firstSighting.
    QHash<QString, Ts>         _pollBaseline;
    // Last activity snapshot per conversation (see applyActivitySnapshot), the
    // reference the next snapshot is diffed against. Keyed by ConversationId
    // string. Fed by ONE source at a time — the counts snapshot while it works,
    // the roster reload after we give up on it — since the two report different
    // fields and would otherwise read as spurious movement in each other's gaps.
    QHash<QString, ConvCounts> _activity;
    bool                       _activityPrimed      = false;
    // Cadence of the unread-counts snapshot on a poll-only backend. One request
    // covers the whole workspace, so this can be far tighter than any
    // per-conversation poll — it is the app's realtime substitute there.
    static constexpr qint64    kCountsPollGapMs     = 10'000;
    qint64                     _lastCountsPollMs    = 0;
    // Consecutive snapshot attempts that produced nothing, and the latch they
    // trip. A blip (offline, a 429 that exhausted its retries) must not cost us
    // the mechanism, but an endpoint this token can't use never will work, so
    // stop asking and fall back rather than burn a request every tick forever.
    static constexpr int       kCountsFailureLimit  = 3;
    int                        _countsFailures      = 0;
    bool                       _countsDisabled      = false;
    // Ceiling on conversations polled per activity diff. Waking from suspend (or
    // a first snapshot after a long stall) can report dozens of moved
    // conversations at once, and one conversations.history each would burst
    // straight into a 429. Conversations over the cap keep their OLD snapshot
    // entry, so the next tick still sees them as moved and they drain over the
    // following ticks instead of being dropped.
    static constexpr int       kMaxDiffPollsPerTick = 8;
    // Cursor into nextBackgroundPollTarget()'s (re-sorted-each-call) candidate
    // list, so successive ticks rotate through every member conversation
    // instead of always re-polling whichever one is most active.
    int                        _backgroundPollIdx   = 0;
    QHash<QString, User>       _botUsers;           // bot_id → User; for bots not in users.list
    QSet<QString>              _pendingBotFetches;  // bot_ids with an in-flight bots.info request
    QSet<QString>              _pendingUserFetches; // user ids with an in-flight users.info request
    rpl::event_stream<UserId>  _botInfoHub;
    rpl::event_stream<UserId>  _userInfoHub;
    rpl::lifetime              _lifetime;
};
