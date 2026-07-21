// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "session.h"
#include "backend/backend.h"
#include "backend/common_commands.h"
#include "cache/workspace_cache.h"
#include "text/mrkdwn_parser.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <algorithm>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QUrl>

Session::Session(std::unique_ptr<Backend> backend, const QString &teamId)
    : _backend(std::move(backend)), _cache(std::make_unique<WorkspaceCache>(teamId)) {}

Session::~Session() {
    // Batched unread merges (_pendingUnreadInfos) are deliberately dropped:
    // applying them would reassign _conversations and fire UI observers on a
    // session that is mid-teardown; the next start re-syncs anyway.
    // Don't lose a debounced unread save when the session is torn down (logout,
    // drop, app exit) before the timer fired.
    if (_saveUnreadsTimer.isActive())
        persistUnreads();
    if (_saveDeadConvsTimer.isActive())
        _cache->saveDeadConvIds(QStringList(_deadConvIds.begin(), _deadConvIds.end()));
    if (_saveUsersTimer.isActive())
        _cache->saveUsers(_users.current());
    flushPendingMsgWrites(); // queued message-cache writes (no-op when empty)
}

// App-level slash commands: OUR app's own commands, available in every workspace
// regardless of service. Currently the home for future AI content commands (e.g.
// /summarize, /rewrite, /ask) routed through LlmService — NOT yet implemented.
// When added, return the definitions here and gate them on
// LlmService::instance().availability() so they only appear when an AI provider is
// connected. Service-specific commands (Slack's /shrug, /away, … and Teams' own,
// once it has any) live in each backend's nativeCommands(), not here — so a
// Slack-only command never shows in a Teams composer.
static std::vector<SlashCommand> appCommands() {
    return {};
}

// Tokens issued before a scope was added to OAuthFlow::userScopes() lack it;
// only a fresh sign-in can grant it, so point the user there.
static QString withReauthHint(QString msg, const QString &err) {
    if (err == QLatin1String("missing_scope"))
        msg += QCoreApplication::translate(
            "Session", " — sign in to this workspace again to grant the new permission"
        );
    return msg;
}

// Maps a raw Slack send-failure code to a human-readable sentence. Unknown
// codes fall through to the raw code so nothing is hidden when diagnosing.
static QString friendlySendError(const QString &err) {
    if (err == QLatin1String("cannot_reply_to_message"))
        return QCoreApplication::translate("Session", "You can't reply to this message.");
    if (err == QLatin1String("not_in_channel"))
        return QCoreApplication::translate("Session", "You're not a member of this channel.");
    if (err == QLatin1String("is_archived"))
        return QCoreApplication::translate("Session", "This conversation is archived.");
    if (err == QLatin1String("msg_too_long"))
        return QCoreApplication::translate("Session", "The message is too long.");
    if (err == QLatin1String("channel_not_found"))
        return QCoreApplication::translate("Session", "This conversation no longer exists.");
    if (err == QLatin1String("restricted_action") || err == QLatin1String("no_permission"))
        return QCoreApplication::translate("Session", "You don't have permission to post here.");
    return err;
}

// Parses /dnd durations: "30" / "45m" / "2h" / "1h 30m" / "1 hour" → minutes;
// "off" / "end" / "resume" → 0 (end snooze); anything else → -1.
static int parseDndMinutes(const QString &args) {
    const QString a = args.trimmed().toLower();
    if (a == QLatin1String("off") || a == QLatin1String("end") || a == QLatin1String("resume"))
        return 0;
    static const QRegularExpression re(
        QStringLiteral("^(?:(\\d+)\\s*h[a-z]*)?\\s*(?:(\\d+)\\s*(?:m[a-z]*)?)?$")
    );
    const auto m = re.match(a);
    if (!m.hasMatch() || (m.captured(1).isEmpty() && m.captured(2).isEmpty()))
        return -1;
    const int minutes = m.captured(1).toInt() * 60 + m.captured(2).toInt();
    return minutes > 0 ? minutes : -1;
}

