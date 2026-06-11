// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Persists per-provider LLM credentials in QSettings under llm/<providerId>/.
// Mirrors auth/token_store.h, which does the same for Slack workspaces.
#pragma once

#include <QString>

namespace LlmTokenStore {

struct Credentials {
    QString apiKey;      // non-empty → API-key auth
    QString accessToken; // non-empty → OAuth auth
    QString refreshToken;
    qint64  expiresAt = 0; // Unix timestamp when accessToken expires; 0 = unknown
    QString accountLabel;  // email or masked key, for display only

    [[nodiscard]] bool isConnected() const { return !apiKey.isEmpty() || !accessToken.isEmpty(); }
};

Credentials load(const QString &providerId);
void        save(const QString &providerId, const Credentials &c);
void        clear(const QString &providerId);

} // namespace LlmTokenStore
