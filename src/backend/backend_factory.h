// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "auth/token_store.h"

#include <memory>

class Backend;

// Construct the backend for a workspace, dispatching on its service. The
// signature is NEUTRAL — no service-specific type crosses it. Each service case
// owns decoding the record's opaque `auth` blob into its own credential shape;
// this function is the single point that switches *into* a service's namespace.
std::unique_ptr<Backend> makeBackend(const TokenStore::WorkspaceRecord &rec);
