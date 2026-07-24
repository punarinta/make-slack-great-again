// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "auth/token_store.h"
#include "backend/domain.h"

#include <QString>

namespace slack {

// Per-workspace Slack credentials — the token-shaped struct, demoted out of the
// neutral TokenStore into the Slack adapter. The token fields are serialized
// into the opaque TokenStore::WorkspaceRecord::auth blob; teamName/iconUrl map
// to the record's neutral displayName/iconUrl, teamId to the WorkspaceKey id.
struct Credentials {
    QString xoxp;          // bearer token: an OAuth xoxp- OR a session xoxc- token
    QString teamId;        // == WorkspaceKey::id
    QString teamName;      // == WorkspaceRecord::displayName
    QString iconUrl;       // == WorkspaceRecord::iconUrl, may be empty
    QString refreshToken;  // non-empty when token rotation is enabled (OAuth only)
    qint64  expiresAt = 0; // Unix ts when the access token expires; 0 = unknown
    // Session auth: the `d` session cookie (value carries its own xoxd- prefix).
    // Non-empty ⇒ this workspace is session-authed (xoxc token + cookie) rather
    // than OAuth; it has no Socket Mode realtime and its token does not rotate.
    QString cookie;
    // Session auth: the workspace URL (e.g. "https://team.slack.com/"). Stored so
    // the xoxc token can be re-derived from a fresh cookie without a live token —
    // needed because the account-wide `d` cookie rotates on logout/re-login, which
    // stales every session workspace's token+cookie at once.
    QString workspaceUrl;

    [[nodiscard]] bool isSessionAuth() const { return !cookie.isEmpty(); }
};

// How the app connects to Slack, chosen globally by the user in Settings → System.
//  - AppKeys: OAuth sign-in (xoxp) + app-level Socket Mode realtime (xapp). Default.
//  - Session: the user's own browser session (xoxc + `d` cookie). NO app keys and
//    NO Socket Mode at all — realtime is replaced by polling. Picking Session must
//    make Socket Mode completely inert (no shared-app-key contention).
enum class ConnectionMode { AppKeys, Session };
ConnectionMode connectionMode();
void           setConnectionMode(ConnectionMode mode);

// Slack app registration (compiled in via credentials.cmake). Moved out of
// TokenStore — app credentials are a Slack-internal concern.
struct AppConfig {
    QString clientId;
    QString clientSecret;
    QString xapp; // app-level token for Socket Mode (connections:write)
};

// Reads the effective Slack app credentials: the personal ones the user saved in
// Settings → System (persisted in QSettings) take precedence, field by field,
// over the values compiled in via credentials.cmake. Lets prebuilt-app users run
// their own Slack app instead of sharing the build's app keys with everyone else
// (the parallel-usage / EvRealtimeContended situation).
AppConfig appConfig();

// Personal Slack app credentials saved in Settings → System. Empty fields fall
// through to the compiled-in build credentials in appConfig().
struct PersonalAppCredentials {
    QString clientId;
    QString clientSecret;
    QString xapp;
};
PersonalAppCredentials personalAppCredentials();
void                   setPersonalAppCredentials(const PersonalAppCredentials &creds);

// Encode/decode the Slack credentials to/from the neutral registry record.
// The auth-blob JSON shape ({xoxp, refreshToken, expiresAt}) is mirrored by the
// one-time migration in token_store.cpp.
TokenStore::WorkspaceRecord toRecord(const Credentials &creds);
Credentials                 fromRecord(const TokenStore::WorkspaceRecord &rec);

} // namespace slack