void Session::start() {
    // Serve cached data immediately so the UI has something to show before
    // the network responds.
    {
        auto convs = _cache->loadConversations();
        if (!convs.empty())
            _conversations = std::move(convs);
        auto users = _cache->loadUsers();
        if (!users.empty())
            _users = std::move(users);
        _botUsers        = _cache->loadBots();
        _emojiMap        = _cache->loadEmojiMap();
        const auto muted = _cache->loadMutedThreads();
        _mutedThreads    = QSet<QString>(muted.begin(), muted.end());
        // The channel_not_found negative cache survives restarts so startup
        // doesn't re-probe every foreign/dead conversation. reconcileDeadConvIds
        // (off the fresh conversations.list) clears whatever came back to life
        // while the app was closed.
        const auto dead  = _cache->loadDeadConvIds();
        _deadConvIds     = QSet<QString>(dead.begin(), dead.end());
        // Seed our own user id from cache so optimistic sends carry the right
        // author (avatar/name, and ghost removal on the realtime echo) even
        // before auth.test answers — or if it fails this run entirely.
        _meUserId        = _cache->loadMeUserId();
    }

    _backend->connectRealtime();

    // Resolve our own user id first — optimistic sends and own-message
    // detection depend on it.
    fetchMe();

    // Poll rich self-presence so the UI can show how the user appears to
    // others; there is no realtime event for your own connection count.
    refreshSelfPresence();
    QObject::connect(&_selfPresenceTimer, &QTimer::timeout, [this] { refreshSelfPresence(); });
    _selfPresenceTimer.start(60 * 1000);

    // Safety net for the realtime socket. Its own watchdog only catches a
    // connection that goes silent; it cannot see one that still answers pings
    // but to which Slack has quietly stopped delivering events — the cause of
    // messages sent from another client never showing up. Every 15 s, re-verify
    // the subscription and poll the open conversation for anything missed.
    QObject::connect(&_realtimeSafetyTimer, &QTimer::timeout, [this] { checkRealtimeHealth(); });
    _realtimeSafetyTimer.start(15 * 1000);

    // Debounced unread persistence (see scheduleSaveUnreads).
    _saveUnreadsTimer.setSingleShot(true);
    QObject::connect(&_saveUnreadsTimer, &QTimer::timeout, [this] { persistUnreads(); });

    // Debounced dead-conversation persistence (see scheduleSaveDeadConvIds).
    _saveDeadConvsTimer.setSingleShot(true);
    QObject::connect(&_saveDeadConvsTimer, &QTimer::timeout, [this] {
        _cache->saveDeadConvIds(QStringList(_deadConvIds.begin(), _deadConvIds.end()));
    });

    // Debounced roster persistence (see scheduleSaveUsers).
    _saveUsersTimer.setSingleShot(true);
    QObject::connect(&_saveUsersTimer, &QTimer::timeout, [this] {
        _cache->saveUsers(_users.current());
    });

    // Deferred per-conversation message-cache writes (see cacheMessages).
    _saveMsgsTimer.setSingleShot(true);
    QObject::connect(&_saveMsgsTimer, &QTimer::timeout, [this] { flushPendingMsgWrites(); });

    // Batched unread-resync merges (see resyncUnreads / applyPendingUnreadInfos).
    _unreadPatchTimer.setSingleShot(true);
    QObject::connect(&_unreadPatchTimer, &QTimer::timeout, [this] { applyPendingUnreadInfos(); });

    // Load conversations; update cache on arrival.
    reloadConversations(/*refreshEmoji=*/true);

    // Load users; update cache on arrival.
    _backend->loadUsers() | rpl::on_next(
                                [this](std::vector<User> users) {
                                    // Merge the snapshot into what's already known rather than
                                    // replacing outright. The snapshot is not complete for any
                                    // backend: Slack's users.list never lists Slack Connect
                                    // external (EXT) peers, and Teams may omit self and supplies
                                    // users without avatars (filled in asynchronously via
                                    // EvUserChanged). A plain replace drops those users until
                                    // users.info re-resolves them one by one — an "Unknown user"
                                    // + no-avatar flicker right after start, in both the message
                                    // list and DM/MPDM titles. So: snapshot rows win, cached
                                    // enrichment (avatar, display name, live presence) fills the
                                    // gaps they leave, and known users the snapshot omits are
                                    // retained. Retention is safe — departed members come back
                                    // as deleted:true rows, not as absences.
                                    {
                                        const auto                   prev = _users.current();
                                        QHash<QString, const User *> prevById;
                                        for (const auto &u : prev)
                                            prevById.insert(u.id.value, &u);
                                        QSet<QString> inSnapshot;
                                        inSnapshot.reserve(int(users.size()));
                                        for (auto &u : users) {
                                            inSnapshot.insert(u.id.value);
                                            const auto it = prevById.constFind(u.id.value);
                                            if (it == prevById.constEnd())
                                                continue;
                                            const User &old = *it.value();
                                            if (u.avatarUrl.isEmpty())
                                                u.avatarUrl = old.avatarUrl;
                                            if (u.displayName.isEmpty())
                                                u.displayName = old.displayName;
                                            if (u.name.isEmpty())
                                                u.name = old.name;
                                            // Presence/DND are polled separately, never carried by
                                            // loadUsers — keep the live values across the refresh.
                                            u.isActive   = old.isActive;
                                            u.dndEnabled = old.dndEnabled;
                                        }
                                        for (const auto &u : prev)
                                            if (!inSnapshot.contains(u.id.value))
                                                users.push_back(u);
                                    }
                                    _users = std::move(users);
                                    scheduleSaveUsers();
                                    // Update admin flag now that the full user list is available.
                                    if (!_meUserId.value.isEmpty()) {
                                        if (const User *u = findUser(_meUserId))
                                            _meIsAdmin = u->isAdmin;
                                    }
                                    // Subscribe to real-time presence events for all non-bot users.
                                    std::vector<UserId> ids;
                                    for (const auto &u : _users.current())
                                        if (!u.isBot && !u.isDeactivated)
                                            ids.push_back(u.id);
                                    _backend->subscribePresence(std::move(ids));

                                    // The self-presence and auth.test calls made at start() can
                                    // race the startup token refresh and come back empty;
                                    // users.list landing proves the token works, so retry now
                                    // (ahead of the per-DM polls below, which share the request
                                    // queue). A session without meUserId ghosts every send: the
                                    // optimistic copy has no author and is never reconciled with
                                    // its realtime echo, so the message shows up twice.
                                    if (!_selfPresence.current().loaded)
                                        refreshSelfPresence();
                                    if (_meUserId.value.isEmpty())
                                        fetchMe();

                                    // Poll current presence for every DM conversation partner so
                                    // the list shows the right indicator without waiting for the
                                    // first change.
                                    for (const auto &conv : _conversations.current())
                                        if (conv.dmUser && !conv.dmUser->value.isEmpty())
                                            requestPresence(*conv.dmUser);

                                    // Resolve DM peers users.list omits now that the
                                    // full list is known (conversations may already
                                    // be loaded; if not, their handler calls us too).
                                    fetchMissingDmUsers();
                                },
                                _lifetime
                            );

    // Wire the backend event firehose through our hub so Session can
    // intercept and patch state before forwarding to the UI.
    _backend->events() |
        rpl::on_next(
            [this](Event e) {
                // Patch in-memory state then forward.
                if (auto *ev = std::get_if<EvPresenceChanged>(&e)) {
                    // Silent: the forwarded event below carries the change to
                    // the UI; a users() re-emission per presence flip would
                    // rebuild the whole conv list once per roster member.
                    patchUserSilently(ev->user, [active = ev->active](User &u) {
                        u.isActive = active;
                    });
                } else if (auto *ev = std::get_if<EvDndChanged>(&e)) {
                    patchUserSilently(ev->user, [dnd = ev->dndEnabled](User &u) {
                        u.dndEnabled = dnd;
                    });
                } else if (auto *ev = std::get_if<EvUserChanged>(&e)) {
                    // Profile/avatar update. Take the fresh fields but keep the
                    // live presence/DND state — the user_change payload doesn't
                    // carry it (isActive is poll-filled, dndEnabled tracked via
                    // EvDndChanged). A changed avatar yields a new image_72 URL
                    // (the hash is embedded), so reassigning _users lets the UI
                    // rebuild and ImageCache fetch the new image on its own.
                    auto users = _users.current();
                    auto it    = std::find_if(users.begin(), users.end(), [&](const User &u) {
                        return u.id == ev->user.id;
                    });
                    if (it != users.end()) {
                        User merged       = ev->user;
                        merged.isActive   = it->isActive;
                        merged.dndEnabled = it->dndEnabled;
                        *it               = std::move(merged);
                    } else {
                        users.push_back(ev->user);
                    }
                    _users = std::move(users);
                    scheduleSaveUsers();
                    // A reactivated peer makes their DM answer conversations.info
                    // again — drop any dead mark so the sweeps resume covering it.
                    // (conversations.list can't clear DMs: it lists dead-peer DMs
                    // forever, see reconcileDeadConvIds.)
                    if (!ev->user.isDeactivated)
                        for (const auto &c : _conversations.current())
                            if (c.kind == ConvKind::Im && c.dmUser == ev->user.id)
                                markConvAlive(c.id);
                } else if (auto *ev = std::get_if<EvUsersChanged>(&e)) {
                    // Bulk EvUserChanged: same merge, but ONE roster copy and
                    // ONE _users re-emission for the whole batch. (No
                    // markConvAlive sweep here — the only bulk sender today is
                    // avatar refreshes, not reactivations.)
                    auto                users = _users.current();
                    QHash<QString, int> idxById;
                    for (int i = 0; i < int(users.size()); ++i)
                        idxById.insert(users[i].id.value, i);
                    for (const User &nu : ev->users) {
                        const auto it = idxById.constFind(nu.id.value);
                        if (it == idxById.constEnd()) {
                            idxById.insert(nu.id.value, int(users.size()));
                            users.push_back(nu);
                            continue;
                        }
                        User &old         = users[size_t(it.value())];
                        User  merged      = nu;
                        merged.isActive   = old.isActive;
                        merged.dndEnabled = old.dndEnabled;
                        old               = std::move(merged);
                    }
                    _users = std::move(users);
                    scheduleSaveUsers();
                } else if (auto *ev = std::get_if<EvConvMarked>(&e)) {
                    auto convs = _conversations.current();
                    for (auto &c : convs) {
                        if (c.id == ev->conv) {
                            c.lastRead     = ev->lastRead;
                            c.unread       = ev->unread;
                            c.mentionCount = ev->mentionCount;
                            break;
                        }
                    }
                    _conversations = std::move(convs);
                } else if (auto *ev = std::get_if<EvMessageNew>(&e)) {
                    // A message for a conversation we don't track yet — almost
                    // always a freshly-created MPDM/group DM, for which Slack
                    // sends no channel_created/member_joined_channel, only this
                    // message. Fetch the conversation, then replay the message
                    // so it lands in the list, badges, and notifies. Don't
                    // forward now: findConversation is still null, so the UI has
                    // nothing to attach it to (fetchUnknownConversation re-fires).
                    if (!findConversation(ev->conv)) {
                        fetchUnknownConversation(ev->conv, ev->msg);
                        return;
                    }
                    // The same message can arrive twice — the chat.postMessage
                    // response echo plus the realtime echo, or a Socket Mode
                    // redelivery of an un-acked envelope. handleNewMessage
                    // de-dups and patches state; a duplicate isn't forwarded.
                    if (!handleNewMessage(ev->conv, ev->msg))
                        return;
                } else if (auto *ev = std::get_if<EvSendFailed>(&e)) {
                    // The send definitively failed (transport problems are
                    // retried before this fires) — drop the translucent
                    // optimistic copy and tell the user. Sends confirm or
                    // fail in FIFO order, so the oldest text-kind ghost is
                    // the one that failed.
                    auto it = _pendingSends.find(ev->conv.value);
                    if (it != _pendingSends.end()) {
                        int idx = 0;
                        while (idx < it->size() && (*it)[idx].withFiles)
                            ++idx;
                        if (idx < it->size())
                            _eventHub.fire(EvMessageDeleted{ev->conv, it->takeAt(idx).ts});
                    }
                    _errorHub.fire(
                        QCoreApplication::translate("Session", "Couldn't send message: %1")
                            .arg(friendlySendError(ev->reason))
                    );
                } else if (auto *ev = std::get_if<EvHuddleChanged>(&e)) {
                    // Patch live-huddle state; the conversations() producer
                    // re-fires, so the huddle banner and conv-list indicator
                    // update in real time.
                    auto convs   = _conversations.current();
                    bool changed = false;
                    for (auto &c : convs) {
                        if (c.id != ev->conv)
                            continue;
                        if (c.huddleActive != ev->active || c.huddleLink != ev->link ||
                            c.huddleParticipants != ev->participants) {
                            c.huddleActive       = ev->active;
                            c.huddleLink         = ev->link;
                            c.huddleParticipants = ev->participants;
                            changed              = true;
                        }
                        break;
                    }
                    if (changed)
                        _conversations = std::move(convs);
                } else if (auto *ev = std::get_if<EvMemberJoined>(&e)) {
                    // member_joined_channel fires for every member; we only care
                    // when it's us joining a channel we don't already track as a
                    // member (we were added/invited). Pull its info so the channel
                    // slots into the list without a manual refresh.
                    if (!_meUserId.value.isEmpty() && ev->user == _meUserId) {
                        const auto &convs = _conversations.current();
                        const auto  it =
                            std::find_if(convs.begin(), convs.end(), [&](const Conversation &c) {
                                return c.id == ev->conv;
                            });
                        if (it == convs.end() || !it->isMember)
                            fetchJoinedConversation(ev->conv);
                    }
                } else if (std::get_if<EvRealtimeReconnected>(&e)) {
                    // The socket came back after a gap Slack won't replay.
                    // Refetch the conversation list so unread/mention badges and
                    // latest-message cursors catch up (refreshEmoji=false: emoji
                    // rarely changes and shouldn't requeue ahead of this). The
                    // open MessageList backfills its own history off this same
                    // event after the re-fire below.
                    //
                    // Coalesce: a flapping socket fires this repeatedly, and
                    // conversations.list is heavily rate-limited — one reload per
                    // window is plenty for badge catch-up and avoids hammering a
                    // 429'd endpoint. The MessageList backfill below is cheap
                    // (single history page) and always runs.
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - _lastReconnectReloadMs >= kReconnectReloadGapMs) {
                        _lastReconnectReloadMs = now;
                        reloadConversations(/*refreshEmoji=*/false);
                    }
                    // conversations.list (above) can't recover unread badges for
                    // messages the stalled socket dropped — it returns unread_count=0.
                    // Re-derive DM/MPDM unreads from conversations.info so a coworker's
                    // missed message surfaces as a badge without the user having to
                    // open the chat. Self-throttled; upward-merges, so it composes with
                    // the reload above regardless of which completes first.
                    resyncUnreads();
                } else if (std::holds_alternative<EvRealtimeContended>(e)) {
                    // Slack keeps evicting our shared socket from the app's
                    // connection pool — another msga instance is running on the
                    // same compiled-in app-level xapp token. msga is single-instance
                    // per user (SingleInstance), so it is NOT a second copy on this
                    // computer: the cause is another device/account. Name that so
                    // the user doesn't hunt locally; throttled (kContentionNoticeGapMs).
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - _lastContentionNoticeMs >= kContentionNoticeGapMs) {
                        _lastContentionNoticeMs = now;
                        // Persistent, dismissable notice (built + shown by the UI) —
                        // the condition lasts until the user closes the app on the
                        // other device, so a 5 s transient banner would be useless.
                        _parallelUsageHub.fire({});
                    }
                } else if (auto *ev = std::get_if<EvRateLimited>(&e)) {
                    // Surface a transient notice naming the throttled method, so
                    // it's clear whether it's a background sweep (conversations.info
                    // /.list/.history on reconnect) or an interactive call. 429s
                    // cluster (background polls hammer the same window), so throttle
                    // to one banner per window — otherwise a sweep would spam it
                    // repeatedly; the banner names whichever method tripped first.
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - _lastRateLimitNoticeMs >= kRateLimitNoticeGapMs) {
                        _lastRateLimitNoticeMs = now;
                        _errorHub.fire(
                            QCoreApplication::translate(
                                "Session",
                                "Slack is rate-limiting requests (%1) — retrying in %n second(s).",
                                nullptr,
                                ev->retryAfterSecs
                            )
                                .arg(ev->method)
                        );
                    }
                }
                _eventHub.fire(std::move(e));
            },
            _lifetime
        );

    // Slash commands come from three sources, each scoped to what's actually
    // available so a Slack-only command never shows in (say) a Teams composer:
    //   • app-level — OUR app's own commands (future AI), available everywhere;
    //   • backend-native — what THIS backend exposes (nativeCommands()): Slack's
    //     /shrug, /away, … live here; Teams has none yet, so it shows nothing;
    //   • backend server commands — the async commands.list (Slack app commands).
    _commands          = appCommands();
    const auto natives = _backend->nativeCommands();
    _commands.insert(_commands.end(), natives.begin(), natives.end());

    // commands.list (when supported) adds app commands; keep the app-level and
    // native commands it doesn't mention. Subscribed last so it queues behind the
    // core data loads.
    _backend->listCommands() |
        rpl::on_next(
            [this](std::vector<SlashCommand> cmds) {
                if (cmds.empty())
                    return;
                for (const auto &existing : _commands) {
                    const bool dup =
                        std::any_of(cmds.begin(), cmds.end(), [&existing](const SlashCommand &c) {
                            return c.name.compare(existing.name, Qt::CaseInsensitive) == 0;
                        });
                    if (!dup)
                        cmds.push_back(existing);
                }
                _commands = std::move(cmds);
            },
            _lifetime
        );
}

