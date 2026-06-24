// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// IMAP per-workspace credentials (the token-shaped struct, below the seam) +
// neutral-record encode/decode, mirroring slack_auth / teams_auth. A "workspace"
// is one email account: WorkspaceKey::id is the email address. Phase 1 uses
// password auth and a dev env-var bridge; the auth UI + XOAUTH2 land in Phase 5.
#pragma once

#include "auth/token_store.h"
#include "backend/domain.h"
#include "backend/imap/imap_providers.h" // AuthMethod
#include "llm/oauth_loopback.h"          // OAuthConfig

#include <QString>
#include <QStringList>

#include <optional>

namespace imap {

struct Credentials {
    QString     host;       // IMAP host, e.g. "imap.fastmail.com"
    quint16     port = 993; // implicit-TLS port
    QString     user;       // == WorkspaceKey::id (the account email)
    QString     password;   // app-password / account password
    QStringList aliases;    // my *other* addresses, for "is this me?" checks
    QString     smtpHost;   // SMTP submission host (Phase 4); empty = derive
    quint16     smtpPort = 587;
    bool        insecure = false; // ignore TLS cert errors (dev/self-signed only)

    // OAuth (XOAUTH2) — used instead of `password` when authMethod is an OAuth
    // variant. Empty/Password ⇒ classic password login.
    AuthMethod authMethod = AuthMethod::Password;
    QString    accessToken;
    QString    refreshToken;
    qint64     expiresAt = 0; // Unix seconds; 0 = unknown
};

// Build the OAuth loopback config for a provider (Google/Microsoft) prefilled
// with the user's address. Returns nullopt when no client id is compiled in
// (credentials.cmake) — the add-account flow then falls back to password login.
std::optional<OAuthConfig> oauthConfigFor(AuthMethod method, const QString &email);

// Encode/decode the credentials to/from the neutral registry record. Auth-blob
// JSON: {host, port, password, aliases, smtpHost, smtpPort, insecure}. The user
// (email) is the WorkspaceKey id; displayName mirrors it.
TokenStore::WorkspaceRecord toRecord(const Credentials &creds);
Credentials                 fromRecord(const TokenStore::WorkspaceRecord &rec);

// Dev bridge (Phase 1, before the auth UI): build credentials from environment
// variables IMAP_HOST / IMAP_USER / IMAP_PASS [/ IMAP_PORT / IMAP_INSECURE /
// IMAP_ALIASES (comma-separated)]. Returns nullopt if the required ones are unset.
std::optional<Credentials> credentialsFromEnv();

// Dev bridge (Phase 1): if those env vars are set, persist an IMAP workspace to
// the TokenStore so the account loads through the normal workspace path and
// renders in the UI. No-op when unset. Temporary scaffolding — removed when the
// Phase-5 auth UI lands. Returns true if a workspace was seeded.
bool seedDevWorkspaceFromEnv();

} // namespace imap
