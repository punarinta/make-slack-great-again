// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Adding the Claude Code workspace. There is nothing to sign in to: the
// workspace is "the Claude Code sessions on this machine", read from Claude
// Code's state directory, plus the `claude` CLI for the sessions msga starts
// itself. The add flow only checks both exist and records where the CLI is.
#pragma once

#include "auth/auth_strategy.h"
#include "auth/token_store.h"
#include "backend/domain.h"

#include <QString>

namespace claude_code {

// This backend's service. The token is stored in workspace handles — never change it.
inline const Service kService{QStringLiteral("claude-code")};

// One workspace per machine.
inline const QString kWorkspaceId = QStringLiteral("local");

struct Credentials {
    QString claudePath; // the `claude` executable; empty = not found (read-only use)
    bool    operator==(const Credentials &) const = default;
};

TokenStore::WorkspaceRecord toRecord(const Credentials &creds);
Credentials                 fromRecord(const TokenStore::WorkspaceRecord &rec);

// The `claude` CLI: PATH first, then where its installers put it. On Windows
// an npm install is `claude.cmd`, the native one `claude.exe`. Empty if absent.
QString findClaudeExecutable();

class AuthStrategy : public auth::AuthStrategy {
    Q_OBJECT
public:
    using auth::AuthStrategy::AuthStrategy;
    void start() override;
};

} // namespace claude_code
