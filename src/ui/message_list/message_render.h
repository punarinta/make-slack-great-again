// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include <QString>
#include <QColor>
#include <QDate>

class QPainter;
class QRect;
class Session;

// Pure rendering helpers shared between message_list.cpp and message_list_paint.cpp.
// No widget state — takes only domain types and an optional Session* for name lookups.
namespace MsgRender {

QString resolveEmoji(const QString &name);
QString formatTs(const Ts &ts);
QDate   tsToDate(const Ts &ts);
QString formatDateLabel(const Ts &ts);
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
