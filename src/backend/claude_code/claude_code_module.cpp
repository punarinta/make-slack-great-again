// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "claude_code_module.h"

#include "backend/backend_registry.h"
#include "backend/claude_code/claude_code_auth.h"
#include "backend/claude_code/claude_code_backend.h"

namespace claude_code {

void registerBackend() {
    backends::registerBackend({
        .service     = kService,
        .displayName = QStringLiteral("Claude Code"),
        .pickerOrder = 30,
        .makeBackend = [](const TokenStore::WorkspaceRecord &rec) -> std::unique_ptr<::Backend> {
            return std::make_unique<Backend>(fromRecord(rec));
        },
        // No sign-in: checks Claude Code is set up and finds the CLI.
        .makeAuthStrategy = [](QObject *parent) -> std::unique_ptr<auth::AuthStrategy> {
            return std::make_unique<AuthStrategy>(parent);
        },
    });
}

} // namespace claude_code
