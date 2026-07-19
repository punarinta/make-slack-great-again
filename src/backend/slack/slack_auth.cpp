// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "slack_auth.h"

#include "app_credentials.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace slack {

namespace {
constexpr auto kClientIdKey     = "credentials/slackClientId";
constexpr auto kClientSecretKey = "credentials/slackClientSecret";
constexpr auto kXappKey         = "credentials/slackXapp";
} // namespace

PersonalAppCredentials personalAppCredentials() {
    QSettings s("msga", "msga");
    return {
        s.value(QString::fromLatin1(kClientIdKey)).toString(),
        s.value(QString::fromLatin1(kClientSecretKey)).toString(),
        s.value(QString::fromLatin1(kXappKey)).toString(),
    };
}

void setPersonalAppCredentials(const PersonalAppCredentials &creds) {
    QSettings  s("msga", "msga");
    // Store trimmed values; blank a field to fall back to the compiled-in build
    // credential. Blank fields are removed rather than stored empty.
    const auto put = [&s](const char *key, const QString &val) {
        const QString v = val.trimmed();
        if (v.isEmpty())
            s.remove(QString::fromLatin1(key));
        else
            s.setValue(QString::fromLatin1(key), v);
    };
    put(kClientIdKey, creds.clientId);
    put(kClientSecretKey, creds.clientSecret);
    put(kXappKey, creds.xapp);
}

AppConfig appConfig() {
    AppConfig cfg{
        QString::fromLatin1(AppCredentials::clientId),
        QString::fromLatin1(AppCredentials::clientSecret),
        QString::fromLatin1(AppCredentials::xapp),
    };
    // Personal credentials override the build defaults, field by field.
    const PersonalAppCredentials personal = personalAppCredentials();
    if (!personal.clientId.isEmpty())
        cfg.clientId = personal.clientId;
    if (!personal.clientSecret.isEmpty())
        cfg.clientSecret = personal.clientSecret;
    if (!personal.xapp.isEmpty())
        cfg.xapp = personal.xapp;
    return cfg;
}

TokenStore::WorkspaceRecord toRecord(const Credentials &creds) {
    QJsonObject blob;
    blob[QStringLiteral("xoxp")]         = creds.xoxp;
    blob[QStringLiteral("refreshToken")] = creds.refreshToken;
    // expiresAt as a string — JSON numbers are doubles, and we want an exact
    // qint64 round-trip.
    blob[QStringLiteral("expiresAt")]    = QString::number(creds.expiresAt);

    TokenStore::WorkspaceRecord rec;
    rec.key         = WorkspaceKey{Service::Slack, creds.teamId};
    rec.displayName = creds.teamName;
    rec.iconUrl     = creds.iconUrl;
    rec.auth        = QJsonDocument(blob).toJson(QJsonDocument::Compact);
    return rec;
}

Credentials fromRecord(const TokenStore::WorkspaceRecord &rec) {
    const auto  blob = QJsonDocument::fromJson(rec.auth).object();
    Credentials c;
    c.xoxp         = blob.value(QStringLiteral("xoxp")).toString();
    c.teamId       = rec.key.id;
    c.teamName     = rec.displayName;
    c.iconUrl      = rec.iconUrl;
    c.refreshToken = blob.value(QStringLiteral("refreshToken")).toString();
    c.expiresAt    = blob.value(QStringLiteral("expiresAt")).toString().toLongLong();
    return c;
}

} // namespace slack
