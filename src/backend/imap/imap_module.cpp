// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "imap_module.h"

#include "backend/backend_registry.h"
#include "backend/imap/imap_auth.h"
#include "backend/imap/imap_auth_strategy.h"
#include "backend/imap/imap_backend.h"

namespace imap {

void registerBackend() {
    backends::registerBackend({
        .service     = kService,
        .displayName = QStringLiteral("Email (IMAP)"),
        .pickerOrder = 20,
        // imap-backend-plan §1: the backend decodes its own credentials blob and
        // connects on construction.
        .makeBackend = [](const TokenStore::WorkspaceRecord &rec) -> std::unique_ptr<::Backend> {
            return std::make_unique<Backend>(fromRecord(rec));
        },
        // imap-backend-plan §5. The strategy is Widgets-free; the add-account
        // dialog is injected via AuthStrategy::setPrompt (registerBuiltinBackendUis).
        .makeAuthStrategy = [](QObject *parent) -> std::unique_ptr<auth::AuthStrategy> {
            return std::make_unique<AuthStrategy>(parent);
        },
    });
}

} // namespace imap
