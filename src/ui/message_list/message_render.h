// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include <QString>
#include <QColor>
#include <QDate>

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

} // namespace MsgRender
