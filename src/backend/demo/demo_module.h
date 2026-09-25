// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

namespace demo {

// Adds this backend's descriptor to the backend registry. Called once from
// registerBuiltinBackends(); constructs nothing else.
void registerBackend();

} // namespace demo
