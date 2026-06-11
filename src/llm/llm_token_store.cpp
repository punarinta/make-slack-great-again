// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_token_store.h"

#include <QSettings>

namespace LlmTokenStore {

static QString key(const QString &providerId, const char *field) {
    return QStringLiteral("llm/%1/%2").arg(providerId, QLatin1String(field));
}

Credentials load(const QString &providerId) {
    QSettings   s("msga", "msga");
    Credentials c;
    c.apiKey       = s.value(key(providerId, "apiKey")).toString();
    c.accessToken  = s.value(key(providerId, "accessToken")).toString();
    c.refreshToken = s.value(key(providerId, "refreshToken")).toString();
    c.expiresAt    = s.value(key(providerId, "expiresAt"), 0).toLongLong();
    c.accountLabel = s.value(key(providerId, "accountLabel")).toString();
    return c;
}

void save(const QString &providerId, const Credentials &c) {
    QSettings s("msga", "msga");
    s.setValue(key(providerId, "apiKey"), c.apiKey);
    s.setValue(key(providerId, "accessToken"), c.accessToken);
    s.setValue(key(providerId, "refreshToken"), c.refreshToken);
    s.setValue(key(providerId, "expiresAt"), c.expiresAt);
    s.setValue(key(providerId, "accountLabel"), c.accountLabel);
}

void clear(const QString &providerId) {
    QSettings("msga", "msga").remove(QStringLiteral("llm/%1").arg(providerId));
}

} // namespace LlmTokenStore
