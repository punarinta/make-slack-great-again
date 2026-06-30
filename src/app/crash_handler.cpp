// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov

// The hang watchdog needs SIGEV_THREAD_ID and the sigev_notify_thread_id member
// macro (glibc gates both behind _GNU_SOURCE; musl exposes them under it too) to
// target the timer signal at the main thread. Must precede every system header.
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "app/crash_handler.h"

#include "app_credentials.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <cstdio>
#include <cstring>

#if defined(Q_OS_WIN)
#include <windows.h>
// windows.h must precede dbghelp.h
#include <dbghelp.h>

#include <csignal>
#else
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
#include <sys/syscall.h> // SYS_gettid
#include <time.h>        // timer_create / timer_settime (POSIX per-process timers)
#endif
// backtrace()/backtrace_symbols_fd() exist on glibc and macOS but not musl
// (the static Linux release builds on Alpine). There the libgcc unwinder
// walks the stack instead and frames print as raw addresses — resolvable
// offline with addr2line, since a static non-PIE binary loads at its link
// address.
#if (defined(__GLIBC__) || defined(Q_OS_MACOS)) && !defined(MSGA_NO_EXECINFO)
#define MSGA_HAVE_EXECINFO 1
#include <execinfo.h>
#else
#include <unwind.h>
#endif
#endif

namespace {

constexpr int kMaxFrames = 64;

// Everything the handlers need is precomputed at install time: a crash
// handler must not allocate or call into Qt.
char gHeader[512];
char gLogNote[1200];

#if !defined(Q_OS_WIN)

char gLogPath[1024];

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};

// Only async-signal-safe calls below: write/open/close/backtrace*.
void writeAll(int fd, const char *s, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= static_cast<size_t>(w);
    }
}

void writeStr(int fd, const char *s) {
    writeAll(fd, s, ::strlen(s));
}

void writePtr(int fd, const void *p) {
    char buf[2 + sizeof(void *) * 2];
    buf[0]      = '0';
    buf[1]      = 'x';
    auto      v = reinterpret_cast<quintptr>(p);
    const int n = static_cast<int>(sizeof(buf));
    for (int pos = n - 1; pos >= 2; --pos) {
        buf[pos] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    writeAll(fd, buf, sizeof(buf));
}

const char *signalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    case SIGBUS:
        return "SIGBUS";
    }
    return "fatal signal";
}

#if !defined(MSGA_HAVE_EXECINFO)
struct UnwindState {
    void **frames;
    int    max;
    int    count;
};

_Unwind_Reason_Code unwindStep(_Unwind_Context *ctx, void *arg) {
    auto *st = static_cast<UnwindState *>(arg);
    if (st->count >= st->max)
        return _URC_END_OF_STACK;
    if (void *ip = reinterpret_cast<void *>(_Unwind_GetIP(ctx)))
        st->frames[st->count++] = ip;
    return _URC_NO_REASON;
}
#endif

int captureFrames(void **frames, int max) {
#if defined(MSGA_HAVE_EXECINFO)
    return ::backtrace(frames, max);
#else
    // libgcc's unwinder knows the Linux signal trampoline, so this walks
    // through the handler into the crashed frames.
    UnwindState st{frames, max, 0};
    _Unwind_Backtrace(unwindStep, &st);
    return st.count;
#endif
}

void writeFrames(int fd, void *const *frames, int depth) {
#if defined(MSGA_HAVE_EXECINFO)
    ::backtrace_symbols_fd(frames, depth, fd);
#else
    for (int i = 0; i < depth; ++i) {
        writePtr(fd, frames[i]);
        writeAll(fd, "\n", 1);
    }
#endif
}

