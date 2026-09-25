// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

namespace imap {

// Adds this backend's descriptor to the backend registry. Called once from
// registerBuiltinBackends(); constructs nothing else.
void registerBackend();

} // namespace imap