bool Session::handleNewMessage(const ConversationId &conv, const Message &msg) {
    // De-dupe: the chat.postMessage response echo, the realtime echo, a Socket
    // Mode envelope redelivery, and the periodic safety poll can all surface the
    // same (conv, ts). Forward only the first sighting.
    if (!firstSighting(conv, msg.ts))
        return false;
    const bool ownMessage = !_meUserId.value.isEmpty() && msg.author == _meUserId;
    {
        auto convs = _conversations.current();
        for (auto &c : convs) {
            if (c.id != conv)
                continue;
            c.latestTs = msg.ts;
            if (ownMessage || !c.isMember)
                break;
            if (conv == _readingConv) {
                // On screen right now — keep the server-side read cursor in
                // sync so other clients (and the next restart) agree it's read.
                c.lastRead = msg.ts;
                _backend->markRead(conv, msg.ts);
                break;
            }
            const bool     isDm        = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
            const QString &mt          = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
            const bool     isMention   = mrkdwnMentions(mt, _meUserId);
            // A thread I muted: its replies stop badging (Slack's "Mute thread"),
            // except an explicit @mention, which always gets through.
            const bool     threadMuted = msg.threadRoot && isThreadMuted(conv, *msg.threadRoot);
            // A reply to a thread we started badges like a mention, matching
            // Slack's "replies to threads you're following" (see maybeNotify) —
            // unless that thread is muted.
            const bool     isFollowed  = !threadMuted && isFollowedThreadReply(msg, _meUserId);
            // Suppress a muted thread's non-mention replies entirely (even in a
            // DM/MPDM). Otherwise plain channel thread replies don't mark the
            // channel unread (they live in the Threads view); mentions,
            // followed-thread replies, and DM replies still count.
            if (msg.threadRoot && !isMention && !isFollowed && (threadMuted || !isDm))
                break;
            if (!c.isMuted) {
                c.unread++;
                if (isDm || isMention || isFollowed)
                    c.mentionCount++;
            } else if (!isDm && (isMention || isFollowed)) {
                // Muted channels stay quiet except for explicit mentions and
                // followed-thread replies, which still badge (official-client
                // behavior).
                c.unread++;
                c.mentionCount++;
            }
            break;
        }
        _conversations = std::move(convs);
    }
    // Remove the matching optimistic copy so the real message replaces it
    // instead of appearing as a duplicate. Match within the same kind (file
    // message vs plain text) so a quick text confirmation can't displace the
    // ghost of a still-uploading file batch.
    if (ownMessage) {
        auto it = _pendingSends.find(conv.value);
        if (it != _pendingSends.end() && !it->isEmpty()) {
            const bool withFiles = !msg.files.empty();
            int        idx       = 0;
            while (idx < it->size() && (*it)[idx].withFiles != withFiles)
                ++idx;
            if (idx == it->size())
                idx = 0; // no same-kind entry; fall back to FIFO
            const QString fakeTs = it->takeAt(idx).ts;
            _eventHub.fire(EvMessageDeleted{conv, fakeTs});
        }
    }
    return true;
}

void Session::checkRealtimeHealth() {
    // (1) Subscription intact: reconnect the shared realtime socket if it has
    // dropped (a no-op while healthy). Covers a reconnect that stalled past the
    // socket's own liveness watchdog.
    _backend->verifyRealtime();

    // (2) New messages: the watchdog can't detect a socket that still answers
    // pings but to which Slack has quietly stopped routing events — so a message
    // sent from another client silently never arrives. The socket is shared by
    // every workspace, so the stall hits ALL of them at once; detecting it
    // anywhere and reestablishing the socket heals realtime for the whole app.
    //
    // Foreground (a conversation is open): poll it every tick. Gate on _openConv,
    // NOT _readingConv, so an open-but-unfocused chat keeps being polled — a blur
    // clears _readingConv to stop marking-read, but a reply to the visible chat
    // must still arrive (when unfocused the injected message flows through
    // handleNewMessage's not-reading branch: it badges and notifies).
    if (!_openConv.value.isEmpty()) {
        // Throttle to Slack's conversations.history budget (1 req/min for
        // non-Marketplace apps). Polling on every 15 s tick was 4x over and, with no
        // cooldown guard, queued duplicate calls that all 429'd — a self-inflicted
        // rate-limit storm even while the app sat idle on an open chat. The socket
        // delivers in realtime; this is only the active chat's stalled-socket
        // backstop, so once a minute is ample.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - _lastForegroundPollMs >= kForegroundPollGapMs) {
            _lastForegroundPollMs = now;
            pollConversationForMissed(_openConv, /*foreground=*/true);
        }
    }

    // Background rotation: sweeps member conversations OTHER than the open one
    // (if any) on a slower cadence. This used to run only when nothing was open
    // at all, on the theory that the foreground poll already covers the active
    // chat — but the foreground poll only ever covers _openConv, so every OTHER
    // channel went completely unchecked for as long as the app had anything
    // open, which is most of the time it runs. A stalled shared socket drops
    // events in every channel at once, not just the visible one, so those other
    // channels need their own (slower) backstop too. Rotating through every
    // member conversation instead of always re-picking the single busiest one
    // guarantees each eventually gets checked rather than only ever the top pick.
    // A hit reestablishes the shared socket, restoring realtime for every
    // workspace; the message itself badges and fires a notification.
    const qint64 now2 = QDateTime::currentMSecsSinceEpoch();
    if (now2 - _lastBackgroundPollMs < kBackgroundPollGapMs)
        return;
    _lastBackgroundPollMs   = now2;
    const ConversationId bg = nextBackgroundPollTarget(_openConv);
    if (!bg.value.isEmpty())
        pollConversationForMissed(bg, /*foreground=*/false);
}

ConversationId Session::nextBackgroundPollTarget(const ConversationId &exclude) {
    // Members only (we can't fetch history for channels we're not in), and a
    // latestTs is required as the "new since" baseline. Sort most-recently-
    // active first — the likeliest place a reply lands — but advance a cursor
    // through the full list rather than always returning index 0, so a quiet
    // channel eventually gets its turn instead of never being checked at all.
    std::vector<const Conversation *> candidates;
    for (const auto &c : _conversations.current())
        if (c.isMember && !c.latestTs.isEmpty() && c.id != exclude)
            candidates.push_back(&c);
    if (candidates.empty())
        return {};
    std::sort(
        candidates.begin(), candidates.end(), [](const Conversation *a, const Conversation *b) {
            return a->latestTs > b->latestTs;
        }
    );
    _backgroundPollIdx %= static_cast<int>(candidates.size());
    return candidates[_backgroundPollIdx++]->id;
}

