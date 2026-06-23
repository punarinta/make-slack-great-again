// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "teams_auth.h"

#include "app_credentials.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace teams {

AppConfig appConfig() {
    return {
        QString::fromLatin1(AppCredentials::teamsClientId),
    };
}

TokenStore::WorkspaceRecord toRecord(const Credentials &creds) {
    QJsonObject blob;
    blob[QStringLiteral("accessToken")]  = creds.accessToken;
    blob[QStringLiteral("refreshToken")] = creds.refreshToken;
    // expiresAt as a string — JSON numbers are doubles, and we want an exact
    // qint64 round-trip.
    blob[QStringLiteral("expiresAt")]    = QString::number(creds.expiresAt);
    blob[QStringLiteral("userId")]       = creds.userId;

    TokenStore::WorkspaceRecord rec;
    rec.key         = WorkspaceKey{Service::Teams, creds.tenantId};
    rec.displayName = creds.orgName;
    rec.iconUrl     = creds.iconUrl;
    rec.auth        = QJsonDocument(blob).toJson(QJsonDocument::Compact);
    return rec;
}

Credentials fromRecord(const TokenStore::WorkspaceRecord &rec) {
    const auto  blob = QJsonDocument::fromJson(rec.auth).object();
    Credentials c;
    c.accessToken  = blob.value(QStringLiteral("accessToken")).toString();
    c.tenantId     = rec.key.id;
    c.orgName      = rec.displayName;
    c.iconUrl      = rec.iconUrl;
    c.refreshToken = blob.value(QStringLiteral("refreshToken")).toString();
    c.expiresAt    = blob.value(QStringLiteral("expiresAt")).toString().toLongLong();
    c.userId       = blob.value(QStringLiteral("userId")).toString();
    return c;
}

} // namespace teams
