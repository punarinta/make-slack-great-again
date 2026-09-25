// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <vector>

class Session;

// A conversation paired with the name the conversation list paints for it.
// Two producers: ConvListWidget::namedConversations() for the workspace on
// screen (it holds the freshest visit stamps and the exact filter the sidebar
// applies), and namedConversationsFor() below for a workspace running in the
// background, which has a Session but no list widget.
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

// The quick switcher's default ordering: most recent activity first, then by
// name. Shared by both producers (one std::sort instantiation, not two).
struct NamedConversationOrder {
    bool operator()(const NamedConversation &a, const NamedConversation &b) const {
        if (a.activitySeconds != b.activitySeconds)
            return a.activitySeconds > b.activitySeconds; // most recent first
        return a.name.localeAwareCompare(b.name) < 0;
    }
};

// Slack names an unnamed group DM "mpdm-alice--bob--carol-1": the member
// usernames joined by "--", with a numeric suffix. Returns those usernames.
QStringList parseMpdmUsernames(const QString &mpdmName);

// Name-resolved, most-recent-first list of the session's joined conversations,
// applying the same membership / deactivated-peer rules as the sidebar.
// `visitedAt` is the sidebar's conv-id → epoch-seconds "last opened here"
// store (ConvListWidget::visitedAt(); it is app-wide, keyed by conversation
// id, so one map serves every workspace). Cheap enough for a keypress: users
// are scanned once, and only the members of the listed DMs are resolved.
std::vector<NamedConversation>
namedConversationsFor(Session *session, const QHash<QString, qint64> &visitedAt);