void Session::pollConversationForMissed(ConversationId conv, bool foreground) {
    // Poll a conversation's head history and inject anything newer than what we've
    // seen through the normal new-message path (dedup, unread/cursor bookkeeping,
    // mark-read). firstSighting makes this idempotent against messages the
    // realtime stream already delivered. Deletion detection runs foreground only —
    // it needs the per-conversation snapshot that only the open chat maintains.
    Ts lastKnown;
    if (const Conversation *c = findConversation(conv))
        lastKnown = c->latestTs;
    if (lastKnown.isEmpty())
        return; // no baseline to detect "new" against — the open load covers it

    _backend->loadHistory(conv, std::nullopt) |
        rpl::on_next(
            [this, conv, lastKnown, foreground](MessagePage page) {
                // Bail if intent changed while the fetch was in flight: a
                // foreground poll is stale once the open conversation moves on;
                // a background poll yields only if THIS exact conversation
                // became the open one mid-flight (the foreground path now
                // covers it, faster) — other conversations opening/closing
                // don't affect a background poll targeting a different one.
                if (foreground ? (_openConv != conv) : (_openConv == conv))
                    return;
                bool missed = false;
                // page.messages is oldest-first; inject in order so rows append
                // chronologically. The ts pre-filter keeps the common (nothing
                // missed) case from running handleNewMessage over the whole page.
                for (const auto &msg : page.messages) {
                    if (msg.ts <= lastKnown)
                        continue;
                    if (handleNewMessage(conv, msg)) {
                        _eventHub.fire(EvMessageNew{conv, msg});
                        missed = true; // realtime should have delivered this
                    }
                }
                // Detect deletions the realtime stream silently dropped. This poll
                // always fetches the head (no-cursor) page, which is authoritative
                // for the channel head: anything in the previous poll's snapshot
                // that is now gone and is NOT older than this page's oldest message
                // was deleted from another client. There is no upper bound — the
                // common case is deleting the most recent message, whose ts is
                // newer than everything that remains. The lower bound spares a
                // message that merely fell below the page as newer traffic pushed
                // the window up. The snapshot is from a prior completed poll (never
                // concurrent with realtime), so this comparison is race-free.
                // firstSighting doesn't dedup deletions, but the UI's
                // EvMessageDeleted handler is a no-op once the row is gone, so a
                // later realtime echo of the same delete is harmless.
                if (foreground && !page.messages.empty()) {
                    QSet<QString> nowTs;
                    nowTs.reserve(static_cast<int>(page.messages.size()));
                    qint64 oldest = page.messages.front().date;
                    for (const auto &m : page.messages) {
                        nowTs.insert(m.ts);
                        oldest = std::min(oldest, m.date);
                    }
                    if (_pollSnapshotConv == conv) {
                        for (auto it = _pollSnapshotTs.constBegin();
                             it != _pollSnapshotTs.constEnd();
                             ++it) {
                            const qint64 d = decimalTsToMicros(it.key());
                            if (d < oldest || nowTs.contains(it.key()))
                                continue;
                            std::optional<Ts> root;
                            if (!it.value().isEmpty())
                                root = it.value();
                            _eventHub.fire(EvMessageDeleted{conv, it.key(), root});
                            missed = true; // realtime should have delivered this
                        }
                    }
                    // Record this poll's window as the next baseline.
                    _pollSnapshotConv = conv;
                    _pollSnapshotTs.clear();
                    _pollSnapshotTs.reserve(static_cast<int>(page.messages.size()));
                    for (const auto &m : page.messages)
                        _pollSnapshotTs.insert(m.ts, m.threadRoot ? *m.threadRoot : QString());
                }

                // The realtime stream dropped a message it should have pushed:
                // the subscription is compromised. Re-establish the socket so
                // future events (and other conversations' badges) flow again.
                // Throttle: a persistently sick socket would otherwise have every
                // 15 s poll force a reconnect, each spawning a conversations.list
                // reload — exactly the 429 storm we're avoiding. Re-establish at
                // most once per window; the poll itself keeps recovering messages
                // meanwhile.
                if (missed) {
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - _lastReestablishMs >= kReestablishGapMs) {
                        _lastReestablishMs = now;
                        qWarning() << "Session: realtime missed messages in" << conv.value
                                   << "— re-establishing socket";
                        _backend->reestablishRealtime();
                    }
                }
            },
            _lifetime
        );
}

QString Session::teamUrl() const {
    return _backend ? _backend->teamUrl() : QString();
}

void Session::fetchMe() {
    _backend->loadMe() | rpl::on_next(
                             [this](UserId id) {
                                 if (id.value.isEmpty())
                                     return;
                                 setMe(std::move(id));
                                 // Persist so the next run knows the id before
                                 // (or without) auth.test answering.
                                 _cache->saveMeUserId(_meUserId);
                                 // Users may already be loaded (from cache); pick up admin flag
                                 // immediately.
                                 if (const User *u = findUser(_meUserId))
                                     _meIsAdmin = u->isAdmin;
                             },
                             _lifetime
                         );
}

void Session::reloadConversations(bool refreshEmoji) {
    _backend->loadConversations() |
        rpl::on_next(
            [this, refreshEmoji](std::vector<Conversation> convs) {
                // Preserve locally-incremented unread/mention counts
                // accumulated since startup (API returns 0 for
                // channels).
                const auto &prev = _conversations.current();
                for (auto &c : convs) {
                    for (const auto &old : prev) {
                        if (old.id != c.id)
                            continue;
                        c.unread       = std::max(c.unread, old.unread);
                        c.mentionCount = std::max(c.mentionCount, old.mentionCount);
                        // Preserve locally-applied star state: the
                        // API may not echo is_starred immediately
                        // after stars.add/stars.remove.
                        if (old.isStarred != c.isStarred && old.isStarred)
                            c.isStarred = old.isStarred;
                        // Preserve locally-set notification level
                        // (no public read API for per-channel prefs).
                        if (old.notifLevel != NotificationLevel::Default &&
                            c.notifLevel == NotificationLevel::Default)
                            c.notifLevel = old.notifLevel;
                        // Mute state also lives in client prefs the public
                        // API can't read — local state wins.
                        if (old.isMuted)
                            c.isMuted = true;
                        // The local "mute this person" switch is ours alone; the
                        // server never echoes it, so carry it forward.
                        if (old.locallyMuted)
                            c.locallyMuted = true;
                        // last_read / latest were dropped from conversations.list
                        // responses; keep the newest value we know (cached from a
                        // previous run's activity sweep or realtime events).
                        if (old.lastRead > c.lastRead)
                            c.lastRead = old.lastRead;
                        if (old.latestTs > c.latestTs)
                            c.latestTs = old.latestTs;
                        // conversations.list carries no `room`, so it can't
                        // report live huddles — keep whatever the realtime
                        // huddle_thread events detected (start/end is tracked
                        // there, not via this reload).
                        if (old.huddleActive && !c.huddleActive) {
                            c.huddleActive       = old.huddleActive;
                            c.huddleLink         = old.huddleLink;
                            c.huddleParticipants = old.huddleParticipants;
                        }
                        break;
                    }
                }
                // Before the sweeps consult _deadConvIds: drop persisted dead
                // marks this fresh list contradicts (invited/unarchived while
                // the app was closed).
                reconcileDeadConvIds(convs);
                _cache->saveConversations(convs);
                _conversations = std::move(convs);
                enrichDmActivity();
                fetchMissingDmUsers();
                if (!refreshEmoji)
                    return;
                // Emoji load is deferred to here so it doesn't queue ahead of
                // conversations/users. Cache serves emojis until the refresh
                // arrives and the result is written back to cache.
                _backend->loadEmojiList() | rpl::on_next(
                                                [this](QHash<QString, QString> map) {
                                                    _emojiMap = std::move(map);
                                                    _cache->saveEmojiMap(_emojiMap);
                                                    _emojiMapLoadedHub.fire({});
                                                },
                                                _lifetime
                                            );
            },
            _lifetime
        );
}

void Session::fetchJoinedConversation(ConversationId id) {
    // conversations.info reports the freshly-joined channel with is_member=true.
    // Fold it into the list (or flip an already-present preview to member),
    // preserving nothing else since this is a channel we weren't tracking.
    // We just joined it, so it's no longer "dead" — drop any stale mark so the
    // fetch (and future message fetches) go through.
    markConvAlive(id);
    _backend->loadConversationInfo(id) |
        rpl::on_next(
            [this](Conversation conv) {
                if (conv.notFound || conv.id.value.isEmpty())
                    return;
                auto       convs = _conversations.current();
                const auto it =
                    std::find_if(convs.begin(), convs.end(), [&](const Conversation &c) {
                        return c.id == conv.id;
                    });
                if (it != convs.end()) {
                    if (it->isMember)
                        return; // a concurrent fetch already added it
                    *it = std::move(conv);
                } else {
                    convs.push_back(std::move(conv));
                }
                _cache->saveConversations(convs);
                _conversations = std::move(convs);
            },
            _lifetime
        );
}

void Session::fetchUnknownConversation(ConversationId id, Message msg) {
    // Queue the message; a present key means a fetch is already in flight, so
    // every message that lands before conversations.info returns rides the same
    // fetch and drains together (a new MPDM's opening burst is often several
    // messages back-to-back).
    // Known not to exist for this workspace (a previous fetch got
    // channel_not_found) — a different workspace's conversation off the shared
    // socket, or a dead conv. Drop the message without another conversations.info
    // call; a busy foreign conversation would otherwise re-fetch on every
    // message. Cleared by markConvAlive if we ever legitimately join it.
    if (_deadConvIds.contains(id.value))
        return;

    const bool alreadyFetching = _unknownConvBacklog.contains(id.value);
    _unknownConvBacklog[id.value].append(std::move(msg));
    if (alreadyFetching)
        return;

    _backend->loadConversationInfo(id) |
        rpl::on_next_done(
            [this, id](Conversation info) {
                // A different workspace's event off the shared socket, or a dead
                // conversation: loadConversationInfo answers channel_not_found
                // with a not-found sentinel. Remember it so future messages skip
                // the fetch (above); on_done still drops the backlog. A bare
                // completion (transient failure) leaves the id fetchable so a
                // later message retries.
                if (info.notFound) {
                    markConvDead(id);
                    return;
                }
                if (info.id.value.isEmpty())
                    return;
                // Let the replayed messages below do the authoritative
                // unread/mention counting through handleNewMessage — the same
                // invariant as every other conversation. conversations.info's
                // unread_count already includes these messages, so keeping it
                // and then replaying would double-count.
                info.unread       = 0;
                info.mentionCount = 0;

                auto       convs = _conversations.current();
                const auto it =
                    std::find_if(convs.begin(), convs.end(), [&](const Conversation &c) {
                        return c.id == info.id;
                    });
                if (it != convs.end())
                    *it = info; // a concurrent path added it first — refresh in place
                else
                    convs.push_back(info);
                _cache->saveConversations(convs);
                _conversations = std::move(convs);

                // Replay through the normal path now that the conversation
                // exists: handleNewMessage badges/marks-read and de-dups, then
                // forward each to the UI so it renders and notifies.
                const auto backlog = _unknownConvBacklog.value(id.value);
                for (const auto &m : backlog) {
                    if (handleNewMessage(id, m))
                        _eventHub.fire(EvMessageNew{id, m});
                }
            },
            [this, id] {
                // Always clear the in-flight marker + backlog, success or
                // failure, so a later message can trigger a fresh fetch.
                _unknownConvBacklog.remove(id.value);
            },
            _lifetime
        );
}

void Session::markConvAlive(const ConversationId &id) {
    if (_deadConvIds.remove(id.value))
        scheduleSaveDeadConvIds();
}

