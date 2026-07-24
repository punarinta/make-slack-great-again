// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "slack_auth.h"

#include "app_credentials.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace slack {

namespace {
constexpr auto kClientIdKey       = "credentials/slackClientId";
constexpr auto kClientSecretKey   = "credentials/slackClientSecret";
constexpr auto kXappKey           = "credentials/slackXapp";
constexpr auto kConnectionModeKey = "slack/connectionMode";
} // namespace

ConnectionMode connectionMode() {
    QSettings     s("msga", "msga");
    const QString v = s.value(QString::fromLatin1(kConnectionModeKey)).toString();
    if (v == QLatin1String("session"))
        return ConnectionMode::Session;
    if (v == QLatin1String("appkeys"))
        return ConnectionMode::AppKeys;
    // Unset (never chosen): default to Session — the recommended, no-app-keys path.
    // Exception: an existing OAuth Slack workspace (signed in before the mode
    // existed) implies app-keys mode; keep it so we don't silently kill live push.
    for (const auto &key : TokenStore::workspaceKeys()) {
        if (key.service != Service::Slack)
            continue;
        const auto rec = TokenStore::loadWorkspace(key);
        if (rec && fromRecord(*rec).cookie.isEmpty())
            return ConnectionMode::AppKeys;
    }
    return ConnectionMode::Session;
}

void setConnectionMode(ConnectionMode mode) {
    QSettings s("msga", "msga");
    s.setValue(
        QString::fromLatin1(kConnectionModeKey),
        mode == ConnectionMode::Session ? QStringLiteral("session") : QStringLiteral("appkeys")
    );
}

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
    // Log the effective app identity ONCE so a client_id/xapp mismatch is visible.
    // The socket uses cfg.xapp's app; sign-in uses cfg.clientId's app — if a user
    // sets their own xapp but signs in (or stays signed in) under a different app,
    // that app's Socket Mode connection has nothing to deliver and Slack closes it
    // after ~10 s. clientId is `<team>.<apppart>`; xapp is `xapp-1-<APPID>-…`.
    static bool logged = false;
    if (!logged) {
        logged            = true;
        const auto    src = [](const QString &p) { return p.isEmpty() ? "compiled" : "settings"; };
        const QString xappApp = cfg.xapp.section('-', 2, 2); // A0…
        qInfo().noquote() << "Slack app identity — clientId" << cfg.clientId.left(20) << "("
                          << src(personal.clientId) << "), secret (" << src(personal.clientSecret)
                          << "), xapp app" << xappApp << "(" << src(personal.xapp) << ")";
    }
    return cfg;
}

TokenStore::WorkspaceRecord toRecord(const Credentials &creds) {
    QJsonObject blob;
    blob[QStringLiteral("xoxp")]         = creds.xoxp;
    blob[QStringLiteral("refreshToken")] = creds.refreshToken;
    // expiresAt as a string — JSON numbers are doubles, and we want an exact
    // qint64 round-trip.
    blob[QStringLiteral("expiresAt")]    = QString::number(creds.expiresAt);
    // Session `d` cookie — omitted for OAuth workspaces so their blob is unchanged.
    if (!creds.cookie.isEmpty())
        blob[QStringLiteral("cookie")] = creds.cookie;
    if (!creds.workspaceUrl.isEmpty())
        blob[QStringLiteral("workspaceUrl")] = creds.workspaceUrl;

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
    c.cookie       = blob.value(QStringLiteral("cookie")).toString();
    c.workspaceUrl = blob.value(QStringLiteral("workspaceUrl")).toString();
    return c;
}

} // namespace slack
