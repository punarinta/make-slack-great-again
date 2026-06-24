// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_auth.h"

#include "app_credentials.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace imap {

std::optional<OAuthConfig> oauthConfigFor(AuthMethod method, const QString &email) {
    if (method == AuthMethod::OAuthGoogle) {
        const QString clientId = QString::fromUtf8(AppCredentials::googleClientId);
        if (clientId.isEmpty())
            return std::nullopt; // not configured → caller falls back to password
        OAuthConfig cfg;
        cfg.authorizeUrl    = QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth");
        cfg.tokenUrl        = QStringLiteral("https://oauth2.googleapis.com/token");
        cfg.clientId        = clientId;
        cfg.clientSecret    = QString::fromUtf8(AppCredentials::googleClientSecret);
        cfg.scopes          = QStringLiteral("https://mail.google.com/");
        cfg.port            = 35253; // fixed loopback port (Google allows any for Desktop apps)
        cfg.callbackPath    = QStringLiteral("/callback");
        // offline → refresh token; consent → re-issue refresh token on re-auth;
        // login_hint pre-selects the account the user typed.
        cfg.extraAuthParams = {
            {QStringLiteral("access_type"), QStringLiteral("offline")},
            {QStringLiteral("prompt"), QStringLiteral("consent")},
        };
        if (!email.isEmpty())
            cfg.extraAuthParams.append({QStringLiteral("login_hint"), email});
        return cfg;
    }
    // OAuthMicrosoft is handled by the Teams app registration elsewhere; IMAP
    // XOAUTH2 for Outlook is not wired yet → fall back to password.
    return std::nullopt;
}

TokenStore::WorkspaceRecord toRecord(const Credentials &creds) {
    QJsonObject blob;
    blob[QStringLiteral("host")]         = creds.host;
    blob[QStringLiteral("port")]         = int(creds.port);
    blob[QStringLiteral("password")]     = creds.password;
    blob[QStringLiteral("smtpHost")]     = creds.smtpHost;
    blob[QStringLiteral("smtpPort")]     = int(creds.smtpPort);
    blob[QStringLiteral("insecure")]     = creds.insecure;
    blob[QStringLiteral("authMethod")]   = int(creds.authMethod);
    blob[QStringLiteral("accessToken")]  = creds.accessToken;
    blob[QStringLiteral("refreshToken")] = creds.refreshToken;
    blob[QStringLiteral("expiresAt")]    = double(creds.expiresAt);
    QJsonArray aliases;
    for (const QString &a : creds.aliases)
        aliases.append(a);
    blob[QStringLiteral("aliases")] = aliases;

    TokenStore::WorkspaceRecord rec;
    rec.key         = WorkspaceKey{Service::Imap, creds.user};
    rec.displayName = creds.user;
    rec.auth        = QJsonDocument(blob).toJson(QJsonDocument::Compact);
    return rec;
}

Credentials fromRecord(const TokenStore::WorkspaceRecord &rec) {
    const auto  blob = QJsonDocument::fromJson(rec.auth).object();
    Credentials c;
    c.user         = rec.key.id;
    c.host         = blob.value(QStringLiteral("host")).toString();
    c.port         = quint16(blob.value(QStringLiteral("port")).toInt(993));
    c.password     = blob.value(QStringLiteral("password")).toString();
    c.smtpHost     = blob.value(QStringLiteral("smtpHost")).toString();
    c.smtpPort     = quint16(blob.value(QStringLiteral("smtpPort")).toInt(587));
    c.insecure     = blob.value(QStringLiteral("insecure")).toBool();
    c.authMethod   = AuthMethod(blob.value(QStringLiteral("authMethod")).toInt(0));
    c.accessToken  = blob.value(QStringLiteral("accessToken")).toString();
    c.refreshToken = blob.value(QStringLiteral("refreshToken")).toString();
    c.expiresAt    = qint64(blob.value(QStringLiteral("expiresAt")).toDouble(0));
    for (const auto &a : blob.value(QStringLiteral("aliases")).toArray())
        c.aliases << a.toString();
    return c;
}

std::optional<Credentials> credentialsFromEnv() {
    const QString host = qEnvironmentVariable("IMAP_HOST");
    const QString user = qEnvironmentVariable("IMAP_USER");
    const QString pass = qEnvironmentVariable("IMAP_PASS");
    if (host.isEmpty() || user.isEmpty() || pass.isEmpty())
        return std::nullopt;

    Credentials c;
    c.host            = host;
    c.user            = user;
    c.password        = pass;
    bool       portOk = false;
    const uint p      = qEnvironmentVariable("IMAP_PORT").toUInt(&portOk);
    if (portOk && p > 0)
        c.port = quint16(p);
    c.insecure       = qEnvironmentVariableIntValue("IMAP_INSECURE") != 0;
    c.smtpHost       = qEnvironmentVariable("IMAP_SMTP_HOST"); // else derived at send time
    bool       sp    = false;
    const uint sport = qEnvironmentVariable("IMAP_SMTP_PORT").toUInt(&sp);
    if (sp && sport > 0)
        c.smtpPort = quint16(sport);
    const QString aliases = qEnvironmentVariable("IMAP_ALIASES");
    if (!aliases.isEmpty())
        c.aliases = aliases.split(QLatin1Char(','), Qt::SkipEmptyParts);
    return c;
}

bool seedDevWorkspaceFromEnv() {
    const auto c = credentialsFromEnv();
    if (!c)
        return false;
    TokenStore::saveWorkspace(toRecord(*c));
    return true;
}

} // namespace imap
