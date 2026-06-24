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

// Starts a main-thread hang watchdog (Linux only; a no-op elsewhere). After
// this, the main thread must call heartbeat() periodically (e.g. from a 1s
// QTimer driven by the event loop): if it stops for `timeoutMs`, a backtrace of
// the wedged main thread is appended to stderr and crash.log — same format and
// symbolication as a crash report — so a silent forever-hang becomes a
// diagnosable, on-record event. Call once, on the main thread, after install().
// Steady-state cost is negligible (no idle wakeups: a one-shot timer re-armed
// past its own deadline never fires until the main thread actually stalls).
// MSGA_WATCHDOG_DISABLE=1 turns it off (use under a debugger, where a breakpoint
// legitimately freezes the main thread); MSGA_WATCHDOG_ABORT=1 makes a confirmed
// hang abort() — core dump via the crash path — instead of letting the
// operation continue.
void startWatchdog(int timeoutMs = 5000);

// Re-arms the watchdog deadline; proves the GUI event loop is still pumping.
// Cheap — one timer_settime syscall. No-op until startWatchdog() and on
// non-Linux. Call only from the main thread.
void heartbeat();

} // namespace CrashHandler
