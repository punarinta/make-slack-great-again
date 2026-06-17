// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend_factory.h"

#include "backend/backend.h"
#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"

std::unique_ptr<Backend> makeBackend(const TokenStore::WorkspaceRecord &rec) {
    switch (rec.key.service) {
    case Service::Slack:
        // The only place Slack credential types appear above the adapter: the
        // switch case that builds the Slack backend. slack::PublicBackend reads
        // its own app-config + acquires the refcounted shared Socket Mode socket.
        return std::make_unique<slack::PublicBackend>(slack::fromRecord(rec));
    }
    return nullptr;
}
