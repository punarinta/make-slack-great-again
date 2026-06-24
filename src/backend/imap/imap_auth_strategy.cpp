// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_auth_strategy.h"

namespace imap {

AuthStrategy::Prompt AuthStrategy::s_prompt;

void AuthStrategy::setPrompt(Prompt p) {
    s_prompt = std::move(p);
}

void AuthStrategy::start() {
    if (!s_prompt) {
        emit failed(QStringLiteral("no_credential_prompt")); // UI didn't install one
        return;
    }
    s_prompt(parent(), [this](std::optional<Credentials> c) {
        if (c)
            emit succeeded(toRecord(*c));
        else
            emit failed(QStringLiteral("cancelled"));
    });
}

} // namespace imap
