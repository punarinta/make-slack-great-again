// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Normalized domain types — the ONLY types that cross the Backend seam upward.
// No Slack JSON, no HTTP types, no xoxp tokens above this layer.
#pragma once

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

// --- Enumerations ---

enum class ConvKind { PublicChannel, PrivateChannel, Im, Mpim };
enum class AuthState { NotLoggedIn, LoggingIn, LoggedIn };
enum class NotificationLevel { Default, All, Mentions, Mute };

struct Capabilities {
    bool typing       = false;
    bool livePresence = false;
    bool huddles      = false;
};

// --- Core domain structs ---

struct User {
    UserId  id;
    QString name;
    QString displayName;
    QString avatarUrl;
    bool    isBot         = false;
    bool    isActive      = false; // presence; polled on public path
    bool    isDeactivated = false; // Slack "deleted" flag
    bool    isAdmin       = false; // is_admin || is_owner from users.list
    bool    dndEnabled    = false; // do-not-disturb; updated via dnd_updated_user event
    QString statusEmoji;           // Slack emoji name without colons, e.g. "palm_tree"
    QString statusText;            // user status text, e.g. "On vacation"
    bool    operator==(const User &) const = default;
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
    bool                  isMuted                                = false;
    bool                  isStarred                              = false;
    NotificationLevel     notifLevel                             = NotificationLevel::Default;
    bool                  operator==(const Conversation &) const = default;
};

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

// File shared in a Slack message (from the "files" array).
struct File {
    QString id;
    QString name;
    QString mimeType;
    QString prettyType; // human-readable type, e.g. "PDF", "Word Document"
    QString urlPrivate; // url_private: auth header required for download
    QString permalink;  // Slack web UI URL — no auth required, opens in browser
    QString thumbUrl;   // thumbnail URL (e.g. thumb_360); auth required
    int     imageWidth  = 0;
    int     imageHeight = 0;
    qint64  size        = 0;

    bool isImage() const { return mimeType.startsWith("image/") && imageWidth > 0; }
    bool operator==(const File &) const = default;
};

// Simplified Block Kit block. Covers the common types needed for rendering.
// rich_text, section, header, context → text field populated.
// image → imageUrl/altText populated; text may be empty.
// divider → typeStr == "divider", rest empty.
struct Block {
    QString typeStr;       // "section"|"header"|"divider"|"image"|"context"|"rich_text"|"actions"
    TextWithEntities text; // primary displayable text
    QString          imageUrl; // for "image" blocks
    QString          altText;  // for "image" blocks
    bool             operator==(const Block &) const = default;
};

// Legacy Slack attachment (link unfurls, bot messages, older integrations).
struct Attachment {
    QString            fallback;
    QString            color; // "#rrggbb" left-border accent; may be empty
    QString            pretext;
    QString            authorName;
    QString            title;
    QString            titleLink;
    TextWithEntities   text;
    QString            imageUrl;
    QString            thumbUrl;
    QString            faviconUrl; // service_icon URL (favicon for link previews)
    QString            footer;
    std::vector<Block> blocks; // Block Kit blocks embedded in this attachment
    bool               operator==(const Attachment &) const = default;
};

// --- Messages ---

struct Message {
    Ts                    ts;
    std::optional<Ts>     threadRoot;     // set when message is in a thread (reply)
    int                   replyCount = 0; // >0 on thread root messages
    std::vector<UserId>   replyUsers;     // participants (up to 5, from reply_users)
    std::optional<Ts>     latestReply;    // ts of the most recent reply
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
    bool                    operator==(const Message &) const = default;
};

struct MessagePage {
    std::vector<Message>   messages;
    std::optional<QString> olderCursor; // pass to next loadHistory call
    bool                   operator==(const MessagePage &) const = default;
};

struct OutgoingMessage {
    TextWithEntities  text;
    QString           rawText; // original mrkdwn source; sent verbatim to chat.postMessage
    std::optional<Ts> threadRoot;
};

// --- Realtime events (normalized from both Socket Mode and internal ws) ---

struct EvMessageNew {
    ConversationId conv;
    Message        msg;
};
struct EvMessageChanged {
    ConversationId conv;
    Message        msg;
};
struct EvMessageDeleted {
    ConversationId conv;
    Ts             ts;
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

// --- Search ---

struct SearchResult {
    ConversationId conv;
    QString        convName;
    Message        msg;
    bool           operator==(const SearchResult &) const = default;
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
    EvMemberJoined>;
