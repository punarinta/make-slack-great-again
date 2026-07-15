// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include <QString>
#include <QStringList>
#include <QColor>
#include <QDate>
#include <QHash>
#include <QRectF>
#include <QSet>
#include <QUrl>
#include <QVector>

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
QStringList collectEmojiImageUrls(const Message &msg, const Session *session);

// Context for rendering Block Kit "image" blocks inline (Slack GIF/Giphy
// messages). When provided, image blocks emit a title line ("GIF ▾", a
// collapse-toggle anchor) followed by the real <img>; the caller registers the
// pixmap/movie frames as doc resources. When absent (preview dialogs), image
// blocks fall back to their italic alt text.
struct GifRenderContext {
    QString              keyPrefix;           // message ts, + "/a<idx>" inside attachment docs
    const QSet<QString> *collapsed = nullptr; // keys of user-collapsed images
};

// Take Message::date (epoch microseconds), not a ts string — display reads the
// dedicated time field so non-Slack ids (which aren't clocks) still render.
QString formatTs(qint64 dateMicros);
QDate   tsToDate(qint64 dateMicros);
QString formatDateLabel(qint64 dateMicros);
// Slack-style absolute label for the reply bar: "today at 1:12 PM",
// "yesterday at 9:03 AM" or "March 3 at 4:15 PM".
QString lastReplyLabel(const Ts &ts);
QString resolveMention(const QString &userId, const Session *session);
QString toHtml(const TextWithEntities &twe, const Session *session = nullptr);

// Plain-text rendering for OS notifications / previews: resolves user and
// channel mentions to their display names and built-in emoji codes to their
// Unicode glyph (custom emoji, which can't render in a text-only notification,
// stay as ":name:"). No HTML, no markup — just readable text.
QString notificationText(const TextWithEntities &twe, const Session *session);

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
QVector<QRectF> botButtonRects(const QTextDocument *doc);
void            paintBotButtonChrome(QPainter &p, const QTextDocument *doc);

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
bool    attachIsTableOnly(const Attachment &att);
QColor  fileTypeColor(const File &f);
QString fileIconLabel(const File &f);
QString formatFileSize(qint64 bytes);

// Paint a single non-image file chip into rect using the canonical message-list style.
// rect should be kFileChipH (52px) tall; width is clamped to kFileChipMaxW (380px).
void paintFileChip(QPainter &p, const File &f, const QRect &rect);

// Canonical chip dimensions — exposed so callers can size widgets correctly.
inline constexpr int kFileChipH    = 52;
inline constexpr int kFileChipMaxW = 380;

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

// Marker cell spacing identifying bot-button tables in a QTextDocument (code
// blocks and blockquotes use 0); read back by botButtonRects().
inline constexpr int kBotBtnCellSpacing = 4;

// Inline image-block size cap — matches the message list's kImgMaxW/kImgMaxH.
inline constexpr int kBlockImgMaxW = 400;
inline constexpr int kBlockImgMaxH = 300;

} // namespace MsgRender
