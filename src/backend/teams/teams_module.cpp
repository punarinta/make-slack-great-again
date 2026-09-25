// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "teams_module.h"

#include "backend/backend_registry.h"
#include "backend/teams/oauth_flow.h"
#include "backend/teams/teams_auth.h"
#include "backend/teams/teams_backend.h"

namespace teams {

void registerBackend() {
    backends::registerBackend({
        .service     = kService,
        .displayName = QStringLiteral("Microsoft Teams"),
        .pickerOrder = 10,
        // Graph (delegated). Backend reads its own compiled-in app-config and
        // decodes the per-service auth blob.
        .makeBackend = [](const TokenStore::WorkspaceRecord &rec) -> std::unique_ptr<::Backend> {
            return std::make_unique<Backend>(fromRecord(rec));
        },
        // Auth Code + PKCE (public client) over Microsoft identity.
        .makeAuthStrategy = [](QObject *parent) -> std::unique_ptr<auth::AuthStrategy> {
            return std::make_unique<OAuthFlow>(appConfig(), parent);
        },
    });
}

} // namespace teams
