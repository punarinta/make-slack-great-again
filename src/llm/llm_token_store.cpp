// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_token_store.h"

#include "util/secret_store.h"

#include <QSettings>

namespace LlmTokenStore {

static QString key(const QString &providerId, const char *field) {
    return QStringLiteral("llm/%1/%2").arg(providerId, QLatin1String(field));
}

QString loadApiKey(const QString &providerId) {
    return SecretStore::readMigrating(key(providerId, "apiKey"));
}

void saveApiKey(const QString &providerId, const QString &apiKey) {
    SecretStore::writeScrubbingLegacy(key(providerId, "apiKey"), apiKey);
}

void clear(const QString &providerId) {
    SecretStore::remove(key(providerId, "apiKey"));
    QSettings("msga", "msga").remove(QStringLiteral("llm/%1").arg(providerId));
}

void scrubLegacyOAuth(const QString &providerId) {
    SecretStore::remove(key(providerId, "accessToken"));
    SecretStore::remove(key(providerId, "refreshToken"));
    QSettings s("msga", "msga");
    s.remove(key(providerId, "expiresAt"));
    s.remove(key(providerId, "accountLabel")); // now derived from the key
}

} // namespace LlmTokenStore
