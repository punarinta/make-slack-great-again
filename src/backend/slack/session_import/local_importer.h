// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// One-click import of the locally-installed Slack desktop app's session: reads and
// decrypts the `d` cookie from its Chromium cookie store and the signed-in
// workspaces (+ xoxc tokens) from its localStorage. Best-effort and inherently
// version/OS-specific — every failure path returns a LocalImport with an `error`
// so the UI transparently falls back to the guided manual-paste flow.
//
// Availability: the real implementation is compiled only when MSGA_SLACK_SESSION_IMPORT
// is defined (Linux + OpenSSL + Qt6::Sql found at configure time). Otherwise this is
// a stub returning error="unsupported_platform". macOS/Windows are follow-up phases.
#pragma once

#include "backend/slack/session_import/session_types.h"

namespace slack::session {

// Returns true when a real local-import backend is compiled in for this platform.
// The UI uses it to decide whether to offer the "Import from local Slack" button
// or go straight to guided manual paste.
[[nodiscard]] bool localImportSupported();

// Synchronous: reads + decrypts the local Slack session. Touches only local files
// (cookie SQLite opened read-only, leveldb scanned read-only) plus, on Linux, a
// read-only Secret Service lookup for the cookie-encryption key.
LocalImport importLocalSlackSession();

} // namespace slack::session
