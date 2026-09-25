// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "auth/token_store.h"

#include <memory>

class Backend;

// Construct the backend for a workspace through the backend registry
// (backend_registry.h). The signature is NEUTRAL — no service-specific type
// crosses it; each registered service owns decoding the record's opaque `auth`
// blob into its own credential shape. nullptr for a service not in this build.
std::unique_ptr<Backend> makeBackend(const TokenStore::WorkspaceRecord &rec);
