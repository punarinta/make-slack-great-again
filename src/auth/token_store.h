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
std::vector<WorkspaceKey>      workspaceKeys();
// Persist a new display order. Unknown keys are ignored; known keys missing
// from `ordered` are appended at the end so no workspace is ever lost.
void                           setWorkspaceOrder(const std::vector<WorkspaceKey> &ordered);
bool                           hasAnyWorkspace();

// High-level per-workspace mute switch. Independent of any conversation's own
// notification settings: while muted, the app suppresses OS notifications and
// the tray badge for the workspace's events, but in-app unread counters and
// chat emphasis are unaffected. Stored separately from the credential record so
// toggling it never has to rewrite the opaque auth blob.
bool                        isWorkspaceMuted(const WorkspaceKey &key);
void                        setWorkspaceMuted(const WorkspaceKey &key, bool muted);
std::optional<WorkspaceKey> activeWorkspace();
void                        setActiveWorkspace(const WorkspaceKey &key);

} // namespace TokenStore
