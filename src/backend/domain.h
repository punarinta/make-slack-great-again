// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Normalized domain types — the ONLY types that cross the Backend seam upward.
// No Slack JSON, no HTTP types, no xoxp tokens above this layer.
#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <variant>
#include <vector>

// --- Notification freshness ---

// A notification is only worth raising while the thing it announces is still
// current. Events older than this window are silently dropped from EVERY
// notification surface — OS notification, sound, tray/dock icon tint, workspace
// counters, chat counters — so a long-offline start, a cache replay or a
// backfill sweep can't replay a month of history at the user. Tune here; this
// is the single source of truth (see tooOldToNotify below).
constexpr int kMaxNotifyAgeDays = 30;

// --- Identity types ---

struct WorkspaceId {
    QString value; // "T0123ABCD"
    bool    operator==(const WorkspaceId &) const = default;
};

struct ConversationId {
    QString value; // "C…" public, "G…" private/mpim, "D…" im
    bool    operator==(const ConversationId &) const = default;
};

struct UserId {
    QString value; // "U…"
    bool    operator==(const UserId &) const = default;
};

// Slack message timestamp — both identity and sort key ("1700000000.000100").
using Ts = QString;

// Derive epoch microseconds from a decimal "seconds.fraction" timestamp string
// (e.g. Slack's "1700000000.000100"). Fills Message::date from a Slack ts and
// backfills it for legacy cached messages that predate the field, so both paths
// agree to the microsecond. Transitional: once `ts` is treated as fully opaque
// (the planned Ts→MessageId step), backends produce `date` directly and the
// cache simply stores/loads it. Integer-parsed (not toDouble) to avoid precision
// loss on the 16-significant-digit value.
inline qint64 decimalTsToMicros(const QString &ts) {
    const int dot = ts.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return ts.toLongLong() * 1000000;
    const qint64 secs = ts.left(dot).toLongLong();
    QString      frac = ts.mid(dot + 1);
    frac.truncate(6);
    while (frac.size() < 6)
        frac.append(QLatin1Char('0'));
    return secs * 1000000 + frac.toLongLong();
}

// --- Services / workspace handle ---

// The messaging services this app can host. Only Slack today; Telegram/Teams/…
// are added here as backends land. Keep minimal.
enum class Service {
    Slack,
    Teams,
    Imap /*, Telegram, … */
#if defined(MSGA_DEMO)
    ,
    Demo // fixture-driven fake workspace (`--demo`), Debug builds only — see demo/README.md.
         // Every reference to it MUST sit under #if defined(MSGA_DEMO).
#endif
};

// Stable serialization token for a Service. NEVER serialize the enum's integer
// — reordering the enum later must not corrupt stored workspace handles.
inline QString serviceToken(Service s) {
    switch (s) {
    case Service::Slack:
        return QStringLiteral("slack");
    case Service::Teams:
        return QStringLiteral("teams");
    case Service::Imap:
        return QStringLiteral("imap");
#if defined(MSGA_DEMO)
    case Service::Demo:
        return QStringLiteral("demo");
#endif
    }
    return QStringLiteral("slack");
}
inline std::optional<Service> serviceFromToken(const QString &t) {
    if (t == QStringLiteral("slack"))
        return Service::Slack;
    if (t == QStringLiteral("teams"))
        return Service::Teams;
    if (t == QStringLiteral("imap"))
        return Service::Imap;
#if defined(MSGA_DEMO)
    if (t == QStringLiteral("demo"))
        return Service::Demo;
#endif
    return std::nullopt;
}

// Human-facing service name — shown in the add-workspace service picker.
inline QString serviceDisplayName(Service s) {
    switch (s) {
    case Service::Slack:
        return QStringLiteral("Slack");
    case Service::Teams:
        return QStringLiteral("Microsoft Teams");
    case Service::Imap:
        return QStringLiteral("Email (IMAP)");
#if defined(MSGA_DEMO)
    case Service::Demo:
        return QStringLiteral("Demo");
#endif
    }
    return QStringLiteral("Slack");
}

// App-wide workspace handle. Service ids are unique only *within* a service, so
// everything that keys a workspace (sessions map, cache dir, storage subtree,
// active marker) is keyed by (service, id) — not a bare id. Kept as explicit
// fields; only encoded to/from a string at the QSettings / cache-path boundary.
struct WorkspaceKey {
    Service service = Service::Slack;
    QString id; // service-local id, e.g. Slack team "T0123ABCD"
    bool    operator==(const WorkspaceKey &) const = default;

    // Canonical form "slack:T0123ABCD" (service token + ':' + id). ':' is a safe
    // delimiter: no service id format uses it (Slack ids are [A-Z0-9]).
    QString toString() const { return serviceToken(service) + QLatin1Char(':') + id; }

    static std::optional<WorkspaceKey> fromString(const QString &s) {
        const int i = s.indexOf(QLatin1Char(':'));
        if (i <= 0 || i + 1 >= s.size())
            return std::nullopt;
        const auto svc = serviceFromToken(s.left(i));
        if (!svc)
            return std::nullopt;
        return WorkspaceKey{*svc, s.mid(i + 1)};
    }
};

// --- Enumerations ---

enum class ConvKind { PublicChannel, PrivateChannel, Im, Mpim };
enum class AuthState { NotLoggedIn, LoggingIn, LoggedIn };
enum class NotificationLevel { Default, All, Mentions, Mute };

// What a backend supports. EVERY flag defaults false: a feature is opt-in, so a
// backend that forgets to set a flag silently *hides* the feature rather than
// claiming one it can't honor. The UI gates Slack-only affordances on these (see
// the canvas tab and huddle call sites) so a future Telegram/Teams backend that
// lacks them shows a clean surface with no dead controls.
struct Capabilities {
    bool typing        = false; // live "user is typing" events (internal path only)
    bool presence      = false; // service has any user presence (online/away dots at all).
                                // IMAP/email has no presence concept → false → no dot drawn.
    bool livePresence  = false; // realtime presence_change (vs. polled presence)
    bool huddles       = false; // live huddle indicator + join links
    bool canvases      = false; // channel canvas tab + editing
    bool slashCommands = false; // listCommands()/runCommand()
    bool reactions     = false; // add/remove emoji reactions
    bool editMessage   = false; // edit own messages (email cannot — see deleteMessage)
    bool deleteMessage = false; // delete own messages (split from editMessage: email can delete a
                                // sent message but never edit it)
    bool deleteAnyMessage = false; // delete *any* message, not just your own (email: it's your own
                                   // mailbox, so every message is deletable regardless of author).
                                   // Requires deleteMessage. Slack/Teams leave this false
                                   // (own-only, plus the separate admin path).
    bool threads          = false; // threaded replies
    bool replyBroadcast   = false; // thread reply can also appear in its channel
    bool memberList       = false; // loadMembers(): who is in a channel or group DM (Slack:
                                   // conversations.members) — the header's member list
    bool gifAttachments   = false; // a GIF from the composer's picker posts as an image of its
                                   // own (OutgoingMessage::gifs), the way Slack's GIF picker
                                   // sends one. Off: it stays a link in the text.
    bool threadsView      = false; // workspace-wide "Threads" overview (loadThreadsView).
                                   // Separate from `threads`: a backend can support replies
                                   // without any server-side subscribed-threads feed (Slack's
                                   // feed is session-token only; IMAP/Teams have none).
    bool botButtons       = false; // press a bot message's interactive (Block Kit) buttons. Slack
                                   // serves the internal blocks.actions only to a session (xoxc)
                                   // token; without it a click explains instead of pressing.
    bool messageReminders = false; // per-message "Save for later" / "Remind me" (Slack's Later).
                                   // Rides the internal saved.* API family, which Slack only
                                   // serves to a session (xoxc) token — OAuth workspaces would
                                   // get every call rejected, so the menu entry is gated here.
    bool permalinks       = false; // "Copy link" on a message: the service has a stable, shareable
                                   // per-message URL we can BUILD locally (Slack: the /archives/
                                   // permalink from teamUrl + conv + ts). Teams/IMAP have no such
                                   // client-constructible link, so the menu entry is gated here.
    bool fileUpload       = false; // upload + share files
    bool scheduledSend    = false; // send a message at a future time (chat.scheduleMessage).
                                   // Slack-only: Teams' Graph has no delegated scheduled-send and
                                   // SMTP has no native one, so the composer's schedule-send
                                   // dropdown is gated on this to avoid a dead control.
    bool moveToThread     = false; // "Move to thread": re-post a top-level message as a reply and
                                   // delete the original once the copy lands. Needs a sendMessage
                                   // that reports its outcome (the delete must never run first).
    bool messageSubjects  = false; // per-message subject line (email); shows the composer subject
                                   // field — see imap-backend-plan §3/§4
    bool collapseQuotedReplies =
        false;                  // email: a reply's trailing quoted history + signature is
                                // the previous message(s) already shown above, so collapse
                                // it behind a "show quoted text" toggle (messenger, not mail
                                // client). Chat services quote intentionally → leave false.
    bool sidebarTheme  = false; // loadSidebarTheme(): the user's stored sidebar theme (Slack's
                                // users.prefs.get, served only to a session token) — feeds the
                                // "Use my Slack theme" button of the custom theme editor.
    bool presenceLink  = false; // setPresenceMode(): the backend can hold the connection that
                                // makes the service show this user "active" without an
                                // official client (Slack: RTM on a session token; an OAuth
                                // token is refused rtm.connect). Requires presence.
    bool rosterRefresh = false; // loadUsers() is a cheap, side-effect-free server snapshot the
                                // Session may re-fetch on its daily cadence so renames and new
                                // avatars reach a long-running session (Slack: users.list). Off
                                // for a backend whose loadUsers re-downloads per-member data
                                // (Teams fetches every photo) or is purely local (IMAP).
    bool removePreview = false; // deleteAttachment(): strip a link preview from an OWN message
                                // server-side, for everyone — the official client's "Remove
                                // preview" (Slack: the internal chat.deleteAttachment, served to
                                // a session token only). Without it the × on a preview only
                                // hides the card locally, for this session.
    bool operator==(const Capabilities &) const = default;
};

