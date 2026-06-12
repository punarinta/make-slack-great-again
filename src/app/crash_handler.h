// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

namespace CrashHandler {

// Installs hooks for fatal signals (Linux/macOS) and unhandled SEH exceptions
// (Windows). On a crash they append a stack trace — with app version, build
// timestamp and executable base address — to stderr and to crash.log in
// AppDataLocation, then re-raise so the OS default (core dump / WER) still
// runs. Call once at startup, after the application and organization names
// are set: they determine the crash-log directory.
void install();

} // namespace CrashHandler
