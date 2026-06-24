// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// IMAP auth strategy (imap-backend-plan §5). Deliberately tiny and Widgets-free
// so the neutral auth factory stays free of any QtWidgets dependency: the actual
// "Email + auto-detect" dialog lives in src/ui/ and is injected as a prompt hook
// at startup. start() runs the prompt, then maps the validated credentials to a
// neutral WorkspaceRecord (succeeded) or reports cancellation (failed).
#pragma once

#include "auth/auth_strategy.h"
#include "backend/imap/imap_auth.h"

#include <functional>
#include <optional>

namespace imap {

class AuthStrategy : public auth::AuthStrategy {
    Q_OBJECT
public:
    using auth::AuthStrategy::AuthStrategy;

    void start() override;

    // Collects + validates credentials (the UI shows the add-account dialog and
    // calls `done` with the result, or std::nullopt if cancelled). Installed once
    // at app startup so neither this strategy nor the auth factory link QtWidgets.
    using Prompt =
        std::function<void(QObject *parent, std::function<void(std::optional<Credentials>)> done)>;
    static void setPrompt(Prompt p);

private:
    static Prompt s_prompt;
};

} // namespace imap
