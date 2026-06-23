// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "auth/token_store.h"
#include "backend/domain.h"

#include <QString>

namespace teams {

// Per-workspace Microsoft Teams (Graph) credentials — the token-shaped struct,
// kept in the Teams adapter below the Backend seam. A Teams "workspace" is one
// signed-in account in one Entra tenant: the WorkspaceKey id is the tenant id,
// displayName is the organization name. The MSAL-style token set is serialized
// into the opaque TokenStore::WorkspaceRecord::auth blob; tenant maps to the
// WorkspaceKey id, orgName/iconUrl to the record's neutral displayName/iconUrl.
struct Credentials {
    QString accessToken;
    QString tenantId;      // == WorkspaceKey::id (the Entra tenant / org)
    QString orgName;       // == WorkspaceRecord::displayName
    QString iconUrl;       // == WorkspaceRecord::iconUrl, may be empty
    QString refreshToken;  // requires the offline_access scope
    qint64  expiresAt = 0; // Unix ts when the access token expires; 0 = unknown
    QString userId;        // the signed-in user's AAD object id (oid)
};

// Microsoft Teams app registration (compiled in via credentials.cmake). A
// multi-tenant public client — no secret (Auth Code + PKCE).
struct AppConfig {
    QString clientId;
};

// Reads the compiled-in Teams app credentials.
AppConfig appConfig();

// Encode/decode the Teams credentials to/from the neutral registry record.
// Auth-blob JSON shape: {accessToken, refreshToken, expiresAt, userId}.
TokenStore::WorkspaceRecord toRecord(const Credentials &creds);
Credentials                 fromRecord(const TokenStore::WorkspaceRecord &rec);

} // namespace teams
