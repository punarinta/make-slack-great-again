// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QString>

// Single source of truth for Slack deep links. Keep every link Slack-shaped:
// these must match the URLs Slack's own clients (and API responses) use, so a
// link we hand to the OS / browser / a notification opens the native client.
namespace SlackLinks {

// Open (and join/start) a huddle in a conversation. This is the exact shape
// Slack returns in a room's `huddle_link` field, e.g.
// https://app.slack.com/huddle/TCF2J0TSP/CCEEHAA1E — NOT the
// /client/<team>/<channel>?open=start_huddle form, which does not start a
// huddle.
inline QString huddle(const QString &teamId, const QString &channelId) {
    return QStringLiteral("https://app.slack.com/huddle/%1/%2").arg(teamId, channelId);
}

} // namespace SlackLinks