// The user's sidebar theme as the service stores it (Slack: users.prefs.get).
// `iaTheme` is the redesign's JSON (`{"primary":{"palette":"aubergine"},…}`),
// `legacyValues` the older custom theme as Slack's comma-joined hex list in
// slot order (column_bg, menu_bg, …). Either may be empty; Th::parseCustomTheme
// reads both.
struct SidebarThemePrefs {
    QString iaTheme;
    QString legacyValues;
};

// --- Core domain structs ---

struct User {
    UserId  id;
    QString name;
    QString displayName;
    QString avatarUrl;
    bool    isBot      = false;
    bool    isExternal = false; // Slack Connect external member — shows "EXT" tag. Set when
                                // is_stranger is true or teamId differs from our workspace team.
    bool    isStranger = false; // raw Slack is_stranger: a Connect user we share a channel with
                                // but cannot open a DM to (conversations.open → user_not_found).
                                // Kept separate from isExternal, which is broadened by team_id.
    QString teamId;             // home team/workspace (Slack team_id); differs for external users
    bool    isActive      = false; // presence; polled on public path
    bool    isDeactivated = false; // Slack "deleted" flag
    bool    isAdmin       = false; // is_admin || is_owner from users.list
    bool    isOwner       = false; // is_owner / is_primary_owner (profile card role label)
    bool    dndEnabled    = false; // do-not-disturb; updated via dnd_updated_user event
    QString statusEmoji;           // Slack emoji name without colons, e.g. "palm_tree"
    QString statusText;            // user status text, e.g. "On vacation"
    QString title;                 // job title from profile.title
    QString email;                 // address for contact-centric services (always set for email
                                   // backends, where it doubles as the UserId; Slack fills it only
    // when the token has users:read.email). Shown on the profile card.
    bool    hasTz                          = false; // true when tzOffset is known
    int     tzOffset                       = 0;     // seconds east of UTC (Slack tz_offset)
    bool    operator==(const User &) const = default;

    // Name to show in UI: the display name when set, otherwise the account name.
    const QString &displayLabel() const { return displayName.isEmpty() ? name : displayName; }
};

// A user group (Slack "subteam", handle @eng-oncall). Mentioned in text as
// <!subteam^S…|@handle>; the label is optional, and a rich_text usergroup
// element carries only the id, so the roster is needed to show a name. On
// Enterprise Grid the same S… ids are shared org-wide, and usergroups.list
// must be asked for the member workspace's team_id to return them.
struct Usergroup {
    QString             id;     // S… id
    QString             handle; // mention handle without the '@', e.g. "eng-oncall"
    QString             name;   // display name, e.g. "Engineering on-call"
    std::vector<UserId> users;  // members (usergroups.list include_users=1)
    bool                operator==(const Usergroup &) const = default;

    // What to show for a mention: "@handle", or the name when there is no handle.
    QString mentionLabel() const { return "@" + (handle.isEmpty() ? name : handle); }
};

// Rich presence for the authed user only. users.getPresence returns these
// extra fields when called for yourself; for everyone else only the binary
// active/away (User::isActive) exists.
struct SelfPresence {
    bool loaded          = false; // true once a snapshot has actually arrived
    bool active          = false; // what others see: true=active, false=away
    bool online          = false; // at least one official client connection exists
    bool autoAway        = false; // idle >10 min while a client is connected
    bool manualAway      = false; // user explicitly set themselves away
    int  connectionCount = 0;     // official clients only; Socket Mode never counts

    // True when the user appears away to others *only* because no official
    // Slack client is connected — not because they chose (or idled into) away.
    bool phantomAway() const { return loaded && !active && !online && !manualAway; }
    bool operator==(const SelfPresence &) const = default;
};

// How the app should hold the user's presence on services that only show a user
// "active" while one of the service's own clients has a live connection (Slack:
// the official apps' socket — a Web-API-only client is always "away" to others,
// users.setPresence can force away but never active). The backend can hold such
// a connection itself (Slack: an RTM socket on the user's session token) so the
// user appears active from this app alone. Chosen once in Settings → System →
// Presence (util/presence_settings.h) and applied to every workspace.
enum class PresenceMode {
    Native,       // leave it to the service's official clients (no connection held)
    WhileUsing,   // hold the connection while the user interacts with the app; drop
                  // it after ~30 min without input (like the official auto-away)
    WhileRunning, // hold the connection for as long as the app runs
};

// State of that presence-holding connection, so the UI can explain WHY the user
// still appears away (Backend::setPresenceMode → EvPresenceLinkChanged).
enum class PresenceLinkState {
    Off,         // PresenceMode::Native, or the backend has no such connection
    Connecting,  // wanted but not (yet / currently) established — incl. reconnect backoff
    Active,      // established: the service counts this app as a connected client
    Idle,        // WhileUsing: dropped on purpose after the idle timeout
    Unavailable, // the service refused it for this workspace (e.g. an OAuth token)
};

// Editable fields of the authed user's own profile (users.profile.get /
// users.profile.set). avatarUrl is read-only here — it's changed via
// users.setPhoto, not the profile fields.
struct MyProfile {
    QString realName;    // profile.real_name ("Full name")
    QString displayName; // profile.display_name (the name shown in the UI)
    QString email;       // profile.email
    QString phone;       // profile.phone
    QString avatarUrl;   // profile.image_512 / image_192
    bool    operator==(const MyProfile &) const = default;
};

