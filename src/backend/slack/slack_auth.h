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
    QString xoxp;
    QString teamId;        // == WorkspaceKey::id
    QString teamName;      // == WorkspaceRecord::displayName
    QString iconUrl;       // == WorkspaceRecord::iconUrl, may be empty
    QString refreshToken;  // non-empty when token rotation is enabled
    qint64  expiresAt = 0; // Unix ts when the access token expires; 0 = unknown
};

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