void Session::markConvDead(const ConversationId &id) {
    _deadConvIds.insert(id.value);
    scheduleSaveDeadConvIds();
}

void Session::scheduleSaveDeadConvIds() {
    // ~1 s debounce: a sweep's burst of channel_not_found answers collapses
    // into one meta write.
    _saveDeadConvsTimer.start(1000);
}

void Session::reconcileDeadConvIds(const std::vector<Conversation> &convs) {
    if (_deadConvIds.isEmpty())
        return;
    bool changed = false;
    for (const auto &c : convs) {
        if (!_deadConvIds.contains(c.id.value))
            continue;
        // A listed channel/MPDM exists by definition — we were invited or it was
        // unarchived while the app was closed. A 1:1 DM is listed forever even
        // when conversations.info answers channel_not_found (dead peer), so it
        // clears only when its peer is loaded and provably active; the
        // EvUserChanged handler catches a live reactivation.
        if (c.kind == ConvKind::Im && (!c.dmUser || !findUser(*c.dmUser) || isDeadDm(c)))
            continue;
        _deadConvIds.remove(c.id.value);
        changed = true;
    }
    if (changed)
        scheduleSaveDeadConvIds();
}

bool Session::isDeadDm(const Conversation &c) const {
    // conversations.list never prunes a DM: once a D… channel exists it lives on
    // the account forever, so it keeps returning DMs whose peer was deactivated or
    // left the org. conversations.info answers channel_not_found for those, so the
    // sweeps skip them — mirroring the liveness filter ConvListWidget already applies
    // before showing a DM. (MPDMs have no single peer, so they always sweep.)
    if (c.kind != ConvKind::Im || !c.dmUser)
        return false;
    const User *u = findUser(*c.dmUser);
    if (!u)
        return false; // user info not loaded yet — let it through
    return u->isDeactivated || u->displayName == QLatin1String("deactivateduser") ||
           isUnresolvedUserId(u->displayName);
}

std::vector<ConversationId> Session::dmSweepTargets() const {
    // Unanalyzed DMs/MPDMs start hidden in the conversation list and pop in as
    // their info arrives, so sweep 1:1 DMs first — they matter more — and the
    // lower-priority MPDMs after.
    std::vector<ConversationId> targets;
    for (const auto &c : _conversations.current())
        if (c.kind == ConvKind::Im && !isDeadDm(c) && !_deadConvIds.contains(c.id.value))
            targets.push_back(c.id);
    for (const auto &c : _conversations.current())
        if (c.kind == ConvKind::Mpim && !_deadConvIds.contains(c.id.value))
            targets.push_back(c.id);
    return targets;
}

void Session::resyncUnreads() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - _lastUnreadResyncMs < kUnreadResyncGapMs)
        return;
    // A previous sweep is still trickling out on the paced background lane — don't
    // stack a second one that would just re-enqueue the same DMs behind it.
    if (_unreadResyncInFlight > 0)
        return;
    const auto targets = dmSweepTargets();
    if (targets.empty())
        return;
    _lastUnreadResyncMs   = now;
    _unreadResyncInFlight = int(targets.size());

    qDebug() << "[UnreadResync] recovering unread for" << targets.size() << "DMs/MPDMs";
    for (const auto &id : targets) {
        _backend->loadConversationInfo(id, /*background=*/true) |
            rpl::on_next_done(
                [this, id](Conversation info) {
                    // Dead DM/MPDM (peer gone, or
                    // another workspace's conv):
                    // remember it so later sweeps
                    // skip it (dmSweepTargets).
                    if (info.notFound) {
                        markConvDead(id);
                        return;
                    }
                    // Batch: merging each reply on
                    // its own reassigned the whole
                    // _conversations vector — one
                    // full conv-list rebuild per DM
                    // in the sweep.
                    _pendingUnreadInfos.emplace_back(id, std::move(info));
                    if (!_unreadPatchTimer.isActive())
                        _unreadPatchTimer.start(300);
                },
                [this] {
                    // Fires on success and error alike — one settled call. When the
                    // batch empties, a later reconnect may start a fresh sweep.
                    if (_unreadResyncInFlight > 0)
                        --_unreadResyncInFlight;
                },
                _lifetime
            );
    }
}

void Session::applyPendingUnreadInfos() {
    _unreadPatchTimer.stop();
    if (_pendingUnreadInfos.empty())
        return;
    auto convs   = _conversations.current();
    bool changed = false;
    for (const auto &[id, info] : _pendingUnreadInfos) {
        for (auto &c : convs) {
            if (c.id != id)
                continue;
            // Cursors (drive list relevance), same as the activity sweep.
            if (info.lastRead > c.lastRead) {
                c.lastRead = info.lastRead;
                changed    = true;
            }
            if (info.latestTs > c.latestTs) {
                c.latestTs = info.latestTs;
                changed    = true;
            }
            // Recover the badge the dropped realtime events never raised.
            // conversations.info reports the authed user's own unread count;
            // for a DM/MPDM every unread is a red-badge "mention"
            // (ConvListWidget reads conv.unread for these), mirroring
            // handleNewMessage. Merge UPWARD only so an unread we already
            // counted locally is never lowered by a momentarily stale server
            // response, and so reading the chat elsewhere (which advances
            // last_read → server unread 0) doesn't fight a reload that ran in
            // the other order.
            if (info.unread > c.unread) {
                c.unread = info.unread;
                changed  = true;
            }
            if (info.unread > c.mentionCount) {
                c.mentionCount = info.unread;
                changed        = true;
            }
            break;
        }
    }
    _pendingUnreadInfos.clear();
    if (changed) {
        _conversations = std::move(convs);
        scheduleSaveUnreads();
    }
}

void Session::enrichDmActivity() {
    // conversations.list no longer returns last_read / latest, so without extra
    // calls we cannot tell how old a DM or MPDM is. Fetch conversations.info for
    // each of them on the backend's low-priority queue, merge only the activity
    // fields, and persist — so the cost is one sweep per kActivitySweepGapSecs
    // across restarts. Realtime events keep latestTs fresh in between; channels
    // are excluded since they get visit/unread stamps through normal use.
    constexpr qint64 kActivitySweepGapSecs = qint64(12) * 3600;
    const qint64     now                   = QDateTime::currentSecsSinceEpoch();
    if (now - _cache->loadActivitySweepAt() < kActivitySweepGapSecs)
        return;

    const auto targets = dmSweepTargets();
    if (targets.empty())
        return;

    qDebug() << "[ActivitySweep] fetching conversations.info for" << targets.size() << "DMs/MPDMs";
    auto remaining = std::make_shared<int>(int(targets.size()));
    for (const auto &id : targets) {
        _backend->loadConversationInfo(id, /*background=*/true) |
            rpl::on_next_done(
                [this, id](Conversation info) {
                    // Dead DM/MPDM (peer gone, or another workspace's conv):
                    // remember it so later sweeps skip it (dmSweepTargets).
                    if (info.notFound) {
                        markConvDead(id);
                        return;
                    }
                    // Merge only the activity cursors. The info response lacks
                    // fields the list provides (e.g. MPDM members), and its
                    // unread fallback could fabricate badges — leave the rest
                    // of the local state alone.
                    auto convs = _conversations.current();
                    for (auto &c : convs) {
                        if (c.id != id)
                            continue;
                        if (info.lastRead > c.lastRead)
                            c.lastRead = info.lastRead;
                        if (info.latestTs > c.latestTs)
                            c.latestTs = info.latestTs;
                        break;
                    }
                    _conversations = std::move(convs);
                },
                [this, now, remaining] {
                    // Fires on success and error alike — the sweep is complete
                    // when every call has settled.
                    if (--*remaining > 0)
                        return;
                    _cache->saveConversations(_conversations.current());
                    _cache->saveActivitySweepAt(now);
                    qDebug() << "[ActivitySweep] done";
                },
                _lifetime
            );
    }
}

rpl::producer<std::vector<Conversation>> Session::conversations() const {
    return _conversations.value();
}

rpl::producer<std::vector<User>> Session::users() const {
    return _users.value();
}

rpl::producer<Event> Session::events() const {
    return _eventHub.events();
}

rpl::producer<AuthState> Session::authState() const {
    return _backend->authState();
}

const User *Session::findUser(UserId id) const {
    for (const auto &u : _users.current()) {
        if (u.id == id)
            return &u;
    }
    auto it = _botUsers.constFind(id.value);
    if (it != _botUsers.constEnd())
        return &*it;
    return nullptr;
}

void Session::fetchBotIfNeeded(UserId botId) {
    if (botId.value.isEmpty() || !_backend->isBotId(botId))
        return;
    if (findUser(botId))
        return;
    if (_pendingBotFetches.contains(botId.value))
        return;
    _pendingBotFetches.insert(botId.value);
    _backend->loadBotInfo(botId) | rpl::on_next(
                                       [this, botId](User u) {
                                           _pendingBotFetches.remove(botId.value);
                                           if (!u.id.value.isEmpty()) {
                                               _botUsers[u.id.value] = std::move(u);
                                               _cache->saveBots(_botUsers);
                                               _botInfoHub.fire_copy(botId);
                                           }
                                       },
                                       _lifetime
                                   );
}

rpl::producer<UserId> Session::botInfoLoaded() const {
    return _botInfoHub.events();
}

void Session::fetchUserIfNeeded(UserId userId) {
    if (userId.value.isEmpty() || !_backend->isUserId(userId))
        return;
    if (findUser(userId))
        return;
    if (_pendingUserFetches.contains(userId.value))
        return;
    _pendingUserFetches.insert(userId.value);
    _backend->loadUser(userId) | rpl::on_next(
                                     [this, userId](User u) {
                                         _pendingUserFetches.remove(userId.value);
                                         if (u.id.value.isEmpty())
                                             return;
                                         // Append to the live user list so the conv
                                         // list (fed by users()) re-resolves the name
                                         // + avatar, and persist: users.list never
                                         // returns these (external/system peers), so
                                         // the cache is the only thing that keeps
                                         // their names across a restart.
                                         auto users = _users.current();
                                         for (const auto &existing : users)
                                             if (existing.id == u.id)
                                                 return;
                                         const UserId resolved = u.id;
                                         users.push_back(std::move(u));
                                         _users = std::move(users);
                                         scheduleSaveUsers();
                                         // Tell the message list a previously-raw
                                         // id now has a name + avatar so it can
                                         // re-render the author header and any
                                         // baked-in @mentions of this user.
                                         _userInfoHub.fire_copy(resolved);
                                     },
                                     _lifetime
                                 );
}

