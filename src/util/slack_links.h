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
//
// CAVEAT — channels only for a *constructed* link: the /huddle/ landing page can
// START a huddle for a channel id (C…/G… channel), but for a DM (D…) or group DM
// it can only JOIN an already-live one. Handing it a DM id with no live huddle
// yields Slack's "Server Error" page. So construct this only for channels; for a
// DM, either use the room's authoritative huddle_link (a live huddle) or fall
// back to conversation() (open the DM so the user starts the huddle there).
inline QString huddle(const QString &teamId, const QString &channelId) {
    return QStringLiteral("https://app.slack.com/huddle/%1/%2").arg(teamId, channelId);
}

// Open a conversation in the Slack client, e.g.
// https://app.slack.com/client/TCF2J0TSP/DCF69AC02 — the plain "go to this
// channel/DM" URL. Used as the huddle fallback for DMs, where a constructed
// /huddle/ link can't start a huddle (see huddle() above).
inline QString conversation(const QString &teamId, const QString &channelId) {
    return QStringLiteral("https://app.slack.com/client/%1/%2").arg(teamId, channelId);
}

} // namespace SlackLinks
