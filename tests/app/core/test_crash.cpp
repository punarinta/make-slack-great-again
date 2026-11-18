// The crash handler (a crashing child leaves its report in crash.log) and the
// dev-only main-thread hang watchdog. Its own process (BASE_TEST_SUITE_MAIN
// below): both install process-wide signal handlers and timers.
//
// The watchdog cases pin its startup window. Crash log 2026-06-30: under
// AddressSanitizer the watchdog fired *during* window construction
// — startup is one long synchronous burst with no event-loop turns to
// heartbeat through, yet it was armed with the tight steady-state window.
// Fix: the first window is much wider; the first heartbeat() drops to the
// steady one.
#include "app/crash/crash_handler.h"
#include "app/identity.h"
#include "base/file.h"
#include "support/test.h"
#include "plat/plat.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

namespace {

std::string g_log;

// Wall-clock wait that survives signal interruption, so the timer signal is
// delivered to this (the main) thread while we wait.
void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool watchdogBuilt() {
    return crash::watchdogStartupGraceMs(5000) != 5000;
}

} // namespace

TEST("crash: crash.log lives in the data directory") {
    auto app = plat::App::create();
    REQUIRE(app);
    const std::string data = app->standardDir(plat::StandardDir::Data);
    CHECK_STR(identity::crashLogPath(*app), file::join(data, "msga/MSGA/crash.log"));
}

// A port build has no crash handler (crash_handler.cpp: the port's OS keeps
// its own records).
#ifndef MSGA_PORT
TEST("crash: a crash appends a report with its signal and stack to crash.log") {
    file::remove(g_log);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(2); // keep the report off the test output
        volatile int *null = nullptr;
        *null              = 1;
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV); // still dies as before
    std::string log;
    REQUIRE(file::readAll(g_log, &log));
    CHECK(log.find("==== msga crash report ====") != std::string::npos);
    CHECK(log.find("version: ") != std::string::npos);
    CHECK(log.find("executable base: 0x") != std::string::npos);
    CHECK(log.find("signal: SIGSEGV, fault address: 0x0000000000000000") != std::string::npos);
    CHECK(log.find("\nstack:\n") != std::string::npos);
    CHECK(log.find("==== end of crash report ====") != std::string::npos);
}

TEST("crash: abort() is reported too") {
    file::remove(g_log);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(2);
        std::abort();
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    std::string log;
    REQUIRE(file::readAll(g_log, &log));
    CHECK(log.find("signal: SIGABRT\n") != std::string::npos);
}
#endif

TEST("watchdog: the startup window is substantially wider than the steady one") {
    if (!watchdogBuilt())
        return;                                           // release builds: not compiled
    CHECK(crash::watchdogStartupGraceMs(5000) > 5000);    // default
    CHECK(crash::watchdogStartupGraceMs(20000) >= 60000); // ASan default
    CHECK(crash::watchdogStartupGraceMs(0) > 0);          // degenerate input, usable window
}

TEST("watchdog: finite startup work shorter than the grace does not trip it") {
    if (!watchdogBuilt())
        return;
    const int before = crash::watchdogReportCountForTesting();
    REQUIRE(crash::startWatchdog(200)); // startup grace = 1200 ms (×6)
    // A long synchronous startup: never heartbeat. Past the steady window but
    // under the startup grace — where the old false positive fired.
    sleepMs(600);
    CHECK(crash::watchdogReportCountForTesting() == before);
    // A genuine forever-hang is still caught once the grace elapses.
    sleepMs(900);
    CHECK(crash::watchdogReportCountForTesting() == before + 1);
    std::string log;
    file::readAll(g_log, &log);
    CHECK(log.find("main thread unresponsive (hang watchdog)") != std::string::npos);
    CHECK(log.find("==== end of hang report ====") != std::string::npos);
}

TEST("watchdog: a heartbeat keeps the steady window from firing") {
    if (!watchdogBuilt())
        return;
    crash::heartbeat(); // clear any earlier stall
    const int before = crash::watchdogReportCountForTesting();
    REQUIRE(crash::startWatchdog(400));
    crash::heartbeat(); // event loop alive → the startup grace is over
    for (int i = 0; i < 10; ++i) {
        sleepMs(60); // faster than the 400 ms deadline
        crash::heartbeat();
    }
    CHECK(crash::watchdogReportCountForTesting() == before);
    // Disarmed for the rest of the run: a far deadline, never reached.
    crash::startWatchdog(3600 * 1000);
    crash::heartbeat();
}

BASE_TEST_SUITE_MAIN(argc, argv) {
    // The watchdog on and non-fatal whatever the shell says; reports go to a
    // throwaway crash.log next to the test's HOME.
    base::test::unsetEnv("MSGA_WATCHDOG_DISABLE");
    base::test::unsetEnv("MSGA_WATCHDOG_ABORT");
    const std::string dir = base::test::makeTempDir("msga-crash-test-");
    if (dir.empty())
        return 1;
    g_log = dir + "/msga/MSGA/crash.log";
    crash::install(g_log);
    const int rc = base::test::runSuite(argc, argv);
    base::test::removeTree(dir);
    return rc;
}