rpl::producer<UserId> Session::userInfoLoaded() const {
    return _userInfoHub.events();
}

void Session::fetchMissingDmUsers() {
    // users.list not loaded yet → every peer would look "missing". The users
    // load handler calls us again once the full list has arrived.
    if (_users.current().empty())
        return;
    for (const auto &c : _conversations.current()) {
        if (c.kind == ConvKind::Im && c.dmUser)
            fetchUserIfNeeded(*c.dmUser);
        // Multi-person DMs name themselves from their members; resolve any that
        // users.list omitted so the title shows names, not raw ids.
        if (c.kind == ConvKind::Mpim)
            for (const auto &uid : c.members)
                fetchUserIfNeeded(uid);
    }
}

QString Session::userDisplayName(UserId id) {
    if (id.value.isEmpty())
        return {};
    if (const User *u = findUser(id)) {
        const QString label = u->displayLabel();
        // A cached entry whose label is itself a raw id (e.g. a deactivated
        // account that fell back to the bare id) is treated as unresolved.
        if (!label.isEmpty() && !_backend->isUnresolvedUserId(label))
            return label;
    }
    fetchUserIfNeeded(id);
    return QCoreApplication::translate("Session", "Unknown user");
}

const Conversation *Session::findConversation(ConversationId id) const {
    for (const auto &c : _conversations.current()) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

bool Session::isAppConversation(const Conversation &c) const {
    if (c.kind != ConvKind::Im || !c.dmUser)
        return false;
    // System accounts may report is_bot=false, so the flag check misses them.
    if (_backend->isSyntheticUser(*c.dmUser))
        return true;
    const User *u = findUser(*c.dmUser);
    return u && u->isBot;
}

Capabilities Session::capabilities() const {
    return _backend->capabilities();
}

bool Session::isSyntheticUser(UserId id) const {
    return _backend->isSyntheticUser(id);
}

bool Session::isUnresolvedUserId(const QString &text) const {
    return _backend->isUnresolvedUserId(text);
}

const std::vector<User> &Session::currentUsers() const {
    return _users.current();
}

const std::vector<Conversation> &Session::currentConversations() const {
    return _conversations.current();
}

static Ts makeFakeTs() {
    // Monotonic: two sends within the same millisecond must not collide —
    // optimistic messages are looked up and removed by this ts.
    static qint64 lastUsec = 0;
    qint64        usec     = QDateTime::currentMSecsSinceEpoch() * 1000;
    if (usec <= lastUsec)
        usec = lastUsec + 1;
    lastUsec = usec;
    return QString("%1.%2").arg(usec / 1000000).arg(usec % 1000000, 6, 10, QChar('0'));
}

bool Session::channelsAreLabels() const {
    return _backend && _backend->channelsAreLabels();
}

void Session::labelMessage(
    ConversationId                     sourceConv,
    Ts                                 ts,
    ConversationId                     targetChannel,
    std::function<void(bool, QString)> done
) {
    if (_backend)
        _backend->labelMessage(sourceConv, ts, targetChannel, std::move(done));
}

void Session::sendMessage(
    ConversationId conv, const QString &text, std::optional<Ts> threadRoot, const QString &subject
) {
    const Ts fakeTs = makeFakeTs();

    Message optimistic;
    optimistic.ts         = fakeTs;
    optimistic.date       = decimalTsToMicros(fakeTs); // so it sorts/renders like a real msg
    optimistic.author     = _meUserId;
    optimistic.text       = MrkdwnParser::parse(text);
    optimistic.rawText    = text;
    optimistic.threadRoot = threadRoot;
    optimistic.pending    = true;

    _eventHub.fire(EvMessageNew{conv, optimistic});

    _pendingSends[conv.value].append({fakeTs, false});

    OutgoingMessage out;
    out.text       = optimistic.text;
    out.rawText    = text;
    out.threadRoot = threadRoot;
    out.subject    = subject;
    // Anchor for the backend's lost-send reconciliation: only messages newer
    // than this server ts can be the one we are about to post.
    if (const Conversation *c = findConversation(conv))
        out.sinceTs = c->latestTs;
    _backend->sendMessage(conv, std::move(out));
}

bool Session::firstSighting(const ConversationId &conv, const Ts &ts) {
    const QString key = conv.value + '|' + ts;
    if (_seenMsgKeys.contains(key))
        return false;
    _seenMsgKeys.insert(key);
    _seenMsgOrder.enqueue(key);
    while (_seenMsgOrder.size() > 512)
        _seenMsgKeys.remove(_seenMsgOrder.dequeue());
    return true;
}

Backend *Session::backend() const {
    return _backend.get();
}

void Session::editMessage(ConversationId conv, Ts ts, const QString &newText) {
    _backend->editMessage(conv, ts, TextWithEntities{newText, {}});
}

void Session::sendTyping(ConversationId conv) {
    _backend->sendTyping(conv);
}

void Session::scheduleMessage(ConversationId conv, const QString &text, qint64 postAt) {
    OutgoingMessage out;
    out.text = MrkdwnParser::parse(text);
    _backend->scheduleMessage(conv, std::move(out), postAt);
}

const SlashCommand *Session::findCommand(const QString &name) const {
    for (const auto &c : _commands)
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return &c;
    return nullptr;
}

void Session::runCommand(ConversationId conv, const QString &name, const QString &args) {
    const QString cmd = name.toLower();

    // Native handlers — where local state or a documented public API can do
    // the job, don't depend on the undocumented chat.command endpoint (it
    // requires the legacy `post` scope and is often rejected for OAuth tokens).
    if (cmd == QLatin1String("shrug")) {
        const QString shrug = QStringLiteral("¯\\_(ツ)_/¯");
        sendMessage(conv, args.isEmpty() ? shrug : args + ' ' + shrug);
        return;
    }
    if (cmd == QLatin1String("msg") || cmd == QLatin1String("dm")) {
        const QString trimmed = args.trimmed();
        const int     sp      = trimmed.indexOf(' ');
        QString       uname   = sp < 0 ? trimmed : trimmed.left(sp);
        const QString rest    = sp < 0 ? QString() : trimmed.mid(sp + 1).trimmed();
        const User   *target  = nullptr;
        if (uname.startsWith(QLatin1String("<@")) && uname.endsWith('>')) {
            // Composer mention pills read back as raw <@U…> tokens.
            target = findUser(UserId{uname.mid(2, uname.size() - 3)});
        } else {
            if (uname.startsWith('@'))
                uname = uname.mid(1);
            for (const auto &u : currentUsers()) {
                if (u.name.compare(uname, Qt::CaseInsensitive) == 0 ||
                    u.displayName.compare(uname, Qt::CaseInsensitive) == 0) {
                    target = &u;
                    break;
                }
            }
        }
        if (!target) {
            _errorHub.fire(QCoreApplication::translate("Session", "No such user: %1").arg(uname));
            return;
        }
        openDm(target->id, [this, rest](ConversationId dm) {
            if (!rest.isEmpty())
                sendMessage(dm, rest);
        });
        return;
    }
    if (cmd == QLatin1String("leave")) {
        leaveConversation(conv);
        return;
    }
    if (cmd == QLatin1String("mute")) {
        const Conversation *c = findConversation(conv);
        setNotificationLevel(
            conv, (c && c->isMuted) ? NotificationLevel::Default : NotificationLevel::Mute
        );
        return;
    }
    if (cmd == QLatin1String("away")) {
        // Official-client semantics: /away toggles between away and auto.
        setPresence(!currentSelfPresence().manualAway);
        return;
    }
    if (cmd == QLatin1String("active")) {
        setPresence(false);
        return;
    }
    if (cmd == QLatin1String("status")) {
        const QString a = args.trimmed();
        if (a.isEmpty() || a.compare(QLatin1String("clear"), Qt::CaseInsensitive) == 0) {
            setStatus({}, {});
            return;
        }
        // Optional leading :emoji:, the rest is the status text.
        QString emoji, text = a;
        if (a.startsWith(':')) {
            const int end = a.indexOf(':', 1);
            if (end > 1) {
                emoji = a.left(end + 1);
                text  = a.mid(end + 1).trimmed();
            }
        }
        setStatus(emoji, text);
        return;
    }
    if (cmd == QLatin1String("dnd")) {
        const int minutes = parseDndMinutes(args);
        if (minutes < 0) {
            _errorHub.fire(
                QCoreApplication::translate(
                    "Session", "Usage: /dnd [duration, e.g. 30m or 2h] — or /dnd off to resume"
                )
            );
            return;
        }
        setDndSnooze(minutes);
        return;
    }

    _backend->runCommand(conv, '/' + cmd, args, [this, cmd](bool ok, QString message) {
        // Success responses (e.g. /who's inline member list) are posted by
        // Slack into the conversation where relevant; only failures need a
        // local surface.
        if (!ok)
            _errorHub.fire(
                QCoreApplication::translate("Session", "Command /%1 failed: %2").arg(cmd, message)
            );
    });
}

void Session::patchMeUser(const std::function<void(User &)> &fn) {
    if (_meUserId.value.isEmpty())
        return;
    auto users = _users.current();
    for (auto &u : users) {
        if (u.id == _meUserId) {
            fn(u);
            break;
        }
    }
    _users = std::move(users);
}

void Session::patchUserSilently(const UserId &id, const std::function<void(User &)> &fn) {
    // rpl::variable has no mutate-without-notify API; its current() hands out a
    // const ref to the live storage, which is safe to patch through as the
    // variable itself is not const.
    auto &users = const_cast<std::vector<User> &>(_users.current());
    for (auto &u : users) {
        if (u.id == id) {
            fn(u);
            break;
        }
    }
}

void Session::setPresence(bool away) {
    _backend->setPresence(away, [this, away](bool ok, QString err) {
        if (!ok) {
            _errorHub.fire(withReauthHint(
                QCoreApplication::translate("Session", "Could not change presence: %1").arg(err),
                err
            ));
            return;
        }
        patchUserSilently(_meUserId, [away](User &u) { u.isActive = !away; });
        if (!_meUserId.value.isEmpty())
            _eventHub.fire(EvPresenceChanged{_meUserId, !away});
        // The rich self-presence snapshot (manualAway etc.) only comes from the
        // server — re-poll instead of guessing.
        refreshSelfPresence();
    });
}

void Session::setStatus(const QString &emoji, const QString &text, qint64 expirationTs) {
    _backend->setStatus(emoji, text, expirationTs, [this, emoji, text](bool ok, QString err) {
        if (!ok) {
            _errorHub.fire(withReauthHint(
                QCoreApplication::translate("Session", "Could not set status: %1").arg(err), err
            ));
            return;
        }
        // User.statusEmoji stores the bare name (":palm_tree:" → "palm_tree").
        QString bare = emoji;
        if (bare.startsWith(':'))
            bare = bare.mid(1);
        if (bare.endsWith(':'))
            bare.chop(1);
        patchMeUser([&bare, &text](User &u) {
            u.statusEmoji = bare;
            u.statusText  = text;
        });
    });
}

void Session::setDndSnooze(int minutes) {
    _backend->setDndSnooze(minutes, [this, minutes](bool ok, QString err) {
        if (!ok) {
            _errorHub.fire(withReauthHint(
                QCoreApplication::translate("Session", "Could not update notifications: %1")
                    .arg(err),
                err
            ));
            return;
        }
        patchUserSilently(_meUserId, [minutes](User &u) { u.dndEnabled = minutes > 0; });
        if (!_meUserId.value.isEmpty())
            _eventHub.fire(EvDndChanged{_meUserId, minutes > 0});
    });
}

void Session::loadMyProfile(std::function<void(MyProfile)> done) {
    _backend->loadMyProfile(std::move(done));
}

void Session::updateProfile(
    const QHash<QString, QString> &fields, std::function<void(bool, QString)> done
) {
    _backend->updateProfile(fields, [this, fields, done](bool ok, QString err) {
        if (!ok) {
            _errorHub.fire(withReauthHint(
                QCoreApplication::translate("Session", "Could not update profile: %1").arg(err), err
            ));
            if (done)
                done(false, err);
            return;
        }
        // Reflect a changed display name in our own entry so the footer/conv
        // list update immediately (the footer shows display_name when set).
        if (fields.contains(QStringLiteral("display_name"))) {
            const QString dn = fields.value(QStringLiteral("display_name"));
            patchMeUser([&dn](User &u) { u.displayName = dn; });
        }
        if (done)
            done(true, {});
    });
}

void Session::setPhoto(const QString &filePath, std::function<void(bool, QString)> done) {
    _backend->setPhoto(filePath, [this, done](bool ok, QString err, QString newAvatarUrl) {
        if (!ok) {
            _errorHub.fire(withReauthHint(
                QCoreApplication::translate("Session", "Could not update avatar: %1").arg(err), err
            ));
            if (done)
                done(false, err);
            return;
        }
        if (!newAvatarUrl.isEmpty())
            patchMeUser([&newAvatarUrl](User &u) { u.avatarUrl = newAvatarUrl; });
        if (done)
            done(true, {});
    });
}

void Session::uploadFiles(ConversationId conv, const QStringList &filePaths, const QString &text) {
    // Uploads can take a while for big files — show the message immediately as
    // a translucent pending copy; the real message arrives via realtime once
    // Slack posts it, replacing the ghost (same flow as plain text sends).
    const Ts fakeTs = makeFakeTs();

    Message optimistic;
    optimistic.ts      = fakeTs;
    optimistic.date    = decimalTsToMicros(fakeTs); // so it sorts/renders like a real msg
    optimistic.author  = _meUserId;
    optimistic.text    = MrkdwnParser::parse(text);
    optimistic.rawText = text;
    optimistic.pending = true;

    QMimeDatabase mimeDb;
    for (const QString &path : filePaths) {
        const QFileInfo info(path);
        File            f;
        f.id         = QStringLiteral("pending:") + path;
        f.name       = info.fileName();
        f.mimeType   = mimeDb.mimeTypeForFile(path).name();
        f.prettyType = info.suffix().toUpper();
        f.size       = info.size();
        if (f.mimeType.startsWith("image/")) {
            // Local preview: point at the file itself; the message list loads
            // file:// URLs straight from disk instead of downloading.
            QImageReader reader(path);
            QSize        dim = reader.size();
            // size() ignores EXIF rotation but the pixmap loads auto-rotated —
            // transpose so the preview box matches what gets drawn.
            if (reader.transformation() & QImageIOHandler::TransformationRotate90)
                dim.transpose();
            if (dim.isValid()) {
                f.imageWidth  = dim.width();
                f.imageHeight = dim.height();
                f.urlPrivate  = QUrl::fromLocalFile(path).toString();
            }
        }
        optimistic.files.push_back(std::move(f));
    }

    _eventHub.fire(EvMessageNew{conv, optimistic});
    _pendingSends[conv.value].append({fakeTs, true});

    _backend->uploadFiles(conv, filePaths, text, [this, conv, fakeTs](bool ok, QString error) {
        if (ok)
            return; // realtime delivery of the real message removes the ghost
        auto it = _pendingSends.find(conv.value);
        if (it != _pendingSends.end())
            it->removeIf([&](const PendingSend &p) { return p.ts == fakeTs; });
        _eventHub.fire(EvMessageDeleted{conv, fakeTs});
        _errorHub.fire(QCoreApplication::translate("Session", "Upload failed: %1").arg(error));
    });
}

void Session::searchMessages(
    const QString &query, std::function<void(std::vector<SearchResult>)> callback
) {
    _backend->searchMessages(query) |
        rpl::on_next(
            [cb = std::move(callback)](std::vector<SearchResult> results) {
                cb(std::move(results));
            },
            _lifetime
        );
}

rpl::producer<QString> Session::errors() const {
    return _errorHub.events();
}

void Session::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    if (!onError) {
        onError = [this](const QString &err) { _errorHub.fire_copy(err); };
    }
    _backend->downloadFile(url, std::move(onData), std::move(onError));
}

