// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <memory>
#include <vector>

class QObject;

namespace auth {

class AuthStrategy;

// Construct the auth strategy for a service. The signature is NEUTRAL — no
// service-specific type crosses it; each case switches *into* a service's
// namespace (mirroring makeBackend). Returns nullptr for a service with no
// registered strategy. The returned object owns its own lifetime via `parent`.
std::unique_ptr<AuthStrategy> makeAuthStrategy(Service service, QObject *parent = nullptr);

// Services the user can currently add a workspace for (i.e. that have a
// registered strategy). The add-workspace UI offers a picker over these; with a
// single entry the picker is skipped and that service is auto-selected.
std::vector<Service> registeredAuthServices();

} // namespace auth
