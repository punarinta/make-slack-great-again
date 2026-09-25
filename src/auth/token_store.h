// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>

// Neutral, service-agnostic workspace registry. TokenStore stores *every*
// service's workspaces over one flat QSettings subtree (`workspace/{handle}/…`)
// keyed by the composite WorkspaceKey handle. It never interprets a workspace's
// credentials: per-service auth lives in an opaque `auth` blob that the owning
// service (slack::, telegram::, …) encodes/decodes itself.
namespace TokenStore {

// The common registry record — neutral. No tokens, no service specifics.
struct WorkspaceRecord {
    WorkspaceKey key;         // service + id — the app-wide handle
    QString      displayName; // human-facing workspace name
    QString      iconUrl;     // workspace icon URL, may be empty
    QByteArray   auth;        // opaque per-service blob; NEVER interpreted here
};

void                           saveWorkspace(const WorkspaceRecord &c);
std::optional<WorkspaceRecord> loadWorkspace(const WorkspaceKey &key);
void                           removeWorkspace(const WorkspaceKey &key);
// Only workspaces whose service passes the service filter (see below).
std::vector<WorkspaceKey>      workspaceKeys();
// Persist a new display order. Unknown keys are ignored; known keys missing
// from `ordered` are appended at the end so no workspace is ever lost.
void                           setWorkspaceOrder(const std::vector<WorkspaceKey> &ordered);
bool                           hasAnyWorkspace();

// Which services' workspaces are visible. The app installs "registered in this
// build" (backends::isRegistered) at startup, so a workspace of a backend that
// was compiled out is hidden from workspaceKeys()/activeWorkspace() — but never
// deleted: its record stays stored and reappears in a build that has the backend.
// No filter (the default, and what tests get) shows every stored workspace.
void setServiceFilter(bool (*isVisible)(const Service &));

// High-level per-workspace mute switch. Independent of any conversation's own
// notification settings: while muted, the app suppresses OS notifications and
// the tray badge for the workspace's events, but in-app unread counters and
// chat emphasis are unaffected. Stored separately from the credential record so
// toggling it never has to rewrite the opaque auth blob.
bool                        isWorkspaceMuted(const WorkspaceKey &key);
void                        setWorkspaceMuted(const WorkspaceKey &key, bool muted);
std::optional<WorkspaceKey> activeWorkspace();
void                        setActiveWorkspace(const WorkspaceKey &key);

// Local override for the workspace icon: a non-admin cannot change the icon
// Slack serves, so the user may pick one that only this install shows. Stored
// as the absolute path of a PNG the app owns (see CustomWorkspaceIcon), apart
// from the credential record so a re-login — which rewrites `iconUrl` from the
// server — never wipes it. Empty path = no override.
QString customWorkspaceIconPath(const WorkspaceKey &key);
void    setCustomWorkspaceIconPath(const WorkspaceKey &key, const QString &path);
// What the UI shows for the workspace: the custom icon as a file:// URL when
// one is set and its file still exists, else the record's server icon URL.
// Every icon consumer (rail, quick switcher, notifications) goes through here.
QString displayIconUrl(const WorkspaceRecord &rec);

} // namespace TokenStore
