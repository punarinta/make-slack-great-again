// The data model: plain structs owned by model::Store (store.h).
//
// Memory rules: users and conversations are referred to by a 32-bit index
// into the Store (UserRef / ConvRef) instead of repeating their id strings in
// every message and reaction; a message keeps only its raw mrkdwn (the view
// parses it when it lays the message out, and caches the layout, not a second
// copy of the text); rarely present parts (files, attachments, bot identity)
// live behind one pointer so an ordinary message stays small.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace model {

// A Slack message timestamp, "1758445200.123456", held as epoch microseconds.
// It is the message's identity AND its time AND its sort key — Slack's ts is
// all three, and an int compares and stores far cheaper than the string.
// Backends whose ids are not clocks synthesise a unique, ordered Ts.
using Ts = int64_t;
Ts             parseTs(std::string_view s); // 0 when malformed
std::string    formatTs(Ts ts);             // "1758445200.123456"
inline int64_t tsSecs(Ts ts) {
    return ts / 1000000;
}

using UserRef             = uint32_t;
using ConvRef             = uint32_t;
constexpr UserRef kNoUser = UINT32_MAX;
constexpr ConvRef kNoConv = UINT32_MAX;

struct User {
    std::string id;          // "U0MIRA"
    std::string name;        // handle: "mira"
    std::string displayName; // the shown name, "Mira Okafor" or "Mira"; may be empty
    // The two names a service keeps apart (Slack's real_name and
    // display_name; either may be empty): displayName is made from them per
    // Store::realNames(). Both empty: displayName is the backend's own.
    std::string realName, profileName;
    std::string title;
    std::string email;
    std::string avatar;      // local path or URL
    std::string statusEmoji; // shortcode without colons
    std::string statusText;
    int32_t     tzOffset    = 0; // seconds east of UTC
    bool        hasTz       = false;
    bool        active      = false; // presence
    bool        bot         = false;
    bool        admin       = false;
    bool        owner       = false;
    bool        dnd         = false;
    // Interned from a reference (mention, reaction) before the roster knew
    // them; a later addUser() fills the record in place.
    bool        placeholder = false;
    // Agent sessions: there, yet not reachable from here (a terminal drives
    // it, or it waits on an approval) — the yellow dot.
    bool        unavailable = false;
    // Deactivated, or a Slack Connect stranger (conversations.open rejects
    // them): never offered as someone to message (Find a channel's People).
    bool        deleted = false, stranger = false;

    // What the UI shows: the display name, else the handle, else the id.
    std::string_view label() const;
    // An @mention inside message text: the profile name whatever
    // realNames() says (as Slack draws it), else label().
    std::string_view mentionLabel() const;
    // displayName from realName / profileName (no-op while both are empty):
    // full names first, or the profile name first; the handle last.
    void             resolveName(bool realNames);
};

enum class ConvKind : uint8_t { Channel, Private, Dm, Group };

// Which new messages notify ("Notify you about…"); `muted` is separate
// and silences everything, the badge included. Default follows the global
// setting; an explicit All overrides a global "Just mentions". The values are
// persisted (workspace cache, Claude Code teammates): 0 is what caches written
// before Default existed stored for an untouched conversation.
enum class NotifyLevel : uint8_t { Default, Mentions, Nothing, All };

struct Reaction {
    std::string          name;      // shortcode, may carry "::skin-tone-N"
    uint32_t             count = 0; // can exceed users.size() (Slack truncates the list)
    std::vector<UserRef> users;
    bool                 operator==(const Reaction &) const; // store.cpp (all equality is)
};

struct File {
    std::string id;
    std::string name;
    std::string mime;       // "image/png"
    std::string prettyType; // "PNG", "PDF", "Plain text"
    std::string path;       // local path or URL of the content (an image's: a thumbnail)
    // The original upload when `path` is something else (Slack: an image's
    // thumbnail, a voice clip's AAC transcode): what Download, Copy full
    // image and Forward fetch. "" = path is the original.
    std::string original;
    std::string permalink;     // the file's page (Slack), what "Copy link to file" copies
    std::string subtype;       // "slack_audio" for voice clips
    std::string transcript;    // voice clips: Slack's preview line
    std::string transcriptVtt; // …and its WebVTT (an auth download); "" = none
    // A non-image file's prerendered picture (a PDF's first page, Slack's
    // thumb_pdf): shown and opened like an image; width/height are its size.
    std::string thumb;
    // A canvas's display title (Slack: the file's title, emoji and mentions
    // kept); "" = the name.
    std::string title;
    int64_t     size  = 0;
    int32_t     width = 0, height = 0; // images, thumb
    int64_t     durationMs = 0;        // audio / video

