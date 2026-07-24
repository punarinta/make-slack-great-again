// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Normalizes a raw Slack event object (the inner `event` of a Socket Mode
// events_api envelope, OR a bare RTM frame — both are the same event shape) into
// a domain Event. Shared by SocketModeRealtime (xapp/app-level) and
// SessionRealtime (xoxc/user-level RTM) so both transports map events identically.
#pragma once

#include "backend/domain.h"

#include <QJsonObject>
#include <optional>

namespace slack {

// Map a Slack event object to a domain Event, or nullopt if we don't handle it.
std::optional<Event> normalizeSlackEvent(const QJsonObject &ev);

// Extra, ADDITIVE huddle-state event for a huddle_thread message (start) or its
// edit (end); does not replace the message's normal event. nullopt otherwise.
std::optional<Event> huddleEventFor(const QJsonObject &ev);

} // namespace slack
