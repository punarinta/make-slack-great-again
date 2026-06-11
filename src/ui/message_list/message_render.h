// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include <QString>
#include <QStringList>
#include <QColor>
#include <QDate>
#include <QHash>

class QPainter;
class QRect;
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

// All custom-emoji image URLs referenced by a message (text, blocks, attachments),
// deduplicated. Used to register QTextDocument image resources.
QStringList collectEmojiImageUrls(const Message &msg, const Session *session);

QString formatTs(const Ts &ts);
QDate   tsToDate(const Ts &ts);
QString formatDateLabel(const Ts &ts);
// Slack-style absolute label for the reply bar: "today at 1:12 PM",
// "yesterday at 9:03 AM" or "March 3 at 4:15 PM".
QString lastReplyLabel(const Ts &ts);
QString resolveMention(const QString &userId, const Session *session);
QString toHtml(const TextWithEntities &twe, const Session *session = nullptr);
QString buildMsgHtml(const Message &msg, const Session *session);
QString buildAttachHtml(const Attachment &att, const Session *session);
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

} // namespace MsgRender