void Session::loadChannelCanvas(ConversationId conv, std::function<void(QString, bool)> done) {
    _backend->loadChannelCanvas(conv, std::move(done));
}

void Session::loadCanvasContent(
    const QString                    &fileId,
    std::function<void(QString html)> onHtml,
    std::function<void(QString)>      onError
) {
    if (!onError) {
        onError = [this](const QString &err) {
            _errorHub.fire(
                QCoreApplication::translate("Session", "Could not load canvas: %1").arg(err)
            );
        };
    }
    _backend->loadCanvasContent(fileId, std::move(onHtml), std::move(onError));
}

void Session::loadCanvasImage(
    const QString                  &fileId,
    std::function<void(QByteArray)> onData,
    std::function<void(QString)>    onError
) {
    // A broken inline image shouldn't surface the error banner; default to
    // swallowing the error (the caller leaves a placeholder instead).
    _backend->loadCanvasImage(fileId, std::move(onData), std::move(onError));
}

void Session::createChannelCanvas(
    ConversationId                      conv,
    const QString                      &markdown,
    std::function<void(QString fileId)> onSuccess,
    std::function<void(QString)>        onError
) {
    // The banner always fires on failure; a caller-provided onError is
    // notified in addition (state cleanup), mirroring editCanvas().
    _backend->createChannelCanvas(
        conv,
        markdown,
        std::move(onSuccess),
        [this, onError = std::move(onError)](const QString &err) {
            _errorHub.fire(
                QCoreApplication::translate("Session", "Could not create canvas: %1").arg(err)
            );
            if (onError)
                onError(err);
        }
    );
}

void Session::editCanvas(
    const QString                            &canvasId,
    const std::vector<CanvasChange>          &changes,
    std::function<void(bool ok, QString err)> done
) {
    _backend->editCanvas(canvasId, changes, [this, done](bool ok, QString err) {
        if (!ok)
            _errorHub.fire(
                QCoreApplication::translate("Session", "Canvas edit failed: %1").arg(err)
            );
        if (done)
            done(ok, std::move(err));
    });
}

void Session::loadCanvasMeta(
    const QString                                                               &fileId,
    std::function<void(QString title, QString permalink, CanvasMetaState state)> done
) {
    _backend->loadCanvasMeta(fileId, std::move(done));
}

void Session::deleteCanvas(const QString &canvasId, std::function<void(bool ok)> done) {
    _backend->deleteCanvas(canvasId, [this, done](bool ok, QString err) {
        if (!ok)
            _errorHub.fire(
                QCoreApplication::translate("Session", "Canvas deletion failed: %1").arg(err)
            );
        if (done)
            done(ok);
    });
}

std::vector<Message> Session::cachedMessages(ConversationId conv) const {
    // A queued-but-unflushed write is newer than the file.
    const auto pending = _pendingMsgWrites.constFind(conv.value);
    if (pending != _pendingMsgWrites.constEnd())
        return pending.value();
    return _cache->loadMessages(conv);
}

void Session::cacheMessages(ConversationId conv, const std::vector<Message> &msgs) {
    // Pending optimistic copies must not survive a restart as ghost messages.
    std::vector<Message> confirmed;
    confirmed.reserve(msgs.size());
    for (const auto &m : msgs)
        if (!m.pending)
            confirmed.push_back(m);
    // Queue instead of writing: callers sit on the conversation-switch click
    // path, and the serialization + write is pure persistence — nothing reads
    // the file before flushPendingMsgWrites() runs (cachedMessages checks the
    // queue). Re-caching the same conversation just replaces its queued copy.
    _pendingMsgWrites.insert(conv.value, std::move(confirmed));
    _saveMsgsTimer.start(300);
}