    bool isImage() const { return mime.compare(0, 6, "image/") == 0 && width > 0; }
    bool isAudio() const { return mime.compare(0, 6, "audio/") == 0 || subtype == "slack_audio"; }
    bool isPdf() const { return mime == "application/pdf"; }
    // A picture to show inline (an image, a PDF's page).
    bool hasPreview() const { return isImage() || (isPdf() && !thumb.empty() && width > 0); }
    // Slack canvases (quip docs): a preview card, opened in the canvas viewer.
    bool isCanvas() const { return mime == "application/vnd.slack-docs"; }
    bool isHtml() const;
    const std::string &source() const { return original.empty() ? path : original; }
    bool               operator==(const File &) const;
};

struct AttachmentField {
    std::string title;
    std::string value; // mrkdwn
    bool        operator==(const AttachmentField &) const;
};

// A Block Kit block drawn as a structure rather than as text: kept only for messages and
// attachments whose blocks have a header, divider, image or table; everything else is mrkdwn in
// `text`.
struct Block {
    enum class Kind : uint8_t { Text, Header, Divider, Image, Table };
    Kind                                  kind = Kind::Text;
    std::string                           text;  // Text/Header: mrkdwn; Image: its title
    std::string                           image; // Image: URL or path
    std::string                           alt;   // Image: alt text (shown without a URL)
    int32_t                               width = 0, height = 0;
    std::vector<std::vector<std::string>> rows; // Table: mrkdwn cells, row 0 the header
    bool                                  operator==(const Block &) const;
};

// Legacy attachments and link previews (unfurls): Slack sends both as
// "attachments"; linkPreview picks the compact card.
struct Attachment {
    std::string                  color; // "#3FCB8E" (the side bar)
    std::string                  pretext, author, title, link;
    std::string                  text; // mrkdwn
    std::string                  service, favicon, footer;
    std::string                  image; // local path or URL
    int32_t                      imageWidth = 0, imageHeight = 0;
    std::vector<AttachmentField> fields;
    bool                         linkPreview = false;
    // A shared message (Slack's is_msg_unfurl): author, text and files are
    // the quoted message's; drawn as a card with "Posted in #channel".
    bool                         msgUnfurl   = false;
    bool                         app         = false; // the quoted author is an app (APP tag)
    int32_t                      id          = 0;     // Slack's positional id (1-based; 0 = none)
    std::string                  authorIcon;          // the quoted author's avatar
    std::string                  channel;             // where the quoted message lives
    Ts                           ts = 0;              // the quoted message's time
    std::vector<Block>           blocks;              // structural blocks (see Block)
    std::vector<File>            files;               // the quoted message's files
    bool                         operator==(const Attachment &) const;
};

// A button under a message (Claude Code's permission question: its options);
// pressing one is Backend::pressButton.
struct Button {
    enum class Style : uint8_t { Default, Primary, Danger };
    std::string id;    // what the backend is told (Slack: the action_id; "" = a legacy
                       // attachment button, which no third-party client can press)
    std::string label; // shown
    Style       style = Style::Default;
    // Slack's Block Kit buttons: a link button opens `url` instead of being
    // pressed; the others are pressed with their block and value.
    std::string url, blockId, value;
    // The attachment it belongs to, 1-based (drawn in that card, as Slack
    // does); 0 = the message's own.
    int32_t     attachment = 0;
    bool        operator==(const Button &) const;
};

// A huddle_thread message's summary: who was in it and
// for how long, from its room.
struct Huddle {
    std::vector<UserRef> attendees; // live: the participants; ended: everyone who was
    int64_t              startSec = 0, endSec = 0;
    bool                 ended = false;
    bool                 operator==(const Huddle &) const;
};

// The parts most messages don't have.
struct MessageExtras {
    std::string             subtype; // "bot_message", "channel_join", …
    std::string             botName, botAvatar;
    std::vector<File>       files;
    std::vector<Attachment> attachments;
    std::vector<Button>     buttons;
    std::vector<Block>      blocks; // drawn instead of `text` when present (see Block)
    std::string             botId;  // Slack: the posting app's bot id (a button press names it)
    Huddle                  huddle; // subtype "huddle_thread": its room's summary

