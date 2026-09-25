// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend_factory.h"

#include "backend/backend.h"
#include "backend/backend_registry.h"

std::unique_ptr<Backend> makeBackend(const TokenStore::WorkspaceRecord &rec) {
    // A workspace whose service isn't in this build gets no backend (its record
    // stays stored; the token store hides it from the visible list).
    const auto *d = backends::find(rec.key.service);
    if (!d || !d->makeBackend)
        return nullptr;
    return d->makeBackend(rec);
}