void fatalSignal(int sig, siginfo_t *info, void *) {
    for (const int s : kSignals)
        ::signal(s, SIG_DFL); // a fault inside the handler takes the default path

    void     *frames[kMaxFrames];
    const int depth = captureFrames(frames, kMaxFrames);

    const int logFd = ::open(gLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    const int fds[] = {STDERR_FILENO, logFd};
    for (const int fd : fds) {
        if (fd < 0)
            continue;
        writeStr(fd, gHeader);
        writeStr(fd, "signal: ");
        writeStr(fd, signalName(sig));
        if (info && (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL)) {
            writeStr(fd, ", fault address: ");
            writePtr(fd, info->si_addr);
        }
        writeStr(fd, "\nstack:\n");
        writeFrames(fd, frames, depth);
        writeStr(fd, "==== end of crash report ====\n");
    }
    if (logFd >= 0) {
        ::close(logFd);
        writeStr(STDERR_FILENO, gLogNote);
    }
    ::raise(sig); // default action now: terminate + core dump, as before
}

#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
// ── Main-thread hang watchdog ───────────────────────────────────────────────
// A POSIX per-process timer, re-armed to a future deadline by the main thread on
// every heartbeat(). While the GUI event loop keeps pumping, the deadline is
// pushed out before it can elapse, so the timer never fires — no idle wakeups,
// no signals delivered on the healthy path. If the main thread wedges (infinite
// loop or deadlock) and stops calling heartbeat(), the deadline elapses and the
// kernel delivers the timer signal *to the main thread itself* (SIGEV_THREAD_ID):
// the handler then runs on the stuck stack, so the backtrace points straight at
// the hang. Same async-signal-safe discipline and capture path as the crash
// handler above. Dev builds only (see MSGA_HANG_WATCHDOG in CMakeLists.txt).

timer_t               gWdTimer;
bool                  gWdArmed       = false;
bool                  gWdAbort       = false;
long                  gWdTimeoutMs   = 5000; // steady-state window (event loop alive)
volatile sig_atomic_t gWdReported    = 0;    // at most one report per stall episode
volatile sig_atomic_t gWdReportCount = 0;    // total reports this process (tests)

// Startup gets a much wider window than a steady-state stall. Building and
// polishing the whole UI tree (MainWindow → all sub-widgets, incl. the settings
// panel's stylesheets) is one long synchronous burst on the main thread — there
// are no event-loop turns to heartbeat through until app.exec(), so the very
// first window has to cover all of it in one go. Under AddressSanitizer (esp.
// with fast_unwind_on_malloc=0, where every allocation captures a full
// backtrace) that burst can take far longer than any later stall window — long
// enough to false-trip the watchdog mid-construction. The first heartbeat()
// (proof the event loop is pumping) drops back to the steady window, so a true
// forever-hang during startup is still caught, just with a roomier deadline.
constexpr int kStartupGraceMultiplier = 6;

long watchdogStartupGraceMsImpl(long steadyMs) {
    const long base = steadyMs > 0 ? steadyMs : 5000;
    return base * kStartupGraceMultiplier;
}

// glibc/musl already advance SIGRTMIN past the RT signals they reserve
// internally; +3 leaves further margin and stays well below SIGRTMAX.
int watchdogSig() {
    return SIGRTMIN + 3;
}

void wdArm(long ms) {
    struct itimerspec its{}; // it_interval left zero → one-shot
    its.it_value.tv_sec  = ms / 1000;
    its.it_value.tv_nsec = (ms % 1000) * 1000000L;
    ::timer_settime(gWdTimer, 0, &its, nullptr);
}

void watchdogExpired(int, siginfo_t *, void *) {
    if (gWdReported)
        return; // already dumped this stall; heartbeat() clears it on recovery
    gWdReported    = 1;
    gWdReportCount = gWdReportCount + 1;

    void     *frames[kMaxFrames];
    const int depth = captureFrames(frames, kMaxFrames);

    const int logFd = ::open(gLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    const int fds[] = {STDERR_FILENO, logFd};
    for (const int fd : fds) {
        if (fd < 0)
            continue;
        writeStr(fd, gHeader);
        writeStr(fd, "main thread unresponsive (hang watchdog)\nstack:\n");
        writeFrames(fd, frames, depth);
        writeStr(fd, "==== end of hang report ====\n");
    }
    if (logFd >= 0) {
        ::close(logFd);
        writeStr(STDERR_FILENO, gLogNote);
    }
    // Default: return and let the main thread carry on — a slow-but-finite
    // operation finishes, a true infinite loop simply resumes (now on record).
    // MSGA_WATCHDOG_ABORT=1 instead turns a confirmed hang into a clean abort
    // (core dump via the crash handler) rather than leaving a dead window up.
    if (gWdAbort)
        ::abort();
}

void startWatchdogImpl(int timeoutMs) {
    if (qEnvironmentVariableIntValue("MSGA_WATCHDOG_DISABLE") == 1)
        return; // e.g. under a debugger, where a breakpoint freezes main for minutes
    gWdTimeoutMs = timeoutMs > 0 ? timeoutMs : 5000;
    gWdAbort     = qEnvironmentVariableIntValue("MSGA_WATCHDOG_ABORT") == 1;

    // Idempotent: a second startWatchdog() must not leave the previous timer
    // armed (a leaked, still-pending one-shot would fire a spurious report).
    if (gWdArmed) {
        ::timer_delete(gWdTimer);
        gWdArmed = false;
    }

    const int        sig = watchdogSig();
    struct sigaction sa{};
    sa.sa_sigaction = watchdogExpired;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESTART; // reuse install()'s altstack
    sigemptyset(&sa.sa_mask);
    if (::sigaction(sig, &sa, nullptr) != 0)
        return;

    struct sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID; // deliver to one specific thread (the main one)
    sev.sigev_signo  = sig;
    // The LWP-id field has no portable accessor: musl exposes the POSIX-style
    // `sigev_notify_thread_id` macro, while glibc only names the internal union
    // member `_sigev_un._tid` (it defines macros for the _function/_attributes
    // members but not this one). Pick whichever the libc provides.
#if defined(sigev_notify_thread_id)
    sev.sigev_notify_thread_id = static_cast<pid_t>(::syscall(SYS_gettid));
#else
    sev._sigev_un._tid = static_cast<pid_t>(::syscall(SYS_gettid));
#endif
    if (::timer_create(CLOCK_MONOTONIC, &sev, &gWdTimer) != 0)
        return;

    gWdArmed = true;
    // Open the first window now (covers startup, before any heartbeat). Startup
    // is uninterrupted main-thread work, so it gets the wider grace window; the
    // first heartbeat() re-arms with the steady window.
    wdArm(watchdogStartupGraceMsImpl(gWdTimeoutMs));
}

void heartbeatImpl() {
    if (!gWdArmed)
        return;
    gWdReported = 0;     // main thread is alive → ready to report the next stall
    wdArm(gWdTimeoutMs); // push the deadline out
}
#endif // MSGA_HANG_WATCHDOG && __linux__

#else // Q_OS_WIN

wchar_t gLogPathW[1024];

void emitStr(HANDLE file, const char *s) {
    const DWORD n       = static_cast<DWORD>(::strlen(s));
    DWORD       written = 0;
    if (file != INVALID_HANDLE_VALUE)
        WriteFile(file, s, n, &written, nullptr);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE)
        WriteFile(err, s, n, &written, nullptr);
}

void dumpTrace(const char *what, const void *addr) {
    const HANDLE file = CreateFileW(
        gLogPathW,
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    char line[512];
    emitStr(file, gHeader);
    std::snprintf(line, sizeof(line), "%s at %p\nstack:\n", what, addr);
    emitStr(file, line);

    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // Names resolve only when symbols are available (PDB); plain release
    // builds still get usable addresses relative to the base in the header.
    const BOOL haveSyms = SymInitialize(proc, nullptr, TRUE);

    void                     *frames[kMaxFrames];
    const USHORT              depth = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
    for (USHORT i = 0; i < depth; ++i) {
        auto *sym         = reinterpret_cast<SYMBOL_INFO *>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 disp      = 0;
        if (haveSyms && SymFromAddr(proc, reinterpret_cast<DWORD64>(frames[i]), &disp, sym))
            std::snprintf(
                line,
                sizeof(line),
                "%p %s+0x%llx\n",
                frames[i],
                sym->Name,
                static_cast<unsigned long long>(disp)
            );
        else
            std::snprintf(line, sizeof(line), "%p\n", frames[i]);
        emitStr(file, line);
    }
    emitStr(file, "==== end of crash report ====\n");
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        emitStr(INVALID_HANDLE_VALUE, gLogNote);
    }
}

LONG WINAPI sehFilter(EXCEPTION_POINTERS *ex) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    char what[64];
    std::snprintf(
        what,
        sizeof(what),
        "exception: 0x%08lx",
        static_cast<unsigned long>(ex->ExceptionRecord->ExceptionCode)
    );
    dumpTrace(what, ex->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH; // WER / an attached debugger still runs
}

// assert()/std::terminate/qFatal end in abort(), which raises SIGABRT through
// the CRT instead of an SEH exception.
void abortHandler(int) {
    dumpTrace("abort()", nullptr);
    ::signal(SIGABRT, SIG_DFL);
    ::raise(SIGABRT);
}

#endif

} // namespace

