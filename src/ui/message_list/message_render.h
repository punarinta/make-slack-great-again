// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "util/slack_links.h"
#include <QString>
#include <QStringList>
#include <QColor>
#include <QDate>
#include <QHash>
#include <QRectF>
#include <QSet>
#include <QUrl>
#include <QVector>
#include <algorithm>
#include <vector>

class QPainter;
class QRect;
class QTextBrowser;
class QTextDocument;
class Session;

// Pure rendering helpers shared between message_list.cpp and message_list_paint.cpp.
// No widget state — takes only domain types and an optional Session* for name lookups.
namespace MsgRender {

QString resolveEmoji(const QString &name);

// Rich emoji resolution: built-in names resolve to `unicode`; workspace custom
// emojis (and aliases to them) resolve to `imageUrl`. Unknown names fall back
// to unicode = ":name:".
struct EmojiResolved {
    QString unicode;
    QString imageUrl;
    // False when the name matched nothing (unicode is the ":name:" placeholder):
    // callers must render it as ordinary text, never in the emoji font.
    bool    resolved = true;
};
EmojiResolved resolveEmojiRich(const QString &name, const Session *session);
// Same, against an explicit custom-emoji map (name → URL or "alias:name").
EmojiResolved resolveEmojiRich(const QString &name, const QHash<QString, QString> &customMap);

// Logical pixel size of inline emoji — exactly one Slack line-height,
// matching the official client (22px at a 15px body font).
int inlineEmojiPx();

// Default stylesheet for message/attachment QTextDocuments: Slack-ratio line
// height (22/15 of the font pixel size) computed for the active font.
QString docStyleSheet();

// All image URLs a message's docs reference as <img>: custom emoji plus Block
// Kit image-block urls (top-level and attachment-embedded), deduplicated.
// Used to register QTextDocument image resources and trigger downloads.
// `mediaUrls`, when given, receives the subset that is in-message media (the
// image blocks — Giphy and pasted GIFs) rather than emoji or icons, so a
// caller can apply the per-kind animation settings.
QStringList collectEmojiImageUrls(
    const Message &msg,
    const Session *session,
    bool           showLinkPreviews = true,
    QSet<QString> *mediaUrls        = nullptr
);

// Slack can deliver pasted images and GIF-picker results as link-unfurl
// attachments. Media-only attachments remain message content when ordinary
// preview cards are disabled; classification is based on their shape rather
// than a filename extension because CDN URLs are often opaque.
bool isMediaAttachment(const Attachment &att);

// Context for rendering Block Kit "image" blocks inline (Slack GIF/Giphy
// messages). When provided, image blocks emit a title line ("GIF ▾", a
// collapse-toggle anchor) followed by the real <img>; the caller registers the
// pixmap/movie frames as doc resources. When absent (preview dialogs), image
// blocks fall back to their italic alt text.
struct GifRenderContext {
    QString              keyPrefix;           // message ts, + "/a<idx>" inside attachment docs
    const QSet<QString> *collapsed = nullptr; // keys of user-collapsed images
    // Keys of shared-message unfurls the user expanded past their preview cut
    // (opposite default to `collapsed`: an unfurl body starts truncated). Null in
    // hosts that can't toggle — those render the preview with no "Show more".
    const QSet<QString> *expanded  = nullptr;
};

// Take Message::date (epoch microseconds), not a ts string — display reads the
// dedicated time field so non-Slack ids (which aren't clocks) still render.
QString formatTs(qint64 dateMicros);
QString formatFooterTs(qint64 dateMicros); // attachment footer: time today, date otherwise
int     footerFontPx();                    // attachment footer text, Slack's 12px on a 15px body
int     footerIconPx();                    // attachment footer_icon, Slack's 16px
QDate   tsToDate(qint64 dateMicros);
QString formatDateLabel(qint64 dateMicros);
// Slack-style absolute label for the reply bar: "today at 1:12 PM",
// "yesterday at 9:03 AM" or "March 3 at 4:15 PM".
QString lastReplyLabel(const Ts &ts);
// Message-row timestamp for digest views (the Threads overview): plain time for
// today, "yesterday at 9:03 AM" / "March 3 at 4:15 PM" for older messages —
// rows from different days sit side by side there, so time alone is ambiguous.
QString dateTimeLabel(qint64 dateMicros);
QString resolveMention(const QString &userId, const Session *session);
// Per-call overrides for inline runs toHtml emits. Anchors get their own inline
// style, so a caller shrinking/recolouring a whole passage (attachment footers)
// has to pass the same values here — a plain <span> around the output wouldn't
// reach into them.
struct InlineStyle {
    QColor linkColor;  // invalid → Th::c().text.link
    int    fontPx = 0; // 0 → inherit
};
QString toHtml(
    const TextWithEntities &twe, const Session *session = nullptr, const InlineStyle &style = {}
);

// The text shown inside a message-link chip: "#channel" for a channel, the peer's
// name for a DM, and a neutral "message" when the conversation isn't one of this
// workspace's (a link into another team).
QString messageLinkLabel(const SlackLinks::MessageRef &ref, const Session *session);

// True when the text renders at least one message-link chip — the doc owner uses
// this to decide whether the chip's icon has to be registered as a resource. The
// Message overload covers everything that ends up in its documents: the body,
// Block Kit blocks and legacy attachments.
bool hasMessageLink(const TextWithEntities &twe);
bool hasMessageLink(const Message &msg);

// Plain-text rendering for OS notifications / previews: resolves user and
// channel mentions to their display names and built-in emoji codes to their
// Unicode glyph (custom emoji, which can't render in a text-only notification,
// stay as ":name:"). No HTML, no markup — just readable text.
QString notificationText(const TextWithEntities &twe, const Session *session);

// Notification body for a whole message: the message text when it has any, and
// otherwise a flattened summary of its Block Kit blocks / legacy attachments so
// bot posts (CodePipeline, Amazon Q, GitHub, …) that leave `text` empty still
// show their content in the OS toast instead of a bare "Bot:".
QString notificationPreview(const Message &msg, const Session *session);

// User ids notificationPreview() would print raw ("@U0C3E7HGZHS") because the
// mention carries no label of its own — the caller resolves the ones the user
// cache doesn't know (Slack Connect / system / deactivated accounts users.list
// omits) before building a toast. A labeled mention ("<@U7|alice>") already
// reads as a name and is left out. Covers the pre-parsed texts (body, Block Kit
// blocks, attachment text and fields); mentions inside an attachment's
// pretext/fallback are parsed only while the preview is built and stay out.
std::vector<UserId> notificationRawMentions(const Message &msg);

// Geometry (doc coordinates, margins excluded) of every ``` code-block table in a
// laid-out message document.
QVector<QRectF> codeBlockRects(const QTextDocument *doc);
// Rounded background + border behind ``` code blocks. Qt rich text has no
// border-radius, so callers paint this under the document, with the painter
// already translated to the doc origin.
void            paintCodeBlockChrome(QPainter &p, const QTextDocument *doc);

// Geometry of every bot-button cell in a laid-out message document, and the
// rounded button face (background + border) painted underneath them — same
// pattern as the code-block chrome. Call wherever paintCodeBlockChrome is called.
// `hoverPos` is the mouse in document coordinates (null when the pointer isn't
// over this document); the button containing it gets its hover face.
QVector<QRectF> botButtonRects(const QTextDocument *doc);
void            paintBotButtonChrome(
    QPainter &p, const QTextDocument *doc, QPointF hoverPos = QPointF(-1, -1)
);

// Inline table messages cap at this many rows (the last one shaded) — the full
// table opens in the TableViewerOverlay, like the official client.
inline constexpr int kMaxInlineTableRows = 10;

// CSV "Preview" caps the table viewer at this many rows — QTextDocument table
// layout gets slow past a few hundred, and a runaway layout hangs the GUI
// thread (see the main-thread watchdog).
inline constexpr int kMaxCsvViewerRows = 400;

// HTML for a "table" block. maxRows <= 0 renders every row (the full-table
// viewer); otherwise the output is capped at maxRows and, when rows were cut,
// the last rendered row is shaded as a "there's more" cue.
QString tableBlockHtml(const Block &blk, const Session *session, int maxRows = -1);

// Parse CSV file bytes into a "table" Block for the TableViewerOverlay (the
// CSV file chip's "Preview" action). RFC 4180 quoting — quoted fields may
// contain the delimiter, newlines and doubled quotes — with the delimiter
// (comma / semicolon / tab) sniffed from the first line and a UTF-8 BOM
// stripped. Cells are plain text; no entities.
Block csvToTableBlock(const QByteArray &bytes);

// Geometry (doc coordinates) of every data table in a laid-out message document
// — the tables tableBlockHtml emits, identified by their border-collapse format
// (code blocks / blockquotes / button rows never set it). Drives the hover
// "Open full table" affordance.
QVector<QRectF> dataTableRects(const QTextDocument *doc);
// collapseQuotedReplies (email only — Capabilities::collapseQuotedReplies): strip
// the trailing quoted history + signature so a reply shows only what the sender
// added. Chat services pass false and keep their intentional quotes.
QString         buildMsgHtml(
    const Message          &msg,
    const Session          *session,
    const GifRenderContext *gif                   = nullptr,
    bool                    collapseQuotedReplies = false
);
QString buildAttachHtml(
    const Attachment &att, const Session *session, const GifRenderContext *gif = nullptr
);

// Body sentence of a huddle row (Message::huddle): "You and Ann were in the
// huddle for 8m." once it ended, "Ann is in the huddle." while it is live.
// Empty when the message carries no huddle summary.
QString huddleSummaryText(const Message &msg, const Session *session);
// "8m", "1h", "1h 5m" — Slack's huddle length label (minutes, at least 1).
QString huddleDurationLabel(qint64 seconds);

// Apply the shared "message preview" chrome to a read-only QTextBrowser (used by
// the delete / forward dialogs): no frame, transparent background, the app's thin
// rounded scrollbar (matching the chats list thumb), no focus stealing, and
// asymmetric text padding — sp.lg on the left so the text lines up with the card
// header, 0 on the right so it reaches the edge with only the scrollbar beside it.
// Call AFTER the content (setHtml / setPlainText) is set: the root-frame margins
// are applied to the populated document.
void configurePreviewBrowser(QTextBrowser *browser);

// True when the attachment renders nothing but Block Kit image blocks (the
// Slack GIF-picker shape) — official clients draw those without the colored
// quote bar and without the bar indent.
bool attachIsImageOnly(const Attachment &att);

// True when the attachment renders nothing but "table" blocks (Slack's table
// messages arrive as such an attachment). Official clients draw those without
// the colored quote bar / indent, and they are message content — not a link
// preview — so they get no dismiss "×" either.
bool attachIsTableOnly(const Attachment &att);

// True when the attachment gets the hover "×" that hides/removes it: only
// generated link previews (unfurls). Bot attachments — the colored-bar cards
// with buttons, fields, etc. — are the message's own content, and the official
// client offers no way to hide them.
bool attachIsDismissable(const Attachment &att);

// The attachment's left-bar color: its `color` as hex ("#rrggbb" or bare
// "rrggbb"), one of the legacy names "good"/"warning"/"danger", or the theme's
// default bar when empty or unparseable.
QColor attachmentBarColor(const Attachment &att);

// True when the attachment paints without the colored quote bar and without its
// indent: image-only, table-only, and shared-message unfurls (which draw their
// own bordered card instead). Paint, hit-testing and selection all key on this,
// so they stay in sync.
bool attachIsBarless(const Attachment &att);

// Padding between a shared-message unfurl card's border and its content. Only
// the quoted BODY is a document; the frame, the author header and the file chips
// are painted by MessageListWidget with the same painters message rows use.
inline constexpr int kUnfurlCardPad      = 10;
inline constexpr int kUnfurlCardRadius   = 8;
// The quoted body renders capped at this much text, with the rest behind "Show
// more" — as in the official client, and because a quoted bot post can run to
// hundreds of lines inside somebody else's message.
inline constexpr int kUnfurlPreviewChars = 400;
inline constexpr int kUnfurlPreviewLines = 6;

// Where a conversation is, as shown to the user: "#general", the peer's name for
// a DM, "group message" for an MPDM, and nothing when it isn't one this
// workspace can see. Cache-only, so it's safe on a paint path.
QString convPlaceLabel(const QString &convId, const Session *session);

QColor  fileTypeColor(const File &f);
QString fileIconLabel(const File &f);
QString formatFileSize(qint64 bytes);

// Inline-player state of an audio card (File::isAudio()). nullptr = idle and
// unhovered. Render stays independent of the media layer; the message list
// maps Media::AudioPlayer's status onto this.
struct AudioChipState {
    enum class Phase { Idle, Loading, Playing, Paused, Ended, Error };
    Phase   phase      = Phase::Idle;
    qint64  positionMs = 0;
    qint64  durationMs = 0;  // 0 = unknown
    qint64  scrubMs    = -1; // ≥0 while the user drags the slider: shown instead of positionMs
    QString error;           // Phase::Error
    // "Transcribe" button (right of the slider row): hovered, and busy while the
    // AI provider is working on this file.
    bool    transcribeHovered = false;
    bool    transcribing      = false;
};

// Paint a single non-image file chip into rect using the canonical message-list
// style. rect should be fileChipHeight(f) tall; width is clamped to kFileChipMaxW.
void paintFileChip(
    QPainter &p, const File &f, const QRect &rect, const AudioChipState *audio = nullptr
);

// Audio card geometry, in the coordinates of the rect given to paintFileChip:
// the round play/pause button, the slider track — sized from durationMs so its
// right edge doesn't jitter as the time label ticks — and the "Transcribe"
// button at the right end of the slider row (after the time label).
QRect audioChipButtonRect(const QRect &chipRect);
QRect audioChipBarRect(const QRect &chipRect, qint64 durationMs);
QRect audioChipTranscribeRect(const QRect &chipRect);
// The transcript line under the card (File::hasTranscript()): quote bar, the
// preview text elided to what fits, then the "View transcript" link. Rects are
// null when the file has no transcript.
struct AudioTranscriptLayout {
    QRect textRect;
    QRect linkRect;
};
AudioTranscriptLayout audioChipTranscriptLayout(const QRect &chipRect, const File &f);

// One WebVTT cue: start time and its text (speaker dashes/tags stripped).
struct VttCue {
    qint64  startMs = 0;
    QString text;
    bool    operator==(const VttCue &) const = default;
};
std::vector<VttCue> parseVtt(const QByteArray &vtt);
// "0:05", "12:34", "1:02:03". Position labels floor, duration labels round —
// a 4.98 s clip is "0:05" long but is at "0:04" while it plays.
QString             formatDuration(qint64 ms, bool round = false);

// Canonical chip dimensions — exposed so callers can size widgets correctly.
// Audio files get the taller player card; every layout walk over a message's
// chips must use fileChipHeight(f), never kFileChipH directly.
inline constexpr int kFileChipH    = 52;
inline constexpr int kAudioChipH   = 88;
inline constexpr int kTranscriptH  = 26; // transcript line under the audio card
inline constexpr int kFileChipMaxW = 380;
inline int           fileChipHeight(const File &f) {
    if (!f.isAudio())
        return kFileChipH;
    return kAudioChipH + (f.hasTranscript() ? kTranscriptH : 0);
}

// A canvas shared as a message file (File::isCanvas) renders as a preview card
// in the message list — header (icon, title, "Canvas") over the start of the
// document, clipped — instead of a file chip. Fixed height, so the row never
// jumps when the content download lands. Only the message's own files get the
// card (a file quoted inside an unfurl stays a chip), which is why the message
// list sizes its own files through these and not fileChipHeight().
inline constexpr int kCanvasCardH    = 300;
inline constexpr int kCanvasCardMaxW = 600;
inline constexpr int kCanvasCardHdrH = 60; // header strip, above the divider
inline int           messageFileHeight(const File &f) {
    return f.isCanvas() ? kCanvasCardH : fileChipHeight(f);
}
inline int messageFileMaxW(const File &f) {
    return f.isCanvas() ? kCanvasCardMaxW : kFileChipMaxW;
}

// User mentions are rendered as anchors with this internal scheme so they are
// hit-testable like links: href = kUserAnchorPrefix + userId.
inline const QString kUserAnchorPrefix = QStringLiteral("msga://user/");

// Returns the user ID when href is a user-mention anchor, else an empty string.
inline QString userIdFromAnchor(const QString &href) {
    return href.startsWith(kUserAnchorPrefix) ? href.mid(kUserAnchorPrefix.size()) : QString();
}

// Channel mentions are anchors with this internal scheme so clicking one
// navigates to the channel: href = kChannelAnchorPrefix + conversationId.
inline const QString kChannelAnchorPrefix = QStringLiteral("msga://channel/");

// Returns the conversation ID when href is a channel-mention anchor, else "".
inline QString channelIdFromAnchor(const QString &href) {
    return href.startsWith(kChannelAnchorPrefix) ? href.mid(kChannelAnchorPrefix.size())
                                                 : QString();
}

// Image-block title lines ("GIF ▾") are anchors with this internal scheme;
// clicking one toggles the collapse key that follows the prefix.
inline const QString kGifToggleAnchorPrefix = QStringLiteral("msga://gif/");

// Returns the collapse key when href is an image-block toggle anchor, else "".
inline QString gifKeyFromAnchor(const QString &href) {
    return href.startsWith(kGifToggleAnchorPrefix) ? href.mid(kGifToggleAnchorPrefix.size())
                                                   : QString();
}

// A shared-message unfurl's "Show more" / "Show less" line is an anchor with this
// scheme; clicking it toggles the expand key that follows the prefix (the key is
// the attachment's own "<ts>/a<idx>").
inline const QString kUnfurlToggleAnchorPrefix = QStringLiteral("msga://unfurl/");

// Returns the expand key when href is an unfurl "Show more" anchor, else "".
inline QString unfurlKeyFromAnchor(const QString &href) {
    return href.startsWith(kUnfurlToggleAnchorPrefix) ? href.mid(kUnfurlToggleAnchorPrefix.size())
                                                      : QString();
}

// True for the anchors that toggle inline state instead of navigating anywhere:
// they get no link underline on hover and no "Copy link" context menu.
inline bool isToggleAnchor(const QString &href) {
    return href.startsWith(kGifToggleAnchorPrefix) || href.startsWith(kUnfurlToggleAnchorPrefix);
}

// An EntityType::MessageLink renders as a chip rather than a raw URL (the
// official client does the same); clicking it jumps to that message instead of
// leaving for the browser. The href is the prefix plus the entity's own
// SlackLinks::refToToken() payload — the host travels with it so a link into a
// workspace we are not signed into can be rebuilt and handed to the browser.
inline const QString kMessageAnchorPrefix = QStringLiteral("msga://msglink/");

inline QString messageAnchor(const SlackLinks::MessageRef &ref) {
    return kMessageAnchorPrefix + SlackLinks::refToToken(ref);
}

// Returns the referenced message when href is a message-link anchor; an invalid
// ref (isValid() == false) otherwise.
inline SlackLinks::MessageRef messageRefFromAnchor(const QString &href) {
    if (!href.startsWith(kMessageAnchorPrefix))
        return {};
    return SlackLinks::refFromToken(href.mid(kMessageAnchorPrefix.size()));
}

// QTextDocument resource name for the icon inside a message-link chip. EVERY
// document that renders message HTML must register it (registerMessageLinkIcon)
// or the chip's <img> falls back to Qt's broken-image box.
inline const QString kMessageLinkIconRes = QStringLiteral("msga://msglink-icon/");
void                 registerMessageLinkIcon(QTextDocument *doc, qreal dpr);

// QTextDocument resource names for the chevron icon on image-block title lines.
// The doc owner registers theme-colored pixmaps under these urls.
inline const QString kGifChevronExpandedRes  = QStringLiteral("msga://gif-chevron/expanded");
inline const QString kGifChevronCollapsedRes = QStringLiteral("msga://gif-chevron/collapsed");

// Bot buttons are anchors with this internal scheme (even URL buttons — a raw
// URL href would pick up the link hover underline, and buttons aren't links).
// "url:<percent-encoded>" buttons open their URL on click; the rest carry no
// deliverable action — Slack routes bot-button callbacks only from its own
// clients — so the click handler explains that instead.
inline const QString kBotBtnAnchorPrefix = QStringLiteral("msga://botbtn/");

inline bool isBotButtonAnchor(const QString &href) {
    return href.startsWith(kBotBtnAnchorPrefix);
}

// The target URL of a bot-button anchor; empty for interactive-only buttons.
inline QString botButtonUrlFromAnchor(const QString &href) {
    if (!isBotButtonAnchor(href))
        return {};
    const QString rest = href.mid(kBotBtnAnchorPrefix.size());
    if (!rest.startsWith(QLatin1String("url:")))
        return {};
    return QUrl::fromPercentEncoding(rest.mid(4).toLatin1());
}

// Marker cell spacing identifying the bot-button container table in a
// QTextDocument (code blocks, blockquotes and data tables use 0); read back by
// botButtonRects(), which takes the buttons as the container's floating tables.
// Doubles as the gap between buttons.
inline constexpr int kBotBtnCellSpacing = 4;

// Anchor-name prefix marking a filled bot button's style ("danger"/"primary")
// on its label, read back by paintBotButtonChrome to pick the face.
inline const QString kBotBtnStyleNamePrefix = QStringLiteral("msga-btn-");

// Inline image-block size cap — matches the message list's kImgMaxW/kImgMaxH.
inline constexpr int kBlockImgMaxW = 400;
inline constexpr int kBlockImgMaxH = 300;

} // namespace MsgRender
