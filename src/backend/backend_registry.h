// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// The backend registry: which services this build can host, and how to build
// each one's Backend and sign-in flow. Every backend module registers ONE
// descriptor at startup (registerBuiltinBackends, builtin_backends.cpp) and does
// nothing else until a workspace for it exists — a registered backend with no
// workspace costs no CPU and no private memory (docs/backend-modules-plan.md §4.5).
#pragma once

#include "auth/token_store.h"
#include "backend/domain.h"

#include <functional>
#include <memory>
#include <vector>

class Backend;
class QObject;
namespace auth {
class AuthStrategy;
}

struct BackendDescriptor {
    Service service;
    QString displayName;         // add-workspace picker label, e.g. "Microsoft Teams"
    int     pickerOrder     = 0; // position in the add-workspace picker; < 0 = never offered
    // At most one workspace of this service: the picker greys it out once one is
    // connected (Claude Code is "the sessions on this machine").
    bool    singleWorkspace = false;
    // Its messages may be forwarded into any other workspace, not just its own
    // conversations (Claude Code: hand an agent's answer to the team).
    bool    forwardAnywhere = false;
    // Builds the backend for a stored workspace. Owns decoding the record's opaque
    // `auth` blob. May return nullptr (e.g. a record it can't decode).
    std::function<std::unique_ptr<Backend>(const TokenStore::WorkspaceRecord &)> makeBackend;
    // Builds the add-workspace sign-in flow; empty for a service with nothing to
    // sign in to (Demo is seeded from a fixture).
    std::function<std::unique_ptr<auth::AuthStrategy>(QObject *parent)>          makeAuthStrategy;
};

namespace backends {

// Adds (or replaces, by service) a descriptor. Called from each module's
// registerBackend() during startup, before any workspace is loaded.
void registerBackend(BackendDescriptor d);

// nullptr when the service isn't in this build.
const BackendDescriptor *find(const Service &service);
bool                     isRegistered(const Service &service);

// The picker's human name for a service; the raw token for an unknown one.
QString displayName(const Service &service);

// Services the add-workspace picker offers, in pickerOrder.
std::vector<Service> pickerServices();

// Test hook: forget every registration.
void clearForTests();

} // namespace backends

// Registers every backend compiled into this build (builtin_backends.cpp, one
// explicit call per MSGA_BACKEND_* switch — never self-registering statics,
// which a static link can silently drop). Idempotent.
void registerBuiltinBackends();
