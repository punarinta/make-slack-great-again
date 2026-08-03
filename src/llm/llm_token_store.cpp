// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_token_store.h"

#include "util/secret_store.h"

#include <QSettings>

namespace LlmTokenStore {

static QString key(const QString &providerId, const char *field) {
    return QStringLiteral("llm/%1/%2").arg(providerId, QLatin1String(field));
}

Credentials load(const QString &providerId) {
    QSettings   s("msga", "msga");
    Credentials c;
    // API key + OAuth tokens are secret → keychain; the rest is metadata.
    c.apiKey       = SecretStore::readMigrating(key(providerId, "apiKey"));
    c.accessToken  = SecretStore::readMigrating(key(providerId, "accessToken"));
    c.refreshToken = SecretStore::readMigrating(key(providerId, "refreshToken"));
    c.expiresAt    = s.value(key(providerId, "expiresAt"), 0).toLongLong();
    c.accountLabel = s.value(key(providerId, "accountLabel")).toString();
    return c;
}

void save(const QString &providerId, const Credentials &c) {
    QSettings s("msga", "msga");
    // API key + OAuth tokens are secret → keychain (empty clears).
    SecretStore::writeScrubbingLegacy(key(providerId, "apiKey"), c.apiKey);
    SecretStore::writeScrubbingLegacy(key(providerId, "accessToken"), c.accessToken);
    SecretStore::writeScrubbingLegacy(key(providerId, "refreshToken"), c.refreshToken);
    s.setValue(key(providerId, "expiresAt"), c.expiresAt);
    s.setValue(key(providerId, "accountLabel"), c.accountLabel);
}

void clear(const QString &providerId) {
    SecretStore::remove(key(providerId, "apiKey"));
    SecretStore::remove(key(providerId, "accessToken"));
    SecretStore::remove(key(providerId, "refreshToken"));
    QSettings("msga", "msga").remove(QStringLiteral("llm/%1").arg(providerId));
}

} // namespace LlmTokenStore