struct Conversation {
    ConversationId id;
    ConvKind       kind;
    QString        name;
    QString        description; // channel topic/purpose; empty for DMs
    bool           isMember    = false;
    int            memberCount = 0; // num_members from conversations.list; 0 for DMs
    Ts             lastRead;
    Ts             latestTs; // ts of most recent message (from conversations.list "latest.ts")
    int            unread       = 0;
    int            mentionCount = 0; // @mentions in channels; for DMs treat all unread as mentions
    std::optional<UserId> dmUser;    // set for Im conversations
    std::vector<UserId>   members;   // set for Mpim conversations (all participants)
    bool                  isMuted      = false;
    bool                  isStarred    = false;
    // Purely local "mute this person" switch (DM context menu). Unlike isMuted /
    // NotificationLevel::Mute it does NOT silence the chat in the list — the
    // conversation still shows its bold "unread" emphasis. It only suppresses the
    // outward signals: no OS notification, no tray ball, no workspace ball, and no
    // red unread counter. No backend supports it, so it lives only in our cache.
    bool                  locallyMuted = false;
    // A name the user gave this group DM in msga ("Name conversation…" in the
    // chats-list menu). Purely local: shown instead of the member list on every
    // surface that titles the conversation, never sent anywhere, so it works on
    // any backend and on OAuth workspaces alike. Lives only in our cache.
    QString               localName;
    NotificationLevel     notifLevel = NotificationLevel::Default;
    QString canvasFileId; // channel canvas file id (conversations.info "properties.canvas"); empty
                          // = none
    bool    canvasIsEmpty = false;
    // A Slack huddle is currently live in this conversation. Derived from the
    // conversations.info `room` object — the only ToS-clean, channel-attached
    // huddle signal our token can see (huddles aren't in the public API; the
    // RTM user_huddle_changed event isn't delivered over Socket Mode and is
    // user-keyed, not channel-keyed).
    bool    huddleActive  = false;
    // Preferred join URL straight from the room (`huddle_link`), e.g.
    // https://app.slack.com/huddle/<team>/<channel>; empty falls back to a
    // constructed link.
    QString huddleLink;
    // People to show on the huddle indicator: current participants, or the host
    // (`created_by`) alone for a freshly-started "prewarmed" huddle that nobody
    // has connected to yet.
    std::vector<UserId> huddleParticipants;
    // Email backends only: the subject a reply into this thread should use
    // ("Re: <latest subject>"), so the composer can prefill it. Empty for chat
    // services and for brand-new conversations with no thread yet.
    QString             replySubject;
    // Transient wire-signal, never cached/persisted: set by loadConversationInfo
    // when conversations.info answers `channel_not_found` (the conversation does
    // not exist for this workspace — another workspace's conv off the shared
    // socket, or a dead DM). Lets Session tell a definitive "gone" from a
    // transient failure and stop re-fetching it, without confusing it for a real
    // conversation. Only ever true on that sentinel result; a real conv is false.
    bool                notFound                               = false;
    bool                operator==(const Conversation &) const = default;
};

// The name to title a group DM with before falling back to its member list:
// the user's local alias first, else a name the service itself reports for it
// (a Slack MPDM renamed in the official client, a Teams group chat topic).
// Slack's auto-generated "mpdm-alice--bob-1" is an id, not a name, and yields
// empty — callers then derive the title from the members. Empty for every other
// conversation kind.
inline QString groupDmCustomName(const Conversation &c) {
    if (c.kind != ConvKind::Mpim)
        return {};
    if (!c.localName.isEmpty())
        return c.localName;
    if (!c.name.isEmpty() && !c.name.startsWith(QLatin1String("mpdm-")))
        return c.name;
    return {};
}

// Resolve a conversation's *effective* notification level — the single source
// of truth for both OS notifications and unread-badge colors.
//   • Muted always wins (a muted/"mute and hide" conversation is fully silent:
//     no notification and no badge, not even for @mentions).
//   • An explicit per-conversation level (set from the conv right-click menu,
//     persisted locally) is honoured next.
//   • Otherwise the user's global default applies. Slack's server-side
//     per-channel notification prefs are not reachable over the public API, so
//     the per-conversation level is a purely local override and `fallback` is
//     the global default — which itself defaults to "All new posts".
inline NotificationLevel effectiveNotifLevel(const Conversation &c, NotificationLevel fallback) {
    if (c.isMuted || c.notifLevel == NotificationLevel::Mute)
        return NotificationLevel::Mute;
    if (c.notifLevel == NotificationLevel::Default)
        return fallback;
    return c.notifLevel;
}

// Notification freshness policy (see kMaxNotifyAgeDays). Both overloads
// fail OPEN: they answer "too old" only when the age is positively known, so an
// unknown/unparseable time never silently swallows a live event.
//
// Event form — `dateMicros` is Message::date (epoch microseconds, the one
// orderable time field). A zero/absent date means "no idea when" → notify.
inline bool tooOldToNotify(qint64 dateMicros) {
    if (dateMicros <= 0)
        return false;
    const qint64 cutoffSecs =
        QDateTime::currentSecsSinceEpoch() - qint64(kMaxNotifyAgeDays) * 86400;
    return dateMicros / 1000000 < cutoffSecs;
}

// Conversation form — for the badge/counter surfaces, which count server-side
// unreads rather than individual events. `latestTs` is the newest thing the
// conversation holds, so when even that is outside the window every unread in
// it is too. Only Slack's ts is a decimal clock (Teams carries a message id,
// IMAP a Message-ID header); those parse to 0 or to a nonsense future value,
// and either way the conversation counts as current.
inline bool tooOldToNotify(const Conversation &c) {
    if (c.latestTs.isEmpty())
        return false;
    return tooOldToNotify(decimalTsToMicros(c.latestTs));
}

// Whether a starting huddle in conversation `c` should raise a desktop
// notification. Pure policy (no UI/settings state) so it's unit-testable:
//   • member-only, never when muted / level Mute;
//   • never for a huddle I'm already in (my id is among the participants);
//   • a DM/MPDM huddle always notifies, a channel huddle only when the conv's
//     effective level is "All new posts" (a mentions-only channel's huddle is
//     the same opted-out noise as its messages).
// The caller still owns the orthogonal gates: the notifications-enabled and
// per-huddle settings, the huddle capability, the already-notified dedup, and
// the "conversation already on screen" case.
inline bool shouldNotifyHuddleStart(
    const Conversation        &c,
    const std::vector<UserId> &participants,
    const UserId              &me,
    NotificationLevel          fallback
) {
    if (!c.isMember || c.isMuted || c.notifLevel == NotificationLevel::Mute)
        return false;
    if (!me.value.isEmpty())
        for (const auto &p : participants)
            if (p == me)
                return false;
    const bool isDm = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
    if (!isDm && effectiveNotifLevel(c, fallback) != NotificationLevel::All)
        return false;
    return true;
}

// One conversation's server-side activity/badge state, as reported by a single
// whole-workspace snapshot call (Slack's `client.counts`). This is the cheap
// signal a poll-only backend needs: without a push transport, the ONLY way a
// message in a conversation the user hasn't opened can ever surface is for us to
// notice the conversation moved and then fetch its history. One request answers
// that for every conversation at once — vastly cheaper than a per-conversation
// info sweep, and unlike conversations.list it also reports channels.
// Fields absent from a given backend's response stay empty/zero; the consumer
// diffs whatever it does get (see Session::applyActivitySnapshot).
struct ConvCounts {
    ConversationId id;
    Ts             latestTs; // ts of the newest message the server knows
    Ts             lastRead; // the authed user's read cursor
    int            unread                               = 0;
    int            mentionCount                         = 0;
    bool           operator==(const ConvCounts &) const = default;
};

