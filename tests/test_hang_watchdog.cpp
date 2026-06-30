// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Regression tests for the main-thread hang watchdog's startup window.
//
// Crash log logs/2026-06-30_09-55-13.log: under AddressSanitizer the watchdog
// fired *during* MainWindow construction (snapshot: a stylesheet parse inside
// SettingsDialog::buildPanel) — a false positive. Cause: startup is one long
// synchronous burst on the main thread with no event-loop turns to heartbeat
// through, yet it was armed with the same tight steady-state window used for a
// runtime stall. ASan (esp. fast_unwind_on_malloc=0) made finite construction
// outrun that window. Fix: arm the *first* window with a much wider startup
// grace; the first heartbeat() drops to the steady window. These tests pin that
// behaviour.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/crash_handler.h"

#include <QStandardPaths>
#include <QtGlobal>

int main(int argc, char **argv) {
    // Keep the watchdog enabled and non-fatal regardless of the dev's shell env,
    // and route any report it writes to a throwaway crash.log.
    qunsetenv("MSGA_WATCHDOG_DISABLE");
    qunsetenv("MSGA_WATCHDOG_ABORT");
    QStandardPaths::setTestModeEnabled(true);
    CrashHandler::install(); // sets the crash-log path + warms the unwinder
    return Catch::Session().run(argc, argv);
}

#if defined(MSGA_HANG_WATCHDOG)

#include <chrono>
#include <thread>

// Wall-clock wait that survives signal interruption (sleep_for loops to its
// absolute deadline even when the watchdog signal hits mid-sleep), so the timer
// signal is delivered to this (the main) thread while we wait.
static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

TEST_CASE("startup grace window is substantially wider than the steady window") {
    // This is the invariant the false positive violated: startup used the
    // steady window (a multiplier of 1). A slow-but-finite construction must
    // not be able to outrun the startup window before the first heartbeat.
    CHECK(CrashHandler::watchdogStartupGraceMs(5000) > 5000);   // release default
    CHECK(CrashHandler::watchdogStartupGraceMs(20000) > 20000); // ASan default
    CHECK(CrashHandler::watchdogStartupGraceMs(20000) >= 60000);
    // A degenerate steady timeout still yields a positive, usable window.
    CHECK(CrashHandler::watchdogStartupGraceMs(0) > 0);
}

TEST_CASE("startup window is not tripped by finite work shorter than the grace") {
    const int before = CrashHandler::watchdogReportCountForTesting();
    const int steady = 200; // startup grace = 1200 ms (×6)
    CrashHandler::startWatchdog(steady);

    // Simulate a long synchronous startup: never heartbeat. Wait well past the
    // steady window but comfortably under the startup grace — pre-fix this is
    // where the watchdog falsely fired.
    sleepMs(600);
    REQUIRE(CrashHandler::watchdogReportCountForTesting() == before);

    // A genuine forever-hang is still caught once the grace window elapses.
    sleepMs(900); // ~1500 ms total > 1200 ms grace
    REQUIRE(CrashHandler::watchdogReportCountForTesting() == before + 1);
}

TEST_CASE("heartbeat keeps the steady window from firing") {
    CrashHandler::heartbeat(); // clear any prior stall report
    const int before = CrashHandler::watchdogReportCountForTesting();

    CrashHandler::startWatchdog(400);
    CrashHandler::heartbeat(); // event loop alive → retire startup grace
    for (int i = 0; i < 10; ++i) {
        sleepMs(60); // pet faster than the 400 ms steady deadline
        CrashHandler::heartbeat();
    }
    CHECK(CrashHandler::watchdogReportCountForTesting() == before);
}

#else // !MSGA_HANG_WATCHDOG

TEST_CASE("hang watchdog is not compiled on this platform/build") {
    SUCCEED("MSGA_HANG_WATCHDOG undefined — watchdog is dev/Linux only");
}

#endif
