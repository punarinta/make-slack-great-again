// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QCoreApplication>
#include <initializer_list>
#include <vector>

// Canonical definitions of the slash commands the app can execute natively
// (Session::runCommand has the handlers). Shared so each backend — and the
// app-level set Session owns — picks the subset it supports by name rather than
// redefining names/help text. Backends overlap heavily (away/status/dnd/dm), and
// this is the single source of truth for that overlap. Header-only so every
// consumer (Session + each backend) compiles it without a shared link target.
namespace CommonCommands {

// All known native commands (freshly built so translations track locale).
inline std::vector<SlashCommand> all() {
    const auto t = [](const char *s) { return QCoreApplication::translate("SlashCommands", s); };
    return {
        // App-level (backend-agnostic): handled by Session without touching the
        // messaging API.
        {"shrug",
         t("Appends \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF to your message"),
         t("[message]"),
         {}},
        {"mute", t("Mute or unmute a channel"), {}, {}},
        // Self presence / status — map to setPresence/setStatus/setDndSnooze.
        {"active", t("Set yourself to active"), {}, {}},
        {"away", t("Toggle your away status"), {}, {}},
        {"dnd", t("Pause or resume notifications"), t("[duration, e.g. 30m or 2h] or off"), {}},
        {"status", t("Set or clear your status"), t("[:emoji:] [text] or clear"), {}},
        // Conversation actions — map to openDm / leaveConversation.
        {"msg", t("Send a direct message"), t("@user [message]"), {}},
        {"dm", t("Send a direct message"), t("@user [message]"), {}},
        {"leave", t("Leave a channel or conversation"), {}, {}},
    };
}

// The subset whose names are listed, in the given order. Unknown names skipped.
inline std::vector<SlashCommand> select(std::initializer_list<const char *> names) {
    const auto                everything = all();
    std::vector<SlashCommand> out;
    for (const char *n : names)
        for (const auto &c : everything)
            if (c.name == QLatin1String(n)) {
                out.push_back(c);
                break;
            }
    return out;
}

} // namespace CommonCommands