// Click-target of an OS notification, round-tripped through the notifier (and,
// on Windows, an msga:// protocol activation) as a 0x1f-separated token:
// "teamId\x1fconvId", or "teamId\x1fconvId\x1frootTs" when the notified message
// is a thread reply. The root matters because conversations.history omits thread
// replies, so opening the channel alone lands on a timeline the reply isn't in
// ("notification, but nothing there") — the root routes the click to the thread.
// A reminder notification appends a fourth field, the reminded message's own ts
// ("teamId\x1fconvId\x1frootTs\x1fmsgTs", rootTs left empty for a non-reply), so
// the click can scroll to the exact message rather than just open the chat.
// Encapsulated + unit-tested because the field splitting is easy to get subtly
// wrong (e.g. a naive indexOf swallowing the root into the conv id).
struct NotifTarget {
    QString        teamId;
    ConversationId conv;
    Ts             threadRoot; // empty unless the notified message was a reply
    Ts             msgTs;      // set only for reminder clicks: the exact message to focus
    bool           operator==(const NotifTarget &) const = default;
};

inline QString encodeNotifToken(
    const QString &teamId, const ConversationId &conv, const Ts &threadRoot, const Ts &msgTs = {}
) {
    QString t = teamId + QChar(0x1f) + conv.value;
    if (!threadRoot.isEmpty() || !msgTs.isEmpty())
        t += QChar(0x1f) + threadRoot; // kept empty (not omitted) when only msgTs is set
    if (!msgTs.isEmpty())
        t += QChar(0x1f) + msgTs;
    return t;
}

inline std::optional<NotifTarget> decodeNotifToken(const QString &token) {
    const QStringList parts = token.split(QChar(0x1f));
    if (parts.size() < 2 || parts.at(1).isEmpty())
        return std::nullopt; // no conversation to open
    NotifTarget t;
    t.teamId     = parts.at(0);
    t.conv       = ConversationId{parts.at(1)};
    t.threadRoot = parts.size() >= 3 ? parts.at(2) : Ts{};
    t.msgTs      = parts.size() >= 4 ? parts.at(3) : Ts{};
    return t;
}

// Click-target of the "session expired" notification (raised when a workspace's
// credentials are rejected for good while the window is tucked away in the tray,
// so the user learns they have to sign in again). Distinct from a conversation
// token: there is no chat to open, only a workspace to bring back to the login
// screen. The "relogin" prefix can never collide with a real team id (Slack ids
// are uppercase alphanumerics), and the 0x1f separator matches the codec above
// so the Windows msga://notif round trip treats both tokens the same way.
inline QString encodeReloginNotifToken(const QString &teamId) {
    return QStringLiteral("relogin") + QChar(0x1f) + teamId;
}

// The team id the token names, or nullopt when the token is not a relogin one.
inline std::optional<QString> decodeReloginNotifToken(const QString &token) {
    static const QString kPrefix = QStringLiteral("relogin") + QChar(0x1f);
    if (!token.startsWith(kPrefix))
        return std::nullopt;
    return token.mid(kPrefix.size());
}

// A message saved to Slack's "Later" list (see Backend::loadMessageReminders):
// either a plain "Save for later" bookmark (dueAt == 0) or a reminder (dueAt >
// 0, alarms when due). Both are one server item — a due date added to a saved
// message turns it into a reminder in place. The backend fills conv/ts/dueAt/
// savedAt from the server; threadRoot/snippet/author/bot* /fired are local
// enrichment the Session captures at set time (the server item doesn't carry
// them) and persists so the reminder's notification can route to the thread,
// show a preview, and show who wrote the message. `fired` marks a reminder
// whose notification was already raised, so a restart doesn't re-announce it;
// the item itself stays (blue tint, "remove reminder") until the user removes
// it — matching the official client's overdue behaviour.
struct MessageReminder {
    ConversationId conv;
    Ts             ts;
    qint64         dueAt   = 0; // Unix seconds; 0 = saved for later, no alarm
    qint64         savedAt = 0; // Unix seconds the item was saved (orders the bookmarks)
    Ts             threadRoot;
    QString        snippet;
    UserId         author;       // message author; empty for authorless bot posts
    QString        botName;      // bot_message display name (author is empty then)
    QString        botAvatarUrl; // bot_message avatar; users resolve theirs live
    bool           fired                                     = false;
    bool           operator==(const MessageReminder &) const = default;
};

// A reminder's local enrichment on its own — everything in MessageReminder that
// the server's saved item does NOT carry. The Session keeps these in a shadow
// map that outlives the reminder record itself, so an item that momentarily
// leaves the local list (an ambiguous write, a cache loss, a server snapshot
// that raced an optimistic add) comes back with its preview instead of as a
// blank "No preview available" card. What is still missing after that — a
// reminder set from another client, say — is fetched from the message itself
// (Session::resolveReminderPreviews).
struct ReminderPreview {
    Ts      threadRoot;
    QString snippet;
    UserId  author;
    QString botName;
    QString botAvatarUrl;

    bool isEmpty() const {
        return snippet.isEmpty() && author.value.isEmpty() && botName.isEmpty();
    }
    bool operator==(const ReminderPreview &) const = default;
};

// One canvases.edit operation. Relative inserts and section ops need a
// sectionId (the "temp:C:…" ids embedded in the canvas HTML / returned by
// canvases.sections.lookup); markdown is canvas markdown — real markdown,
// NOT Slack mrkdwn.
struct CanvasChange {
    enum class Op {
        InsertAtStart,
        InsertAtEnd,
        InsertAfter,    // needs sectionId
        InsertBefore,   // needs sectionId
        ReplaceSection, // needs sectionId
        ReplaceAll,
        DeleteSection, // needs sectionId; markdown unused
        Rename,        // markdown = new canvas title; sectionId unused
    };
    Op      op = Op::InsertAtEnd;
    QString sectionId;
    QString markdown;
    bool    operator==(const CanvasChange &) const = default;
};

// Outcome of a canvas metadata lookup (files.info).
// Gone     — the file no longer exists (file_deleted / file_not_found);
//            conversations.info keeps referencing deleted channel canvases,
//            so this is the authoritative existence check.
// NoAccess — the file exists but the token may not view it (not_visible),
//            e.g. a canvas in a public channel the user hasn't joined or one
//            with restricted access — show it read-only, never edit it.
// Ok       — visible; also the fallback for transient/unknown errors (then
//            with empty title/permalink).
enum class CanvasMetaState { Ok, Gone, NoAccess };

// True when mrkdwn text explicitly mentions `me` — a direct <@U…> / <@U…|name>
// mention, a <!subteam^S…> mention of a user group in `myUsergroups`, or a
// broadcast keyword (<!here>, <!channel>, <!everyone>). This is what the
// official Slack client treats as a mention for red badges and notifications.
inline bool
mrkdwnMentions(const QString &mrkdwn, const UserId &me, const QSet<QString> &myUsergroups = {}) {
    if (!me.value.isEmpty()) {
        const QString tag = QStringLiteral("<@") + me.value;
        for (qsizetype i = mrkdwn.indexOf(tag); i >= 0; i = mrkdwn.indexOf(tag, i + 1)) {
            const qsizetype after = i + tag.size();
            if (after < mrkdwn.size() && (mrkdwn[after] == u'>' || mrkdwn[after] == u'|'))
                return true;
        }
    }
    if (!myUsergroups.isEmpty()) {
        const QString tag = QStringLiteral("<!subteam^");
        for (qsizetype i = mrkdwn.indexOf(tag); i >= 0; i = mrkdwn.indexOf(tag, i + 1)) {
            const qsizetype start = i + tag.size();
            qsizetype       end   = start;
            while (end < mrkdwn.size() && mrkdwn[end] != u'>' && mrkdwn[end] != u'|')
                ++end;
            if (myUsergroups.contains(mrkdwn.mid(start, end - start)))
                return true;
        }
    }
    return mrkdwn.contains(QLatin1String("<!here")) ||
           mrkdwn.contains(QLatin1String("<!channel")) ||
           mrkdwn.contains(QLatin1String("<!everyone"));
}