namespace CrashHandler {

void install() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString logPath = dir + QStringLiteral("/crash.log");

    const void *base = nullptr;
#if defined(Q_OS_WIN)
    const QString clipped = logPath.left(static_cast<int>(std::size(gLogPathW)) - 1);
    gLogPathW[clipped.toWCharArray(gLogPathW)] = 0;

    base = GetModuleHandleW(nullptr);
    SetUnhandledExceptionFilter(sehFilter);
    ::signal(SIGABRT, abortHandler);
#else
    const QByteArray encoded = QFile::encodeName(logPath);
    std::snprintf(gLogPath, sizeof(gLogPath), "%s", encoded.constData());

    Dl_info di{};
    if (::dladdr(reinterpret_cast<void *>(&install), &di) != 0)
        base = di.dli_fbase;

    // The first backtrace() call may dlopen libgcc — do it here, not inside
    // the signal handler.
    void *warmup[2];
    captureFrames(warmup, 2);

    // A dedicated signal stack so stack-overflow SIGSEGVs still get a trace.
    static char altStack[64 * 1024];
    stack_t     ss{};
    ss.ss_sp   = altStack;
    ss.ss_size = sizeof(altStack);
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = fatalSignal;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (const int s : kSignals)
        ::sigaction(s, &sa, nullptr);
#endif

    std::snprintf(
        gHeader,
        sizeof(gHeader),
        "\n==== msga crash report ====\n"
        "version: %d (built %s)\n"
        "executable base: %p (addr2line -e msga -fC <frame address minus base>)\n",
        AppCredentials::version,
        AppCredentials::buildTimestamp,
        base
    );
    const QByteArray noteUtf8 = logPath.toUtf8();
    std::snprintf(
        gLogNote, sizeof(gLogNote), "crash report appended to %s\n", noteUtf8.constData()
    );
}

void startWatchdog(int timeoutMs) {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    startWatchdogImpl(timeoutMs);
#else
    (void)timeoutMs;
#endif
}

void heartbeat() {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    heartbeatImpl();
#endif
}

int watchdogStartupGraceMs(int steadyMs) {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    return static_cast<int>(watchdogStartupGraceMsImpl(steadyMs));
#else
    return steadyMs;
#endif
}

int watchdogReportCountForTesting() {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    return gWdReportCount;
#else
    return 0;
#endif
}

} // namespace CrashHandler
