// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Persists per-provider LLM API keys in the secret store under
// llm/<providerId>/apiKey. Mirrors auth/token_store.h, which does the same for
// Slack workspaces. Provider *configuration* (name, URL, model) is plain
// metadata and lives with LlmService in QSettings.
#pragma once

#include <QString>

namespace LlmTokenStore {

QString loadApiKey(const QString &providerId);
void    saveApiKey(const QString &providerId, const QString &key); // empty clears
void    clear(const QString &providerId);

// Removes the OAuth token fields an earlier version stored next to the key
// (accessToken/refreshToken/expiresAt). LLM OAuth was never offered by either
// vendor; run once at startup so stale keychain entries don't linger.
void scrubLegacyOAuth(const QString &providerId);

} // namespace LlmTokenStore