struct Reaction {
    QString             name; // e.g. "thumbsup"
    int                 count = 0;
    std::vector<UserId> users;
    bool                operator==(const Reaction &) const = default;
};

// --- Text with inline markup ---

enum class EntityType {
    Bold,
    Italic,
    Underline,
    Strike,
    Code,
    Pre,
    Blockquote,     // block-level; data unused
    Link,           // data = URL
    UserMention,    // data = UserId::value
    ChannelMention, // data = ConversationId::value
    HereCommand,
    ChannelCommand,
    Emoji, // data = emoji name (e.g. "rocket")
    // A link to another message, rendered as a chip that jumps to it.
    // data = SlackLinks::refToToken(ref) — see util/slack_links.h.
    // NOTE: cached entities store this enum as an int, so new values go LAST.
    MessageLink,
    UsergroupMention, // data = Usergroup::id (S…); text = the parser's "@label" fallback
};

struct TextEntity {
    EntityType type;
    int        offset = 0;
    int        length = 0;
    QString    data; // type-dependent payload (see EntityType)
    bool       operator==(const TextEntity &) const = default;
};

struct TextWithEntities {
    QString                 text; // plain text with entities stripped
    std::vector<TextEntity> entities;
    bool                    operator==(const TextWithEntities &) const = default;
};

// --- Phase 3: Files, Blocks, Attachments ---

// One entry of a Slack file's prerendered thumbnail ladder (thumb_64 … thumb_1024).
struct FileThumb {
    int width = 0; // actual pixel width (thumb_N_w; N is the long side, so width < N for portraits)
    int height = 0;
    QString url; // auth required
    bool    operator==(const FileThumb &) const = default;
};

// A transcript the user's own AI provider produced for an audio file — kept
// per workspace so it keeps replacing Slack's (or fills in for an upload
// Slack never transcribes) across restarts. See Session::setAiTranscript.
struct AiTranscript {
    QString text;
    QString provider; // display name, shown as "transcribed by …"
    bool    operator==(const AiTranscript &) const = default;
};

// File shared in a Slack message (from the "files" array).
struct File {
    QString                id;
    QString                name;
    QString                mimeType;
    QString                prettyType; // human-readable type, e.g. "PDF", "Word Document"
    QString                urlPrivate; // url_private: auth header required for download
    // url_private_download: the original bytes as uploaded. For audio uploads
    // url_private is Slack's AAC/MP4 transcode (see aacUrl), so this is the
    // only way at the .mp3/.wav itself — and what "Download" must save.
    QString                urlPrivateDownload;
    QString                permalink; // Slack web UI URL — no auth required, opens in browser
    QString                thumbUrl;  // thumbnail URL (e.g. thumb_360); auth required
    int                    imageWidth  = 0;
    int                    imageHeight = 0;
    qint64                 size        = 0;
    std::vector<FileThumb> thumbs; // thumbnail ladder, ascending by width
    // Animated preview ladder (thumb_360_gif/thumb_480_gif): the plain thumb_N
    // renders of a GIF are static first frames, so these take priority.
    std::vector<FileThumb> animThumbs;
    // Audio metadata. durationMs comes from Slack's `duration_ms` (0 when
    // unknown). aacUrl is Slack's server-side AAC/MP4 transcode (`aac`), made
    // for every audio file: voice clips are recorded as WebM/Opus, which no
    // native player decodes, so the transcode is the portable playback source.
    qint64                 durationMs = 0;
    QString                aacUrl;  // auth required
    QString                subtype; // e.g. "slack_audio" for voice clips
    // Slack's `filetype` id ("html", "python", "pdf", …): what Slack keys its
    // file icons off. Empty on other backends — callers fall back to the name.
    QString                fileType;
    // Slack's own speech-to-text for voice clips (`transcription` + `vtt`):
    // status ("complete" when usable; uploads report "none"), the one-line
    // preview, and the WebVTT with per-cue timestamps (auth required).
    QString                transcriptStatus;
    QString                transcriptPreview;
    QString                transcriptVttUrl;
    // Non-empty when transcriptPreview is NOT Slack's: the user's own AI
    // provider transcribed the file locally (Session::applyAiTranscripts) and
    // its text replaced Slack's line. Holds the provider's display name.
    QString                transcriptBy;
    // Canvas files (Slack "quip" docs): the display title. Unlike `name` (a
    // slugged filename) it keeps the emoji codes and <@U…> mentions Slack puts
    // in huddle-notes titles, entity-decoded.
    QString                title;

    // Preview source covering physW physical pixels: the smallest thumbnail wide
    // enough, else the largest available (never the original — it can be huge),
    // else the legacy thumbUrl, else the original file. animated=false skips
    // the animated ladder (a viewer with animations disabled wants the small
    // static render, not a multi-megabyte GIF it would only show one frame of).
    QString previewUrl(int physW, bool animated = true) const {
        if (animated) {
            for (const auto &t : animThumbs)
                if (t.width >= physW)
                    return t.url;
            if (!animThumbs.empty())
                return animThumbs.back().url;
        }
        for (const auto &t : thumbs)
            if (t.width >= physW)
                return t.url;
        if (!thumbs.empty())
            return thumbs.back().url;
        return thumbUrl.isEmpty() ? urlPrivate : thumbUrl;
    }

    bool isImage() const { return mimeType.startsWith("image/") && imageWidth > 0; }
    bool isPdf() const { return mimeType == "application/pdf"; }
    // Slack canvases (filetype "quip"): drawn as a preview card in the message
    // list, opened in the in-app canvas viewer.
    bool isCanvas() const { return mimeType == QLatin1String("application/vnd.slack-docs"); }
    // Audio uploads and Slack voice clips get the inline player chip.
    bool isAudio() const {
        return mimeType.startsWith("audio/") || subtype == QLatin1String("slack_audio");
    }
    bool hasTranscript() const {
        return transcriptStatus == QLatin1String("complete") && !transcriptPreview.isEmpty();
    }
    // CSV uploads get a "Preview" action that opens them in the table viewer.
    bool isCsv() const {
        return mimeType == "text/csv" || name.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive);
    }
    // True when Slack provides a prerendered preview image: the image itself, or the
    // server-rendered first page of a PDF (thumb_pdf) — no client-side rendering needed.
    bool hasPreview() const { return isImage() || (isPdf() && !thumbUrl.isEmpty()); }
    bool operator==(const File &) const = default;
};

// A bot button — from a Block Kit "actions"/"section" button element or a
// legacy attachment "actions" entry. URL buttons just open their URL. A Block
// Kit button (actionId set) can also be pressed via Capabilities::botButtons
// (Slack: the internal blocks.actions, session tokens only); legacy attachment
// buttons carry no actionId and stay display-only.
struct BotButton {
    QString text;
    QString url;      // empty for interactive-only buttons
    QString style;    // ""|"primary"|"danger"
    QString actionId; // Block Kit action_id; empty for legacy attachment buttons
    QString blockId;  // block_id of the enclosing "actions"/"section" block
    QString value;    // opaque payload the bot gets back on a press
    bool    operator==(const BotButton &) const = default;
};

// Simplified Block Kit block. Covers the common types needed for rendering.
// rich_text, section, header, context → text field populated.
// image → imageUrl/altText populated; text may be empty.
// divider → typeStr == "divider", rest empty.
// actions → buttons populated; section may also carry an accessory button.
// table → tableRows populated (cells left empty for the payload's null cells).
struct Block {
    QString typeStr; // "section"|"header"|"divider"|"image"|"context"|"rich_text"|"actions"|"table"
    TextWithEntities       text; // primary displayable text; for "image" blocks this is the title
    QString                imageUrl;        // for "image" blocks
    QString                altText;         // for "image" blocks
    int                    imageWidth  = 0; // for "image" blocks; 0 when not provided
    int                    imageHeight = 0;
    std::vector<BotButton> buttons;                       // "actions" elements / section accessory
    std::vector<std::vector<TextWithEntities>> tableRows; // "table" cells, row-major
    bool                                       operator==(const Block &) const = default;
};