void Session::flushPendingMsgWrites() {
    _saveMsgsTimer.stop();
    for (auto it = _pendingMsgWrites.constBegin(); it != _pendingMsgWrites.constEnd(); ++it)
        _cache->saveMessages(ConversationId{it.key()}, it.value());
    _pendingMsgWrites.clear();
}

void Session::saveLastConv(ConversationId conv, const QString &displayName) {
    _cache->saveLastConv(conv, displayName);
}

std::pair<ConversationId, QString> Session::loadLastConv() const {
    return _cache->loadLastConv();
}

void Session::cacheImage(const QString &url, const QByteArray &data) {
    _cache->saveImage(url, data);
}

QByteArray Session::cachedImage(const QString &url) const {
    return _cache->loadImage(url);
}

void Session::requestPresence(UserId userId) {
    // users.getPresence answers internal_error for any user with no observable
    // presence, and Slack returns the generic error rather than a clean code:
    //   - the Slack system accounts (fixed ids USLACKBOT = Slackbot, USLACK = the
    //     "Slack" workspace/billing notifier — both report is_bot=false, so the
    //     flag check alone misses them; see ConvListWidget::isAppConv);
    //   - bot/app users (isBot) and deactivated accounts;
    //   - users not in this workspace's users.list at all (external Slack Connect
    //     partners, some app IMs) — findUser() is null for them.
    // We only ever render a presence dot for a known human member, so unless we
    // hold such a User record, skip the doomed call instead of spamming retries.
    const User *u = findUser(userId);
    if (!u || u->isBot || u->isDeactivated || _backend->isSyntheticUser(userId))
        return;
    _backend->loadPresence(userId) |
        rpl::on_next(
            [this, userId](bool active) {
                // Patch the cached user silently; the event is
                // how listeners learn the new state.
                patchUserSilently(userId, [active](User &u) { u.isActive = active; });
                _eventHub.fire(EvPresenceChanged{userId, active});
            },
            _lifetime
        );
}

rpl::producer<SelfPresence> Session::selfPresence() const {
    return _selfPresence.value();
}

SelfPresence Session::currentSelfPresence() const {
    return _selfPresence.current();
}

void Session::refreshSelfPresence() {
    _backend->loadSelfPresence() |
        rpl::on_next(
            [this](SelfPresence sp) {
                _selfPresence  = sp;
                // Keep our own User.isActive (the others-see-me flag) in sync
                // and notify listeners the same way requestPresence() does.
                const User *me = findUser(_meUserId);
                if (me && me->isActive != sp.active) {
                    patchUserSilently(_meUserId, [active = sp.active](User &u) {
                        u.isActive = active;
                    });
                    _eventHub.fire(EvPresenceChanged{_meUserId, sp.active});
                }
            },
            _lifetime
        );
}

void Session::persistUnreads() {
    _saveUnreadsTimer.stop(); // an explicit flush subsumes any pending debounce
    _cache->saveConversations(_conversations.current());
}

void Session::scheduleSaveUnreads() {
    // ~1 s debounce: a flurry of switches/reads collapses into one write, and
    // the serialization never blocks the triggering interaction.
    _saveUnreadsTimer.start(1000);
}

void Session::scheduleSaveUsers() {
    // ~1 s debounce, same rationale as scheduleSaveUnreads() but for the roster
    // (see the declaration for the watchdog incident this prevents).
    _saveUsersTimer.start(1000);
}

void Session::setOpenConversation(ConversationId conv) {
    if (_openConv == conv)
        return;
    _openConv         = conv;
    // The deletion-detection baseline belongs to the conversation that was open;
    // it's meaningless once we switch (and a stale set would mis-fire). The next
    // poll rebuilds it for the new conversation.
    _pollSnapshotConv = {};
    _pollSnapshotTs.clear();
    // _lastForegroundPollMs is a single timestamp shared by every conversation's
    // foreground poll, not per-conversation — it exists to keep repeated 15 s
    // ticks against the SAME open chat under Slack's ~1/min conversations.history
    // budget. Carrying it across a switch instead starves the newly-opened chat:
    // its first poll has to wait out whatever's left of the previous chat's
    // cooldown, and a user hopping between a few conversations can keep pushing
    // that wait out indefinitely, so a chat can sit missing messages until a
    // restart. Reset it here so the next 15 s tick always checks the chat that's
    // actually open now; the 60 s throttle still applies tick-to-tick for it.
    _lastForegroundPollMs = 0;
}

void Session::setReading(ConversationId conv) {
    _readingConv = conv;
    // Reading a conversation means it is open. Keep the focus-independent
    // open-conversation cursor in sync so the safety poll covers it — but only on
    // a real open (non-empty id). A blur fires setReading({}) to stop marking
    // read; that must NOT clear _openConv, or the safety poll would go dark while
    // the window is in the background (the cause of replies to an open-but-
    // unfocused chat never arriving until a manual refetch).
    if (!conv.value.isEmpty())
        setOpenConversation(conv);
    if (conv.value.isEmpty())
        return;

    // Optimistically zero the badge.
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            const bool hadUnread = c.unread > 0;
            c.unread             = 0;
            c.mentionCount       = 0;
            // Sync the read cursor to the server so other clients (and the next
            // restart) agree this conversation is read. Fire when there was
            // anything unread (the reliable, backend-agnostic signal — captured
            // before the badge is zeroed above), OR the read cursor trails the
            // latest message. The latter is the ordered-ts shortcut: valid for
            // Slack/Teams epoch ts, a meaningless string compare for email
            // Message-IDs — kept only as a backstop for when the badge was already
            // cleared elsewhere but the cursor still lags. markRead is idempotent,
            // so a redundant call is a no-op.
            if (!c.latestTs.isEmpty() && (hadUnread || c.lastRead < c.latestTs)) {
                c.lastRead = c.latestTs;
                _backend->markRead(conv, c.latestTs);
            }
            break;
        }
    }
    _conversations = std::move(convs);
    // NOTE: we deliberately do NOT refresh huddle state from conversations.info
    // on open. Slack doesn't surface the live `room` to our token there, so the
    // response reports "no huddle" and would clobber the realtime-detected state
    // (the huddle_thread message — start, and its has_ended edit — end — is the
    // authoritative signal). See docs/HUDDLES_PLAN.md.
}

void Session::starConversation(ConversationId conv, bool star) {
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            c.isStarred = star;
            break;
        }
    }
    _conversations = std::move(convs);
    _backend->starConversation(conv, star);
}

void Session::leaveConversation(ConversationId conv) {
    _backend->leaveConversation(conv);
    // Optimistically remove from the local list so the UI updates immediately.
    auto convs = _conversations.current();
    convs.erase(
        std::remove_if(
            convs.begin(), convs.end(), [&](const Conversation &c) { return c.id == conv; }
        ),
        convs.end()
    );
    _conversations = std::move(convs);
}

void Session::createChannel(
    const QString                      &name,
    bool                                isPrivate,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    _backend->createChannel(
        name,
        isPrivate,
        [this, onSuccess](ConversationId id) {
            // Refresh conversation list so the new channel appears.
            _backend->loadConversations() |
                rpl::on_next(
                    [this](std::vector<Conversation> convs) { _conversations = std::move(convs); },
                    _lifetime
                );
            if (onSuccess)
                onSuccess(id);
        },
        [this, onError](QString err) {
            if (onError)
                onError(err);
            else
                _errorHub.fire_copy(err);
        }
    );
}

void Session::joinChannel(
    ConversationId                      id,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    _backend->joinChannel(
        id,
        [this, onSuccess](ConversationId convId) {
            markConvAlive(convId); // joined — no longer channel_not_found
            _backend->loadConversations() |
                rpl::on_next(
                    [this](std::vector<Conversation> convs) { _conversations = std::move(convs); },
                    _lifetime
                );
            if (onSuccess)
                onSuccess(convId);
        },
        [this, onError](QString err) {
            if (onError)
                onError(err);
            else
                _errorHub.fire_copy(err);
        }
    );
}

void Session::openDm(
    UserId user, std::function<void(ConversationId)> onSuccess, std::function<void(QString)> onError
) {
    for (const auto &c : _conversations.current()) {
        if (c.kind == ConvKind::Im && c.dmUser == user) {
            if (onSuccess)
                onSuccess(c.id);
            return;
        }
    }
    _backend->openDm(
        user,
        [this, user, onSuccess](ConversationId convId) {
            markConvAlive(convId); // DM opened — no longer channel_not_found
            // Insert a minimal conversation so the UI can select it right away;
            // the next conversations.list refresh fills in the rest.
            auto       convs = _conversations.current();
            const bool known = std::any_of(convs.begin(), convs.end(), [&](const Conversation &c) {
                return c.id == convId;
            });
            if (!known) {
                Conversation c;
                c.id       = convId;
                c.kind     = ConvKind::Im;
                c.name     = user.value;
                c.isMember = true;
                c.dmUser   = user;
                convs.push_back(std::move(c));
                _conversations = std::move(convs);
            }
            if (onSuccess)
                onSuccess(convId);
        },
        [this, onError](QString err) {
            if (onError)
                onError(err);
            else
                _errorHub.fire_copy(err);
        }
    );
}

void Session::setNotificationLevel(ConversationId conv, NotificationLevel level) {
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            c.notifLevel = level;
            c.isMuted    = (level == NotificationLevel::Mute);
            break;
        }
    }
    _conversations = std::move(convs);
}

void Session::setConvMuted(ConversationId conv, bool muted) {
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            c.locallyMuted = muted;
            break;
        }
    }
    _conversations = std::move(convs);
}

bool Session::isThreadMuted(const ConversationId &conv, const Ts &root) const {
    return _mutedThreads.contains(threadMuteKey(conv, root));
}

void Session::setThreadMuted(const ConversationId &conv, const Ts &root, bool muted) {
    const QString key = threadMuteKey(conv, root);
    if (muted == _mutedThreads.contains(key))
        return; // no change
    if (muted)
        _mutedThreads.insert(key);
    else
        _mutedThreads.remove(key);
    if (_cache)
        _cache->saveMutedThreads(QStringList(_mutedThreads.begin(), _mutedThreads.end()));
}
