// Crash reports and the dev-only main-thread hang watchdog (crash.log keeps
// the format earlier versions wrote).
#pragma once

#include <string>

namespace crash {

// Installs hooks for fatal signals (Linux/macOS) and unhandled SEH exceptions
// plus abort() (Windows). On a crash they append a stack trace — with app
// version, build timestamp and executable base address — to stderr and to
// `logPath` (its directory is created here; identity::crashLogPath, the old
// app's file, so reports from both apps land in one), then re-raise so the OS
// default (core dump / WER) still runs. Call once, early.
void install(const std::string &logPath);

// Starts the main-thread hang watchdog (Linux dev builds only, see
// MSGA_HANG_WATCHDOG; a no-op elsewhere). After this, the main thread must
// call heartbeat() periodically (a 1 s timer on the event loop): if it stops
// for `timeoutMs`, a backtrace of the wedged main thread is appended to
// stderr and crash.log — same format as a crash report. No idle wakeups: a
// one-shot timer re-armed past its own deadline never fires until the main
// thread actually stalls. MSGA_WATCHDOG_DISABLE=1 turns it off (debuggers);
// MSGA_WATCHDOG_ABORT=1 makes a confirmed hang abort() (core dump) instead of
// letting the operation continue. Call once, on the main thread, after install().
// Returns whether the watchdog is armed (only then does heartbeat() matter).
bool startWatchdog(int timeoutMs = 5000);

// Re-arms the watchdog deadline: the event loop is still pumping. One
// timer_settime call; a no-op until startWatchdog() and on other builds. The
// first call also retires the wider startup window (watchdogStartupGraceMs).
void heartbeat();

// The watchdog's FIRST window (process startup, before the event loop's
// heartbeat takes over) is wider than `steadyMs`: building the whole UI is one
// uninterrupted burst of main-thread work. Returns `steadyMs` unchanged on
// builds without the watchdog. For tests.
int watchdogStartupGraceMs(int steadyMs);

// Hang reports emitted this process (tests); always 0 without the watchdog.
int watchdogReportCountForTesting();

} // namespace crash
