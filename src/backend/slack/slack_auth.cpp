// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "slack_auth.h"

#include "app_credentials.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace slack {

AppConfig appConfig() {
    return {
        QString::fromLatin1(AppCredentials::clientId),
        QString::fromLatin1(AppCredentials::clientSecret),
        QString::fromLatin1(AppCredentials::xapp),
    };
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
