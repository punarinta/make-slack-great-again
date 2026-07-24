// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Shared value types for Slack session-token (xoxc/xoxd) sign-in. Widgets-free so
// the importer/deriver stay out of the UI layer (mirrors the imap auth split).
#pragma once

#include <QList>
#include <QString>

namespace slack::session {

// One workspace discovered from a Slack session. `token` (xoxc-) may be empty when
// only the cookie is known (manual paste) — the deriver fills it from the boot
// payload. `workspaceUrl` is the team's slack.com host, used to derive a missing
// token. teamId/teamName/iconUrl are best-effort until auth.test/team.info fill them.
struct TeamSession {
    QString token;        // xoxc- bearer, or empty to derive
    QString workspaceUrl; // e.g. "https://myteam.slack.com", used to derive token
    QString teamId;
    QString teamName;
    QString iconUrl;
};

// Result of reading the locally-installed Slack desktop app. `cookie` is the
// decrypted `d` session cookie (carries its xoxd- prefix). `teams` lists the
// signed-in workspaces (with tokens when leveldb yielded them). On failure `cookie`
// is empty and `error` holds a short machine reason (e.g. "not_installed",
// "locked", "decrypt_failed", "unsupported_platform") that the UI turns into the
// "auto-import failed → guided manual paste" transition.
struct LocalImport {
    QString            cookie;
    QList<TeamSession> teams;
    QString            error;

    [[nodiscard]] bool ok() const { return !cookie.isEmpty() && error.isEmpty(); }
};

} // namespace slack::session
