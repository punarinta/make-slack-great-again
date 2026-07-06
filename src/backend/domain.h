// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Normalized domain types — the ONLY types that cross the Backend seam upward.
// No Slack JSON, no HTTP types, no xoxp tokens above this layer.
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <variant>
#include <vector>

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
enum class Service { Slack, Teams, Imap /*, Telegram, … */ };

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
    bool fileUpload       = false; // upload + share files
    bool scheduledSend    = false; // send a message at a future time (chat.scheduleMessage).
                                   // Slack-only: Teams' Graph has no delegated scheduled-send and
                                   // SMTP has no native one, so the composer's schedule-send
                                   // dropdown is gated on this to avoid a dead control.
    bool messageSubjects  = false; // per-message subject line (email); shows the composer subject
                                   // field — see imap-backend-plan §3/§4
    bool collapseQuotedReplies =
        false; // email: a reply's trailing quoted history + signature is
               // the previous message(s) already shown above, so collapse
               // it behind a "show quoted text" toggle (messenger, not mail
               // client). Chat services quote intentionally → leave false.
    bool operator==(const Capabilities &) const = default;
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
    NotificationLevel     notifLevel   = NotificationLevel::Default;
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
// mention or a broadcast keyword (<!here>, <!channel>, <!everyone>). This is
// what the official Slack client treats as a mention for red badges and
// notifications.
inline bool mrkdwnMentions(const QString &mrkdwn, const UserId &me) {
    if (!me.value.isEmpty()) {
        const QString tag = QStringLiteral("<@") + me.value;
        for (qsizetype i = mrkdwn.indexOf(tag); i >= 0; i = mrkdwn.indexOf(tag, i + 1)) {
            const qsizetype after = i + tag.size();
            if (after < mrkdwn.size() && (mrkdwn[after] == u'>' || mrkdwn[after] == u'|'))
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

// File shared in a Slack message (from the "files" array).
struct File {
    QString                id;
    QString                name;
    QString                mimeType;
    QString                prettyType; // human-readable type, e.g. "PDF", "Word Document"
    QString                urlPrivate; // url_private: auth header required for download
    QString                permalink;  // Slack web UI URL — no auth required, opens in browser
    QString                thumbUrl;   // thumbnail URL (e.g. thumb_360); auth required
    int                    imageWidth  = 0;
    int                    imageHeight = 0;
    qint64                 size        = 0;
    std::vector<FileThumb> thumbs; // thumbnail ladder, ascending by width

    // Preview source covering physW physical pixels: the smallest thumbnail wide
    // enough, else the largest available (never the original — it can be huge),
    // else the legacy thumbUrl, else the original file.
    QString previewUrl(int physW) const {
        for (const auto &t : thumbs)
            if (t.width >= physW)
                return t.url;
        if (!thumbs.empty())
            return thumbs.back().url;
        return thumbUrl.isEmpty() ? urlPrivate : thumbUrl;
    }

    bool isImage() const { return mimeType.startsWith("image/") && imageWidth > 0; }
    bool isPdf() const { return mimeType == "application/pdf"; }
    // True when Slack provides a prerendered preview image: the image itself, or the
    // server-rendered first page of a PDF (thumb_pdf) — no client-side rendering needed.
    bool hasPreview() const { return isImage() || (isPdf() && !thumbUrl.isEmpty()); }
    bool operator==(const File &) const = default;
};

// A bot button — from a Block Kit "actions"/"section" button element or a
// legacy attachment "actions" entry. Display-only: interactive callbacks need
// the app's interactivity endpoint (not reachable with public API tokens), so
// only buttons carrying a URL are clickable.
struct BotButton {
    QString text;
    QString url;   // empty for interactive-only buttons
    QString style; // ""|"primary"|"danger"
    bool    operator==(const BotButton &) const = default;
};

// Simplified Block Kit block. Covers the common types needed for rendering.
// rich_text, section, header, context → text field populated.
// image → imageUrl/altText populated; text may be empty.
// divider → typeStr == "divider", rest empty.
// actions → buttons populated; section may also carry an accessory button.
struct Block {
    QString typeStr; // "section"|"header"|"divider"|"image"|"context"|"rich_text"|"actions"
    TextWithEntities       text; // primary displayable text; for "image" blocks this is the title
    QString                imageUrl;        // for "image" blocks
    QString                altText;         // for "image" blocks
    int                    imageWidth  = 0; // for "image" blocks; 0 when not provided
    int                    imageHeight = 0;
    std::vector<BotButton> buttons; // "actions" elements / section accessory
    bool                   operator==(const Block &) const = default;
};

// Legacy Slack attachment (link unfurls, bot messages, older integrations).
// One entry of an attachment's "fields" array (bold title + mrkdwn value).
struct AttachmentField {
    QString          title;
    TextWithEntities value;
    bool             operator==(const AttachmentField &) const = default;
};

struct Attachment {
    QString                      fallback;
    QString                      color; // "#rrggbb" left-border accent; may be empty
    QString                      pretext;
    QString                      authorName;
    QString                      title;
    QString                      titleLink;
    TextWithEntities             text;
    QString                      imageUrl;
    QString                      thumbUrl;
    QString                      faviconUrl; // service_icon URL (favicon for link previews)
    QString                      footer;
    int                          imageWidth  = 0; // image_url dimensions; 0 when not provided
    int                          imageHeight = 0;
    int                          thumbWidth  = 0; // thumb_url dimensions; 0 when not provided
    int                          thumbHeight = 0;
    std::vector<AttachmentField> fields;  // bold-titled key/value rows (classic bot format)
    std::vector<Block>           blocks;  // Block Kit blocks embedded in this attachment
    std::vector<BotButton>       buttons; // legacy "actions" buttons (classic bot format)
    bool                         operator==(const Attachment &) const = default;

    // Preview source covering physW physical pixels: the thumbnail when it is
    // large enough (or its size is unknown), the full image otherwise.
    QString previewUrl(int physW) const {
        if (thumbUrl.isEmpty() || imageUrl.isEmpty())
            return thumbUrl.isEmpty() ? imageUrl : thumbUrl;
        return (thumbWidth > 0 && thumbWidth < physW) ? imageUrl : thumbUrl;
    }
};

// --- Messages ---

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
    TextWithEntities      text;
    QString               rawText; // original mrkdwn from Slack; used for edit pre-fill
    std::vector<Reaction> reactions;
    bool                  edited = false;
    std::optional<QString>  subtype;        // "bot_message", "channel_join", etc.
    std::vector<File>       files;          // Phase 3
    std::vector<Block>      blocks;         // Phase 3
    std::vector<Attachment> attachments;    // Phase 3
    bool                    pinned = false; // true if pinned to channel
    UserId                  pinnedBy;       // user who pinned it
    // Local optimistic copy shown while the send/upload is in flight; rendered
    // translucent and replaced by the real message once the server confirms.
    bool                    pending                           = false;
    bool                    operator==(const Message &) const = default;
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
// avatar and name) — are NOT system events.
inline bool isSystemEvent(const Message &m) {
    if (!m.subtype)
        return false;
    static const QSet<QString> kSystemSubtypes = {
        QStringLiteral("channel_join"),      QStringLiteral("channel_leave"),
        QStringLiteral("channel_topic"),     QStringLiteral("channel_purpose"),
        QStringLiteral("channel_name"),      QStringLiteral("channel_archive"),
        QStringLiteral("channel_unarchive"), QStringLiteral("group_join"),
        QStringLiteral("group_leave"),       QStringLiteral("group_topic"),
        QStringLiteral("group_purpose"),     QStringLiteral("group_name"),
        QStringLiteral("group_archive"),     QStringLiteral("group_unarchive"),
        QStringLiteral("pinned_item"),       QStringLiteral("unpinned_item"),
        QStringLiteral("bot_add"),           QStringLiteral("bot_remove"),
        QStringLiteral("huddle_thread"),
    };
    return kSystemSubtypes.contains(*m.subtype);
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

struct MessagePage {
    std::vector<Message>   messages;
    std::optional<QString> olderCursor; // pass to next loadHistory call
    bool                   operator==(const MessagePage &) const = default;
};

struct OutgoingMessage {
    TextWithEntities  text;
    QString           rawText; // original mrkdwn source; sent verbatim to chat.postMessage
    std::optional<Ts> threadRoot;
    // Latest server ts known for the conversation when the send started.
    // Anchors the duplicate-check window when a send must be reconciled after
    // a connection loss (server-assigned, so immune to local clock skew).
    Ts                sinceTs;
    // Per-message subject (email backends, gated by Capabilities::messageSubjects;
    // empty for chat services). On a reply the backend inherits the thread subject.
    QString           subject;
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

// An API request hit HTTP 429 and is being transparently retried after
// `retryAfterSecs`. Informational — the call still completes; the UI can show a
// transient "rate-limited" notice. `method` is the throttled API method.
struct EvRateLimited {
    QString method;
    int     retryAfterSecs = 0;
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
    EvSendFailed,
    EvHuddleChanged,
    EvRealtimeReconnected,
    EvRateLimited>;
