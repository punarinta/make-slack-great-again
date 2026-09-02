// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QString>

// A conversation paired with the name the conversation list paints for it —
// ConvListWidget::namedConversations() is the only producer.
//
// It exists so pickers don't each reimplement the DM-peer / group-member /
// channel name resolution (there are already three copies of that logic in the
// tree). Independent of the list's visual rows: neither a collapsed section nor
// the relevance filter drops an entry.
struct NamedConversation {
    ConversationId id;
    QString        name;
    ConvKind       kind;
    QString        avatarUrl; // 1:1 DMs only
    // Most recent of "you were last in here" and the conversation's own
    // activity, in epoch seconds — the quick switcher's default ordering.
    qint64         activitySeconds = 0;
};