// Legacy Slack attachment (link unfurls, bot messages, older integrations).
// One entry of an attachment's "fields" array (bold title + mrkdwn value).
struct AttachmentField {
    QString          title;
    TextWithEntities value;
    bool             operator==(const AttachmentField &) const = default;
};

struct Attachment {
    // Slack's positional attachment id: 1-based, renumbered by the server when
    // one is removed (chat.deleteAttachment). 0 when the service sent none.
    int                          id = 0;
    QString                      fallback;
    QString                      color; // "#rrggbb" left-border accent; may be empty
    QString                      pretext;
    QString                      authorName;
    QString                      title;
    QString                      titleLink;
    TextWithEntities             text;
    QString                      imageUrl;
    QString                      thumbUrl;
    QString                      faviconUrl;      // service_icon URL (favicon for link previews)
    QString                      footer;          // footer text; carries <url|label>/<!date> tokens
    QString                      footerIcon;      // footer_icon URL, drawn before the footer text
    int                          imageWidth  = 0; // image_url dimensions; 0 when not provided
    int                          imageHeight = 0;
    int                          thumbWidth  = 0; // thumb_url dimensions; 0 when not provided
    int                          thumbHeight = 0;
    std::vector<AttachmentField> fields;  // bold-titled key/value rows (classic bot format)
    std::vector<Block>           blocks;  // Block Kit blocks embedded in this attachment
    std::vector<BotButton>       buttons; // legacy "actions" buttons (classic bot format)

    // Generated link preview, including app and shared-message unfurls.
    bool isLinkPreview = false;

    // --- Shared-message unfurl (Slack's `is_msg_unfurl`) ---
    // A message quoted into another conversation by pasting its permalink. The
    // fields above then describe the QUOTED message, not a link preview:
    // authorName/authorIcon are its author, `blocks`/`text` its body, `files` its
    // uploads. Official clients render it as a card with the author's avatar,
    // name, time and "Posted in #channel" — not as a colored-bar preview — so the
    // renderer needs to tell the two shapes apart.
    bool              isMsgUnfurl = false;
    QString           authorIcon;    // author_icon: the quoted author's avatar
    QString           authorSubname; // author_subname: bot username; set only for app posts
    QString           channelId;     // channel_id: where the quoted message lives
    // `ts`, epoch micros: the footer's timestamp on an ordinary attachment (drawn
    // after the footer text), the quoted message's wall clock on a message unfurl.
    qint64            msgDate = 0;
    std::vector<File> files; // files attached to the quoted message

    bool operator==(const Attachment &) const = default;

    // Preview source covering physW physical pixels: the thumbnail when it is
    // large enough (or its size is unknown), the full image otherwise.
    QString previewUrl(int physW) const {
        if (thumbUrl.isEmpty() || imageUrl.isEmpty())
            return thumbUrl.isEmpty() ? imageUrl : thumbUrl;
        return (thumbWidth > 0 && thumbWidth < physW) ? imageUrl : thumbUrl;
    }
};

// --- Messages ---

// The call a huddle_thread message announces, from its `room` object — what the
// official client's "A huddle happened · You and X were in the huddle for 8m"
// row is made of. Names are roster state, so the sentence itself is built at
// render time (MsgRender::huddleSummaryText).
struct HuddleInfo {
    // Everyone who was in it (room.participant_history) once it ended; the
    // current participants while it is live.
    std::vector<UserId> attendees;
    qint64              startSec                             = 0; // room.date_start, unix seconds
    qint64              endSec                               = 0; // room.date_end, 0 while live
    bool                ended                                = false;
    bool                operator==(const HuddleInfo &) const = default;
};

struct Message {
    Ts                    ts;
    // Wall-clock time AND sort key, in epoch microseconds. The single orderable
    // time field: `ts` is identity only (equality / dedup / mutation target /
    // thread linkage), never compared for ordering — on non-Slack services the id
    // is not a clock. The Slack backend fills this by parsing `ts`; cached
    // messages backfill it from their stored `ts` on load. Display, sorting,
    // grouping, and the last-read compare all read `date`.
    qint64                date = 0;
    std::optional<Ts>     threadRoot;     // set when message is in a thread (reply)
    int                   replyCount = 0; // >0 on thread root messages
    std::vector<UserId>   replyUsers;     // participants (up to 5, from reply_users)
    std::optional<Ts>     latestReply;    // ts of the most recent reply
    UserId                parentUserId;   // author of the thread root (Slack's parent_user_id),
                                          // set on thread replies; == me identifies a reply to a
                                          // thread we started (see isFollowedThreadReply)
    UserId                author;
    QString               botName;      // display name for bot_message (from username field)
    QString               botAvatarUrl; // avatar URL for bot_message (from bot_profile or icon_url)
    QString               botId;        // posting bot (Slack bot_id); a bot-button press targets it
    TextWithEntities      text;
    QString               rawText; // original mrkdwn from Slack; used for edit pre-fill
    std::vector<Reaction> reactions;
    bool                  edited = false;
    std::optional<QString>    subtype;        // "bot_message", "channel_join", etc.
    std::vector<File>         files;          // Phase 3
    std::vector<Block>        blocks;         // Phase 3
    std::vector<Attachment>   attachments;    // Phase 3
    bool                      pinned = false; // true if pinned to channel
    UserId                    pinnedBy;       // user who pinned it
    // Local optimistic copy shown while the send/upload is in flight; rendered
    // translucent and replaced by the real message once the server confirms.
    bool                      pending = false;
    // Set on huddle_thread messages (see presentHuddleThread).
    std::optional<HuddleInfo> huddle;
    bool                      operator==(const Message &) const = default;
};

// True when `msg` is a reply to a thread the authed user is "following", so it
// should notify and badge regardless of the channel's notification level —
// matching Slack's default-on "Replies to threads you're following". The
// per-event signal is parent_user_id: a reply carries the thread root's author,
// so a reply to a thread `me` started is `parentUserId == me`. (Threads `me`
// only replied to aren't covered here — Slack exposes no per-event flag for
// that; it would need separate thread-subscription state.)
inline bool isFollowedThreadReply(const Message &msg, const UserId &me) {
    return msg.threadRoot.has_value() && !me.value.isEmpty() && msg.parentUserId == me;
}

// True for Slack "activity" messages — channel/member lifecycle events (joins,
// topic/purpose/name changes, archive, integration add/remove, pins, …) rather
// than real content. These render as centered system lines (no
// avatar/header/toolbar) and cannot host a thread, so this is a denylist of
// those subtypes, not an `!subtype` test. Content subtypes that the official
// client draws as ordinary messages — bot_message, file_share, me_message,
// thread_broadcast, and reminder_add (a user-authored "/remind" message with an
// avatar and name) — are NOT system events. Neither is huddle_thread: it is
// presented as an ordinary bot-style row (see presentHuddleThread) so its
// thread — the huddle's chat — stays reachable.
inline bool isSystemEvent(const Message &m) {
    if (!m.subtype)
        return false;
    static const QSet<QString> kSystemSubtypes = {
        QStringLiteral("channel_join"),
        QStringLiteral("channel_leave"),
        QStringLiteral("channel_topic"),
        QStringLiteral("channel_purpose"),
        QStringLiteral("channel_name"),
        QStringLiteral("channel_archive"),
        QStringLiteral("channel_unarchive"),
        QStringLiteral("group_join"),
        QStringLiteral("group_leave"),
        QStringLiteral("group_topic"),
        QStringLiteral("group_purpose"),
        QStringLiteral("group_name"),
        QStringLiteral("group_archive"),
        QStringLiteral("group_unarchive"),
        QStringLiteral("pinned_item"),
        QStringLiteral("unpinned_item"),
        QStringLiteral("bot_add"),
        QStringLiteral("bot_remove"),
    };
    return kSystemSubtypes.contains(*m.subtype);
}