    // Out of line (store.cpp): the implicit ones would inline a dozen string
    // and vector destructors into every function that drops a message.
    MessageExtras();
    MessageExtras(const MessageExtras &);
    ~MessageExtras();
    bool operator==(const MessageExtras &) const;
};

struct Message {
    Ts                    ts          = 0;
    Ts                    threadTs    = 0; // replies: the root's ts; 0 on top-level messages
    Ts                    latestReply = 0; // roots: newest reply
    UserRef               user        = kNoUser;
    UserRef               pinnedBy    = kNoUser; // "Pinned by …" (kNoUser: unknown)
    uint32_t              replyCount  = 0;       // roots
    std::string           text;                  // raw mrkdwn, exactly as sent
    bool                  edited  = false;
    bool                  pinned  = false;
    bool                  saved   = false; // in my "Save for later" list
    bool                  pending = false; // optimistic local copy while a send is in flight
    std::vector<UserRef>  replyUsers;      // roots: up to 5 participants
    std::vector<Reaction> reactions;
    std::unique_ptr<MessageExtras> extra;

    bool                           isReply() const { return threadTs != 0 && threadTs != ts; }
    bool                           isHuddle() const; // subtype huddle_thread (extra->huddle)
    MessageExtras                 &extras();         // creates on first use
    const std::string             &subtype() const;
    const std::vector<File>       &files() const;
    const std::vector<Attachment> &attachments() const;

    // Special members out of line for size (see MessageExtras).
    Message();
    Message(Message &&) noexcept;
    Message &operator=(Message &&) noexcept;
    ~Message();
    Message clone() const; // deep copy (messages are move-only to avoid accidental copies)
    // Every field and part equal (a refetched page's unchanged messages).
    bool    operator==(const Message &) const;
};

struct Thread {
    Ts                   root = 0;
    std::vector<Message> replies; // oldest first
};

// A new metadata field also goes into store.cpp's sameMeta (addConversation
// emits Meta only when one of them changed).
struct Conversation {
    std::string          id;
    std::string          name; // channel name without '#'; DMs: the peer's handle
    std::string          topic;
    ConvKind             kind   = ConvKind::Channel;
    UserRef              dmUser = kNoUser; // Dm peer
    std::vector<UserRef> members;          // Group participants (me included)
    uint32_t             memberCount = 0;
    uint32_t             unread      = 0; // top-level messages after lastRead, not mine
    uint32_t             mentions    = 0; // of those, the ones that mention me
    Ts                   lastRead    = 0;
    Ts                   latest      = 0;
    bool                 starred     = false;
    bool                 muted       = false;
    bool                 member      = true;  // channels: joined; DMs: open (not closed)
    bool                 extShared   = false; // a Slack Connect channel, shared with another org
    NotifyLevel          notify      = NotifyLevel::Default;
    std::string          canvasTitle; // non-empty: the channel has a canvas
    std::string          canvasId;    // its file ("F0123"); "" while unknown or none
    std::string          localName;   // "Name conversation…": only this client shows it
    // Why nobody can write here ("This session is running in a terminal…"):
    // the composer shows it instead of taking text. "" = writable.
    std::string          readOnly;
    // A live huddle (Capabilities::huddles): its link (Slack's huddle_link,
    // "" = unknown) and who is in it (or only its host before anyone joined).
    bool                 huddleActive = false;
    std::string          huddleLink;
    std::vector<UserRef> huddleParticipants;

    std::vector<Message> messages;             // top-level, oldest first, unique ts
    std::vector<Thread>  threads;              // loaded threads, any order
    bool                 hasMoreBefore = true; // older history exists on the server

    bool        isDirect() const { return kind == ConvKind::Dm || kind == ConvKind::Group; }
    // The one rule for notifications and badge
    // colours: muted is Nothing, Default is `fallback` (the global level).
    NotifyLevel effectiveNotify(NotifyLevel fallback) const {
        return muted ? NotifyLevel::Nothing : notify == NotifyLevel::Default ? fallback : notify;
    }
};

// Someone typing right now (typing events are transient; see Store::setTyping).
struct Typing {
    UserRef user    = kNoUser;
    Ts      thread  = 0;
    // An agent at work: "thinking (1m 5s)" counted from here (epoch ms);
    // 0 = plain typing.
    int64_t sinceMs = 0;
};

} // namespace model
