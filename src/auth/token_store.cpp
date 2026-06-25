// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "token_store.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

static QSettings settings() {
    return QSettings("msga", "msga");
}

namespace {

// Bumped whenever the on-disk layout changes; migrate() runs once per bump.
//  v1: single-workspace `auth/*` → multi-workspace `workspace/{bareId}/{xoxp,…}`.
//  v2: bare-id slack subtree → composite `workspace/{handle}/{displayName,iconUrl,auth}`.
constexpr int kStoreVersion = 2;

QString recordBase(const WorkspaceKey &key) {
    return QStringLiteral("workspace/") + key.toString();
}

// One-time legacy bridge from the pre-multi-workspace single-account layout
// (`auth/*`) to the v1 bare-id `workspace/{id}/*` shape. Slack-only era.
void migrateV0toV1(QSettings &s) {
    if (s.contains(QStringLiteral("workspaces")) || !s.contains(QStringLiteral("auth/xoxp")))
        return;
    const QString xoxp   = s.value(QStringLiteral("auth/xoxp")).toString();
    const QString teamId = s.value(QStringLiteral("auth/team_id")).toString();
    const QString name   = s.value(QStringLiteral("auth/team_name")).toString();
    if (xoxp.isEmpty()) {
        s.remove(QStringLiteral("auth"));
        return;
    }
    const QString id = teamId.isEmpty() ? QStringLiteral("legacy") : teamId;
    s.setValue(QStringLiteral("workspace/%1/xoxp").arg(id), xoxp);
    s.setValue(QStringLiteral("workspace/%1/name").arg(id), name);
    s.setValue(QStringLiteral("workspace/%1/iconUrl").arg(id), QString());
    s.setValue(QStringLiteral("workspaces"), QStringList{id});
    s.setValue(QStringLiteral("active"), id);
    s.remove(QStringLiteral("auth"));
}

// v1 → v2: bare-id entries were all Slack. Promote each to a composite handle
// (`slack:{id}`) and pack the token-shaped fields into the opaque `auth` blob.
// The blob's JSON shape is the contract shared with slack::Credentials
// (see backend/slack/slack_auth.cpp); this is the only spot outside slack:: that
// touches it, and only on this cold one-time path.
void migrateV1toV2(QSettings &s) {
    const QStringList ids = s.value(QStringLiteral("workspaces")).toStringList();
    QStringList       handles;
    for (const QString &id : ids) {
        if (WorkspaceKey::fromString(id)) { // already a handle — leave alone
            handles << id;
            continue;
        }
        const QString oldBase = QStringLiteral("workspace/") + id;
        QJsonObject   blob;
        blob[QStringLiteral("xoxp")]         = s.value(oldBase + "/xoxp").toString();
        blob[QStringLiteral("refreshToken")] = s.value(oldBase + "/refreshToken").toString();
        blob[QStringLiteral("expiresAt")] =
            QString::number(s.value(oldBase + "/expiresAt").toLongLong());

        const WorkspaceKey key{Service::Slack, id};
        const QString      base = recordBase(key);
        s.setValue(base + "/displayName", s.value(oldBase + "/name").toString());
        s.setValue(base + "/iconUrl", s.value(oldBase + "/iconUrl").toString());
        s.setValue(base + "/auth", QJsonDocument(blob).toJson(QJsonDocument::Compact));
        s.remove(oldBase);
        handles << key.toString();
    }
    s.setValue(QStringLiteral("workspaces"), handles);

    const QString active = s.value(QStringLiteral("active")).toString();
    if (!active.isEmpty() && !WorkspaceKey::fromString(active))
        s.setValue(QStringLiteral("active"), WorkspaceKey{Service::Slack, active}.toString());
}

// Guarded, idempotent. Cold path: only ever runs on first launch after upgrade.
void migrate(QSettings &s) {
    if (s.value(QStringLiteral("storeVersion"), 0).toInt() >= kStoreVersion)
        return;
    migrateV0toV1(s);
    migrateV1toV2(s);
    s.setValue(QStringLiteral("storeVersion"), kStoreVersion);
}

} // namespace

void TokenStore::saveWorkspace(const WorkspaceRecord &c) {
    auto s = settings();
    migrate(s);
    auto       ids    = s.value(QStringLiteral("workspaces")).toStringList();
    const auto handle = c.key.toString();
    if (!ids.contains(handle))
        ids.append(handle);
    s.setValue(QStringLiteral("workspaces"), ids);
    const auto base = recordBase(c.key);
    s.setValue(base + "/displayName", c.displayName);
    s.setValue(base + "/iconUrl", c.iconUrl);
    s.setValue(base + "/auth", c.auth);
}

std::optional<TokenStore::WorkspaceRecord> TokenStore::loadWorkspace(const WorkspaceKey &key) {
    auto       s    = settings();
    const auto base = recordBase(key);
    if (!s.contains(base + "/displayName") && !s.contains(base + "/auth"))
        return std::nullopt;
    WorkspaceRecord r;
    r.key         = key;
    r.displayName = s.value(base + "/displayName").toString();
    r.iconUrl     = s.value(base + "/iconUrl").toString();
    r.auth        = s.value(base + "/auth").toByteArray();
    return r;
}

void TokenStore::removeWorkspace(const WorkspaceKey &key) {
    auto s = settings();
    migrate(s);
    const auto handle = key.toString();
    auto       ids    = s.value(QStringLiteral("workspaces")).toStringList();
    ids.removeAll(handle);
    s.setValue(QStringLiteral("workspaces"), ids);
    s.remove(recordBase(key));
    if (s.value(QStringLiteral("active")).toString() == handle)
        s.setValue(QStringLiteral("active"), ids.isEmpty() ? QString() : ids.first());
}

std::vector<WorkspaceKey> TokenStore::workspaceKeys() {
    auto s = settings();
    migrate(s);
    std::vector<WorkspaceKey> keys;
    for (const auto &h : s.value(QStringLiteral("workspaces")).toStringList())
        if (auto k = WorkspaceKey::fromString(h))
            keys.push_back(*k);
    return keys;
}

void TokenStore::setWorkspaceOrder(const std::vector<WorkspaceKey> &ordered) {
    auto s = settings();
    migrate(s);
    const auto  existing = s.value(QStringLiteral("workspaces")).toStringList();
    QStringList next;
    for (const auto &k : ordered) {
        const auto h = k.toString();
        if (existing.contains(h) && !next.contains(h))
            next.append(h);
    }
    for (const auto &h : existing)
        if (!next.contains(h))
            next.append(h);
    s.setValue(QStringLiteral("workspaces"), next);
}

bool TokenStore::hasAnyWorkspace() {
    return !workspaceKeys().empty();
}

bool TokenStore::isWorkspaceMuted(const WorkspaceKey &key) {
    return settings().value(recordBase(key) + "/muted", false).toBool();
}

void TokenStore::setWorkspaceMuted(const WorkspaceKey &key, bool muted) {
    auto s = settings();
    if (muted)
        s.setValue(recordBase(key) + "/muted", true);
    else
        s.remove(recordBase(key) + "/muted");
}

std::optional<WorkspaceKey> TokenStore::activeWorkspace() {
    auto s = settings();
    migrate(s);
    return WorkspaceKey::fromString(s.value(QStringLiteral("active")).toString());
}

void TokenStore::setActiveWorkspace(const WorkspaceKey &key) {
    settings().setValue(QStringLiteral("active"), key.toString());
}