// Slack announces a huddle by posting a `huddle_thread` message into the
// channel: authored by USLACKBOT, empty text, one rich_text block saying "A
// huddle started" that Slack never updates — the payload is the `room` object
// (consumed separately as EvHuddleChanged, and summarized into Message::huddle).
// Present it the way the official client does: an authorless row whose "name"
// is the event itself ("A huddle happened" once it ended), a headphones tile
// for an avatar (MessageListWidget::paintAvatar), and a body sentence built at
// render time from Message::huddle (MsgRender::huddleSummaryText). The author
// is cleared so name resolution takes the botName path (the USLACKBOT id would
// win the lookup and render "Slackbot"); the stale block is dropped; and the
// labels are ALWAYS overwritten — re-deriving on every load keeps a cached copy
// in the current locale instead of replaying a persisted translation. The text
// is only the plain-text stand-in (previews, copy, search). Applied at both
// points where messages enter the app: JSON mapping (JsonMappers::toMessage)
// and cache load (WorkspaceCache::messageFromJson).
inline bool isHuddleMessage(const Message &m) {
    return m.subtype && *m.subtype == QLatin1String("huddle_thread");
}

inline void presentHuddleThread(Message &m) {
    if (!isHuddleMessage(m))
        return;
    const bool ended = !m.huddle || m.huddle->ended;
    m.author         = {};
    m.botName        = ended ? QCoreApplication::translate("domain", "A huddle happened")
                             : QCoreApplication::translate("domain", "A huddle started");
    m.botAvatarUrl   = {};
    m.blocks.clear();
    m.text = {m.botName};
}

// True for messages the official client draws as ordinary rows (avatar, name,
// timestamp) but greys the body of and refuses to thread — e.g. reminder_add,
// the user-authored "/remind" notice. Distinct from isSystemEvent, which is a
// centered line with no avatar/header at all.
inline bool isMutedMessage(const Message &m) {
    return m.subtype && *m.subtype == QLatin1String("reminder_add");
}

// True when a message can be the parent of a thread. Slack rejects replies to
// system/activity and muted notice messages with `cannot_reply_to_message`, so
// the "Reply in thread" affordance must be hidden for them.
inline bool canHostThread(const Message &m) {
    return !isSystemEvent(m) && !isMutedMessage(m);
}

// The thread `m` belongs to, identified by the root's ts: `m` itself when it's a
// thread root with replies, or its threadRoot when it's a reply. nullopt when
// `m` isn't part of any thread (so no thread-mute affordance should be shown).
inline std::optional<Ts> threadRootOf(const Message &m) {
    if (m.threadRoot)
        return m.threadRoot; // a reply
    if (m.replyCount > 0)
        return m.ts; // a root that has replies
    return std::nullopt;
}

// True for a thread reply that was also sent to the channel ("Also send to
// channel"). It counts toward its root's replies AND has a row of its own in the
// channel view, unlike an ordinary reply, which only bumps the count.
inline bool isThreadBroadcast(const Message &m) {
    return m.threadRoot && m.subtype && *m.subtype == QLatin1String("thread_broadcast");
}

struct MessagePage {
    std::vector<Message>   messages;
    std::optional<QString> olderCursor; // pass to next loadHistory call
    bool                   operator==(const MessagePage &) const = default;
};

// One thread in the workspace-wide "Threads" overview (the threads the authed
// user is subscribed to — started, replied to, or followed). `root` is the
// complete parent message; `latestReplies` is only the newest few (oldest-first
// for display) — the full thread stays one loadThread away.
struct ThreadOverview {
    ConversationId       conv; // channel hosting the thread
    Message              root;
    std::vector<Message> latestReplies;
    Ts                   lastRead; // the authed user's read cursor inside the thread
    bool                 operator==(const ThreadOverview &) const = default;
};

// One page of the Threads overview, newest activity first.
struct ThreadsViewPage {
    std::vector<ThreadOverview> threads;
    int                         totalUnreadReplies = 0; // workspace-wide unread reply count
    bool                        hasMore            = false;
    QString                     nextCursor; // pass to the next loadThreadsView call
    bool                        operator==(const ThreadsViewPage &) const = default;
};

// A GIF from the composer's picker (MarkdownCompose::takeGifLinks), posted as
// Slack's own picker posts one: no link in the text, but an attachment holding
// an image block titled "GIF".
struct OutgoingGif {
    QString url;
    QString altText; // GIPHY's description of the GIF
    bool    operator==(const OutgoingGif &) const = default;
};

// That attachment as it comes back from the server — for the optimistic copy,
// so a sent GIF looks right before the confirmation arrives. `id` is its
// 1-based position among the message's attachments.
inline Attachment gifAttachment(const OutgoingGif &gif, int id) {
    Block image;
    image.typeStr  = QStringLiteral("image");
    image.imageUrl = gif.url;
    image.altText  = gif.altText;
    image.text     = TextWithEntities{QStringLiteral("GIF"), {}};
    Attachment att;
    att.id       = id;
    att.fallback = QStringLiteral("shared a GIF");
    att.blocks   = {std::move(image)};
    return att;
}

struct OutgoingMessage {
    TextWithEntities  text;
    QString           rawText; // original mrkdwn source; sent verbatim to chat.postMessage
    // Block Kit `blocks` to post alongside the text — one rich_text block when
    // the composer text held a list (MarkdownCompose::convert), else empty.
    // Slack only; other services render rawText.
    QJsonArray        blocks;
    std::optional<Ts> threadRoot;
    bool              replyBroadcast = false; // Slack chat.postMessage reply_broadcast
    // Latest server ts known for the conversation when the send started.
    // Anchors the duplicate-check window when a send must be reconciled after
    // a connection loss (server-assigned, so immune to local clock skew).
    Ts                sinceTs;
    // Per-message subject (email backends, gated by Capabilities::messageSubjects;
    // empty for chat services). On a reply the backend inherits the thread subject.
    QString           subject;

    // GIFs taken out of the text, each posted as an attachment of its own
    // (Capabilities::gifAttachments).
    std::vector<OutgoingGif> gifs;
};

// --- Realtime events (normalized from both Socket Mode and internal ws) ---

