// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "slack_module.h"

#include "backend/backend_registry.h"
#include "backend/slack/oauth_flow.h"
#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"

namespace slack {

void registerBackend() {
    backends::registerBackend({
        .service     = kService,
        .displayName = QStringLiteral("Slack"),
        .pickerOrder = 0,
        // PublicBackend reads its own app-config + acquires the refcounted shared
        // Socket Mode socket.
        .makeBackend = [](const TokenStore::WorkspaceRecord &rec) -> std::unique_ptr<Backend> {
            return std::make_unique<PublicBackend>(fromRecord(rec));
        },
        // The OAuth ("app keys") flow. The default Slack sign-in — session import —
        // is a UI flow of its own (MainWindow::connectSlack).
        .makeAuthStrategy = [](QObject *parent) -> std::unique_ptr<auth::AuthStrategy> {
            return std::make_unique<OAuthFlow>(appConfig(), parent);
        },
    });
}

} // namespace slack
