// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "token_store.h"
#include "app_credentials.h"
#include <QSettings>

static QSettings settings() {
    return QSettings("msga", "msga");
}

static void migrate(QSettings &s) {
    if (s.contains("workspaces") || !s.contains("auth/xoxp"))
        return;
    const QString xoxp   = s.value("auth/xoxp").toString();
    const QString teamId = s.value("auth/team_id").toString();
    const QString name   = s.value("auth/team_name").toString();
    if (xoxp.isEmpty())
        return;
    const QString id = teamId.isEmpty() ? QStringLiteral("legacy") : teamId;
    s.setValue(QStringLiteral("workspace/%1/xoxp").arg(id), xoxp);
    s.setValue(QStringLiteral("workspace/%1/name").arg(id), name);
    s.setValue(QStringLiteral("workspace/%1/iconUrl").arg(id), QString());
    s.setValue(QStringLiteral("workspaces"), QStringList{id});
    s.setValue(QStringLiteral("active"), id);
    s.remove(QStringLiteral("auth"));
}

void TokenStore::saveWorkspace(const Credentials &c) {
    auto s = settings();
    migrate(s);
    auto ids = s.value(QStringLiteral("workspaces")).toStringList();
    if (!ids.contains(c.teamId))
        ids.append(c.teamId);
    s.setValue(QStringLiteral("workspaces"), ids);
    s.setValue(QStringLiteral("workspace/%1/xoxp").arg(c.teamId), c.xoxp);
    s.setValue(QStringLiteral("workspace/%1/name").arg(c.teamId), c.teamName);
    s.setValue(QStringLiteral("workspace/%1/iconUrl").arg(c.teamId), c.iconUrl);
    s.setValue(QStringLiteral("workspace/%1/refreshToken").arg(c.teamId), c.refreshToken);
    s.setValue(QStringLiteral("workspace/%1/expiresAt").arg(c.teamId), c.expiresAt);
}

TokenStore::Credentials TokenStore::loadWorkspace(const QString &teamId) {
    auto s = settings();
    return {
        s.value(QStringLiteral("workspace/%1/xoxp").arg(teamId)).toString(),
        teamId,
        s.value(QStringLiteral("workspace/%1/name").arg(teamId)).toString(),
        s.value(QStringLiteral("workspace/%1/iconUrl").arg(teamId)).toString(),
        s.value(QStringLiteral("workspace/%1/refreshToken").arg(teamId)).toString(),
        s.value(QStringLiteral("workspace/%1/expiresAt").arg(teamId)).toLongLong(),
    };
}

void TokenStore::removeWorkspace(const QString &teamId) {
    auto s = settings();
    migrate(s);
    auto ids = s.value(QStringLiteral("workspaces")).toStringList();
    ids.removeAll(teamId);
    s.setValue(QStringLiteral("workspaces"), ids);
    s.remove(QStringLiteral("workspace/%1").arg(teamId));
    if (s.value(QStringLiteral("active")).toString() == teamId)
        s.setValue(QStringLiteral("active"), ids.isEmpty() ? QString() : ids.first());
}

QStringList TokenStore::workspaceIds() {
    auto s = settings();
    migrate(s);
    return s.value(QStringLiteral("workspaces")).toStringList();
}

bool TokenStore::hasAnyWorkspace() {
    return !workspaceIds().isEmpty();
}

QString TokenStore::activeWorkspaceId() {
    auto s = settings();
    migrate(s);
    return s.value(QStringLiteral("active")).toString();
}

void TokenStore::setActiveWorkspace(const QString &teamId) {
    settings().setValue(QStringLiteral("active"), teamId);
}

bool TokenStore::hasToken() {
    return hasAnyWorkspace();
}

TokenStore::Credentials TokenStore::load() {
    return loadWorkspace(activeWorkspaceId());
}

void TokenStore::save(const Credentials &c) {
    saveWorkspace(c);
    setActiveWorkspace(c.teamId);
}

void TokenStore::clear() {
    const auto id = activeWorkspaceId();
    if (!id.isEmpty())
        removeWorkspace(id);
}

TokenStore::AppConfig TokenStore::loadApp() {
    return {
        QString::fromLatin1(AppCredentials::clientId),
        QString::fromLatin1(AppCredentials::clientSecret),
        QString::fromLatin1(AppCredentials::xapp),
    };
}