struct EvMessageNew {
    ConversationId conv;
    Message        msg;
};
struct EvMessageChanged {
    ConversationId conv;
    Message        msg;
    // True when msg carries only the new text (the chat.update response echo —
    // Slack returns just text/user there). The UI merges text+edited into the
    // existing row instead of replacing it, which would strip files, reactions
    // and thread state. False for realtime echoes, which carry the full message.
    bool           textOnly = false;
};
struct EvMessageDeleted {
    ConversationId    conv;
    Ts                ts;
    // Set when the deleted message was a thread reply (its parent's ts), so the
    // channel list can drop the root's reply count. Empty for root/plain msgs.
    std::optional<Ts> threadRoot;
};
// One attachment of an own message was removed server-side ("Remove preview",
// Session::removeAttachment). Fired from the HTTP response, like the delete and
// edit confirmations: the realtime message_changed echo may never arrive on a
// stalled socket and never arrives at all on a poll-only workspace. It carries
// the removed attachment itself rather than its positional id, because Slack
// renumbers the remaining ones and the echo (or a head refresh) may land before
// this event: a handler drops the attachment equal to `attachment` if the row
// still has one, and does nothing otherwise — so applying it after the echo is
// harmless, while an id would then address the wrong (renumbered) card.
struct EvAttachmentRemoved {
    ConversationId conv;
    Ts             ts;
    Attachment     attachment;
};
// The realtime safety poll re-fetched the open conversation's head page. Unlike
// EvMessageNew — which the poll fires only for messages NEWER than the latest ts
// we already hold — this carries the WHOLE head page so the open MessageList can
// merge it. That fills a *middle* gap: a run of messages the shared socket's
// round-robin steal dropped, which then got buried under a later message that
// arrived normally. Once a newer message exists, the "newer than latest" filter
// can never recover the buried run, so nothing did — the gap sat forever. The
// merge (mergeNetworkMessages, fromHeadPage) inserts them in order and also
// reconciles edits/deletions on the head. Open conversation only; the poll fires
// it foreground-only, where a MessageList is actually showing these rows.
struct EvHeadRefresh {
    ConversationId       conv;
    std::vector<Message> messages;
    // Session's shared clock, captured before the history request starts.
    quint64              requestRevision = 0;
};
struct EvReactionAdded {
    ConversationId conv;
    Ts             ts;
    QString        name;
    UserId         user;
};
struct EvReactionRemoved {
    ConversationId conv;
    Ts             ts;
    QString        name;
    UserId         user;
};
struct EvConvMarked {
    ConversationId conv;
    Ts             lastRead;
    int            unread;
    int            mentionCount = 0;
};
struct EvTyping {
    ConversationId conv;
    UserId         user;
};
struct EvPresenceChanged {
    UserId user;
    bool   active;
};
struct EvDndChanged {
    UserId user;
    bool   dndEnabled;
};
struct EvChannelCreated {
    Conversation conv;
};
struct EvMemberJoined {
    ConversationId conv;
    UserId         user;
};
// A member updated their profile/data (display name, status, title, or
// avatar). Carries the complete refreshed User; Session merges it into the
// cache while preserving live presence/DND (which this event doesn't carry).
struct EvUserChanged {
    User user;
};
// A batch of EvUserChanged-style updates delivered as ONE event. Emitted when
// many users refresh at once (e.g. the IMAP domain-icon resolver upgrading
// avatars for hundreds of senders as probes complete): Session pays one roster
// merge + one cache write + one re-emission instead of per-user — firing these
// individually froze the UI, exactly like the per-user loadUsers storm did.
// Same merge semantics as EvUserChanged (live presence/DND preserved).
struct EvUsersChanged {
    std::vector<User> users;
};
// The workspace's user groups changed (Slack subteam_* events: created,
// renamed, membership edited, self added/removed). Carries nothing: the
// Session re-fetches the list, which is small and one request.
struct EvUsergroupsChanged {};
// A sendMessage definitively failed (Slack rejected it — not a transport
// problem, those are retried). Session removes the optimistic copy and
// surfaces the reason to the user.
struct EvSendFailed {
    ConversationId conv;
    QString        reason; // Slack error string, e.g. "not_in_channel"
};
// A huddle started or ended in a conversation. Derived from the huddle_thread
// message event (USLACKBOT posts/edits one in the conversation as the room's
// state changes); carries the channel and live/ended state. Session patches
// Conversation::huddleActive from it.
struct EvHuddleChanged {
    ConversationId      conv;
    bool                active = false;
    QString             link;         // room.huddle_link
    std::vector<UserId> participants; // current participants, or [host]
};
// The realtime websocket re-established after a gap (network blip, server
// recycle, zombie-socket watchdog, sleep/wake). Slack's Socket Mode does NOT
// replay events missed while disconnected, so anything posted during the gap
// (own sends, others' messages) is absent from the live view until a refetch.
// App-level (the socket is shared by all workspaces), so it carries no conv:
// every backend/UI re-syncs. Session refetches the conversation list (unread/
// latest badges); the open MessageList re-fetches + merges its history — i.e.
// exactly what leaving the chat and coming back already does. Not fired on the
// first connect (the initial load covers that).
struct EvRealtimeReconnected {};

// Slack is repeatedly evicting our live Socket Mode socket from the app's
// connection pool: it cleanly closes the socket (WebSocket close code 1000, no
// preceding "disconnect" envelope) several times in a short window, or sends an
// explicit too-many-connections disconnect. Both mean another client is sharing
// this app's ≤10-connection pool — Socket Mode connections are keyed by the
// app-level xapp token, which is compiled into every msga build, so a second
// instance anywhere (another device, a coworker, a dev/release build) churns the
// same pool and Slack round-robins us out. The result is a reconnect storm rather
// than a transport failure. App-level (the socket is shared by all workspaces),
// so it carries no conv; surfaced so the UI can name the cause instead of the
// user seeing an unexplained flapping connection. See socket_mode_realtime.h.
//
// `otherConnections` is how many connections in the app's pool are NOT ours,
// read straight from the `hello` frame's num_connections (0 when the count is
// unknown — e.g. detected only from a burst of bare closes, before any hello).
// Since msga is single-instance per user (SingleInstance), those connections
// cannot be a second local copy: they are another device/account on the same
// compiled-in xapp token. The UI uses this to state the cause is not local.
struct EvRealtimeContended {
    int otherConnections = 0;
};

// The presence-holding connection (see PresenceMode) changed state. Raised by
// the backend; the Session records it (Session::presenceLink()) and re-polls the
// rich self presence, since Slack flips `online`/`active` as the socket comes
// and goes.
struct EvPresenceLinkChanged {
    PresenceLinkState state = PresenceLinkState::Off;
};

// An API request hit HTTP 429 and is being transparently retried after
// `retryAfterSecs`. Informational — the call still completes; the UI can show a
// transient "rate-limited" notice. `method` is the throttled API method.
struct EvRateLimited {
    QString method;
    int     retryAfterSecs = 0;
};

// A message reminder came due. Raised by the Session's local timer, not by any
// backend transport — Slack delivers NOTHING when a saved-item reminder fires
// (no Slackbot DM, no event; verified), official clients alarm from their own
// state. Carries everything the notification needs so the UI never has to look
// the reminder back up: threadRoot routes the click into the thread when the
// reminded message is a reply, snippet is the stored message preview, and
// author/bot* identify who wrote it (for the notification's name and avatar).
// All enrichment fields may be empty for a reminder set from another client —
// the server item carries none of them.
struct EvReminderDue {
    ConversationId conv;
    Ts             ts;
    Ts             threadRoot;
    QString        snippet;
    UserId         author;
    QString        botName;
    QString        botAvatarUrl;
};

// --- Search ---

struct SearchResult {
    ConversationId conv;
    QString        convName;
    Message        msg;
    bool           operator==(const SearchResult &) const = default;
};

// --- Slash commands ---

// A slash command available in the workspace ("/remind", an app's "/github", …).
// Built-in Slack commands have an empty appId.
struct SlashCommand {
    QString name;    // without the leading slash, e.g. "remind"
    QString desc;    // human-readable description
    QString usage;   // argument hint, e.g. "[@someone or #channel] [what] [when]"
    QString appId;   // owning app ID for app commands; empty for core commands
    QString appName; // owning app display name ("Giphy"); empty for core commands
    QString iconUrl; // owning app icon URL; empty → fall back to the generic mark
    bool    operator==(const SlashCommand &) const = default;
};

using Event = std::variant<
    EvMessageNew,
    EvMessageChanged,
    EvMessageDeleted,
    EvAttachmentRemoved,
    EvHeadRefresh,
    EvReactionAdded,
    EvReactionRemoved,
    EvConvMarked,
    EvTyping,
    EvPresenceChanged,
    EvDndChanged,
    EvChannelCreated,
    EvMemberJoined,
    EvUserChanged,
    EvUsersChanged,
    EvUsergroupsChanged,
    EvSendFailed,
    EvHuddleChanged,
    EvRealtimeReconnected,
    EvRealtimeContended,
    EvPresenceLinkChanged,
    EvRateLimited,
    EvReminderDue>;
