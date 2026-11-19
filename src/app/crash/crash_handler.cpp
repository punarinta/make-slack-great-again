// The hang watchdog needs SIGEV_THREAD_ID and the sigev_notify_thread_id member
// macro (glibc gates both behind _GNU_SOURCE; musl exposes them under it too) to
// target the timer signal at the main thread; the ucontext register names need
// it as well. Must precede every system header.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "app/crash/crash_handler.h"

#if defined(MSGA_PORT)

// A port build: no signals, no stack walking, no per-thread timers — a port
// OS keeps its own crash records (PEOS: the journal and the supervisor's
// restarts). Neither crash reports nor the hang watchdog.
namespace crash {

void install(const std::string &) {}
bool startWatchdog(int) {
    return false;
}
void heartbeat() {}
int  watchdogStartupGraceMs(int steadyMs) {
    return steadyMs;
}
int watchdogReportCountForTesting() {
    return 0;
}

} // namespace crash

#else

#include "base/file.h"
#include "base/str.h"
#include "plat/plat.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#ifndef MSGA_VERSION
#define MSGA_VERSION 0
#endif
#ifndef MSGA_BUILD_TIMESTAMP
#define MSGA_BUILD_TIMESTAMP ""
#endif

#if defined(_WIN32)
#include <windows.h>
// windows.h must precede dbghelp.h
#include <dbghelp.h>

#include <csignal>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#if defined(__ELF__)
#include <sys/mman.h> // mincore
#ifndef __OpenBSD__   // OpenBSD's ucontext_t is struct sigcontext, in <csignal>
#include <ucontext.h> // macOS: ucontext_t comes with <csignal>
#endif
#endif
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
#include <sys/syscall.h> // SYS_gettid
#include <time.h>        // timer_create / timer_settime (POSIX per-process timers)
#endif
// backtrace()/backtrace_symbols_fd() exist on glibc and macOS but not musl
// (the static Linux release builds). There the libgcc unwinder walks the
// stack instead and frames print as raw addresses — resolvable offline with
// addr2line against the executable base in the header.
#if defined(__GLIBC__) || defined(__APPLE__)
#define MSGA_HAVE_EXECINFO 1
#include <execinfo.h>
#else
#include <unwind.h>
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#endif
#endif

#if defined(__ELF__)
// The executable's ELF header (= its load address) and the end of its code,
// from the linker (GNU ld and lld both provide them; Linux and the BSDs).
extern "C" char __ehdr_start[] __attribute__((weak));
extern "C" char etext[] __attribute__((weak));
#endif

namespace {

constexpr int kMaxFrames = 64;

// Everything the handlers need is precomputed at install time: a crash
// handler must not allocate.
char gHeader[512];
char gLogNote[1200];

#if !defined(_WIN32)

char gLogPath[1024];

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};

// Only async-signal-safe calls below: write/open/close/backtrace*.
void writeAll(int fd, const char *s, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= size_t(w);
    }
}

void writeStr(int fd, const char *s) {
    writeAll(fd, s, ::strlen(s));
}

// A pointer's 2 * sizeof(void *) hex digits into out (not terminated).
void hexDigits(char *out, const void *p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    for (int i = int(2 * sizeof(void *)) - 1; i >= 0; --i, v >>= 4)
        out[i] = str::kHexLower[v & 0xf];
}

void writePtr(int fd, const void *p) {
    char buf[2 + sizeof(void *) * 2] = {'0', 'x'};
    hexDigits(buf + 2, p);
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

// The interrupted code's program counter and stack pointer, where the
// platform tells (null otherwise).
void contextRegs(void *uc, const void **pc, const uintptr_t **sp) {
    *pc = nullptr;
    *sp = nullptr;
    if (!uc)
        return;
    const auto *c = static_cast<const ucontext_t *>(uc);
#if defined(__linux__) && defined(__x86_64__)
    *pc = reinterpret_cast<const void *>(c->uc_mcontext.gregs[REG_RIP]);
    *sp = reinterpret_cast<const uintptr_t *>(c->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
    *pc = reinterpret_cast<const void *>(c->uc_mcontext.pc);
    *sp = reinterpret_cast<const uintptr_t *>(c->uc_mcontext.sp);
#elif defined(__FreeBSD__) && defined(__x86_64__)
    *pc = reinterpret_cast<const void *>(c->uc_mcontext.mc_rip);
    *sp = reinterpret_cast<const uintptr_t *>(c->uc_mcontext.mc_rsp);
#elif defined(__FreeBSD__) && defined(__aarch64__)
    *pc = reinterpret_cast<const void *>(c->uc_mcontext.mc_gpregs.gp_elr);
    *sp = reinterpret_cast<const uintptr_t *>(c->uc_mcontext.mc_gpregs.gp_sp);
#elif defined(__OpenBSD__) && defined(__x86_64__)
    *pc = reinterpret_cast<const void *>(c->sc_rip);
    *sp = reinterpret_cast<const uintptr_t *>(c->sc_rsp);
#elif defined(__APPLE__) && defined(__aarch64__)
    *pc = reinterpret_cast<const void *>(c->uc_mcontext->__ss.__pc);
    *sp = reinterpret_cast<const uintptr_t *>(c->uc_mcontext->__ss.__sp);
#elif defined(__APPLE__) && defined(__x86_64__)
    *pc = reinterpret_cast<const void *>(c->uc_mcontext->__ss.__rip);
    *sp = reinterpret_cast<const uintptr_t *>(c->uc_mcontext->__ss.__rsp);
#else
    (void)c;
#endif
}

// Release builds carry no unwind tables (-fno-asynchronous-unwind-tables, see
// CMakeLists.txt), so the unwinder stops at the signal frame. What still
// tells where the crash came from: the faulting pc, plus every word on the
// stack that points into our code — a superset of the real return addresses
// (stale ones included), resolvable with addr2line like the frames.
void writeStackScan(int fd, const uintptr_t *sp) {
#if defined(__ELF__) && !defined(__OpenBSD__) // OpenBSD has no mincore()
    if (!sp || !__ehdr_start || !etext)
        return;
    const auto lo = reinterpret_cast<uintptr_t>(__ehdr_start);
    const auto hi = reinterpret_cast<uintptr_t>(etext);
    writeStr(fd, "stack scan (addresses into the executable, innermost first):\n");
    int             found  = 0;
    // 16 KiB of stack at most, and never past its end: a crash near the top of
    // a stack has less than that above it, and reading on would fault inside
    // the handler. mincore() (a plain syscall) fails on an unmapped page.
    const uintptr_t page   = uintptr_t(::sysconf(_SC_PAGESIZE));
    uintptr_t       mapped = 0; // below this address is known to be mapped
#if defined(__linux__)
    unsigned char resident;
#else
    char resident; // the BSDs' mincore() takes char *
#endif
    for (int i = 0; i < 2048 && found < kMaxFrames; ++i) {
        const auto addr = reinterpret_cast<uintptr_t>(sp + i);
        if (addr + sizeof(uintptr_t) > mapped) {
            const uintptr_t base = addr & ~(page - 1);
            if (::mincore(reinterpret_cast<void *>(base), page, &resident) != 0)
                break;
            mapped = base + page;
        }
        const uintptr_t v = sp[i];
        if (v > lo && v < hi) {
            writePtr(fd, reinterpret_cast<const void *>(v));
            writeAll(fd, "\n", 1);
            ++found;
        }
    }
#else
    (void)fd;
    (void)sp;
#endif
}

void writeReport(
    int              fd,
    const char      *what,
    const void      *pc,
    const uintptr_t *sp,
    void *const     *frames,
    int              depth,
    const char      *end
) {
    writeStr(fd, gHeader);
    writeStr(fd, what);
    if (pc) {
        writeStr(fd, "pc: ");
        writePtr(fd, pc);
        writeAll(fd, "\n", 1);
    }
    writeStr(fd, "stack:\n");
    writeFrames(fd, frames, depth);
    if (depth < 4) // the unwinder stopped at the signal frame: no unwind tables
        writeStackScan(fd, sp);
    writeStr(fd, end);
}

void fatalSignal(int sig, siginfo_t *info, void *uc) {
    for (const int s : kSignals)
        ::signal(s, SIG_DFL); // a fault inside the handler takes the default path

    void            *frames[kMaxFrames];
    const int        depth = captureFrames(frames, kMaxFrames);
    const void      *pc;
    const uintptr_t *sp;
    contextRegs(uc, &pc, &sp);

    // "signal: SIGSEGV, fault address: 0x…\n"
    char what[96] = "signal: ";
    std::strcat(what, signalName(sig));
    if (info && (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL)) {
        std::strcat(what, ", fault address: 0x");
        char hex[2 * sizeof(void *) + 1] = {};
        hexDigits(hex, info->si_addr);
        std::strcat(what, hex);
    }
    std::strcat(what, "\n");

    const int logFd = ::open(gLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    const int fds[] = {STDERR_FILENO, logFd};
    for (const int fd : fds)
        if (fd >= 0)
            writeReport(fd, what, pc, sp, frames, depth, "==== end of crash report ====\n");
    if (logFd >= 0) {
        ::close(logFd);
        writeStr(STDERR_FILENO, gLogNote);
    }
    ::raise(sig); // default action now: terminate + core dump, as before
}

#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
// ── Main-thread hang watchdog ───────────────────────────────────────────────
// A POSIX per-process timer, re-armed to a future deadline by the main thread on
// every heartbeat(). While the event loop keeps pumping, the deadline is pushed
// out before it can elapse, so the timer never fires — no idle wakeups, no
// signals on the healthy path. If the main thread wedges (infinite loop or
// deadlock) and stops calling heartbeat(), the deadline elapses and the kernel
// delivers the timer signal *to the main thread itself* (SIGEV_THREAD_ID): the
// handler then runs on the stuck stack, so the backtrace points straight at
// the hang. Same async-signal-safe discipline and capture path as the crash
// handler above. Dev builds only (MSGA_HANG_WATCHDOG, src/app/CMakeLists.txt).

timer_t               gWdTimer;
bool                  gWdArmed       = false;
bool                  gWdAbort       = false;
long                  gWdTimeoutMs   = 5000; // steady-state window (event loop alive)
volatile sig_atomic_t gWdReported    = 0;    // at most one report per stall episode
volatile sig_atomic_t gWdReportCount = 0;    // total reports this process (tests)

// Startup gets a much wider window than a steady-state stall: building the
// whole UI is one long synchronous burst on the main thread with no event-loop
// turns to heartbeat through, and under AddressSanitizer it can take far
// longer than any later stall window. The first heartbeat() (proof the loop
// is pumping) drops back to the steady window, so a true forever-hang during
// startup is still caught, just with a roomier deadline.
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

void watchdogExpired(int, siginfo_t *, void *uc) {
    if (gWdReported)
        return; // already dumped this stall; heartbeat() clears it on recovery
    gWdReported    = 1;
    gWdReportCount = gWdReportCount + 1;

    void            *frames[kMaxFrames];
    const int        depth = captureFrames(frames, kMaxFrames);
    const void      *pc;
    const uintptr_t *sp;
    contextRegs(uc, &pc, &sp);

    const int logFd = ::open(gLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    const int fds[] = {STDERR_FILENO, logFd};
    for (const int fd : fds)
        if (fd >= 0)
            writeReport(
                fd,
                "main thread unresponsive (hang watchdog)\n",
                pc,
                sp,
                frames,
                depth,
                "==== end of hang report ====\n"
            );
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

bool envIsOne(const char *name) {
    const char *v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

bool startWatchdogImpl(int timeoutMs) {
    if (envIsOne("MSGA_WATCHDOG_DISABLE"))
        return false; // e.g. under a debugger, where a breakpoint freezes main for minutes
    gWdTimeoutMs = timeoutMs > 0 ? timeoutMs : 5000;
    gWdAbort     = envIsOne("MSGA_WATCHDOG_ABORT");

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
        return false;

    struct sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID; // deliver to one specific thread (the main one)
    sev.sigev_signo  = sig;
    // The LWP-id field has no portable accessor: musl exposes the POSIX-style
    // `sigev_notify_thread_id` macro, while glibc only names the internal union
    // member `_sigev_un._tid`. Pick whichever the libc provides.
#if defined(sigev_notify_thread_id)
    sev.sigev_notify_thread_id = pid_t(::syscall(SYS_gettid));
#else
    sev._sigev_un._tid = pid_t(::syscall(SYS_gettid));
#endif
    if (::timer_create(CLOCK_MONOTONIC, &sev, &gWdTimer) != 0)
        return false;

    gWdArmed = true;
    // The first window covers startup (before any heartbeat) and is wider;
    // the first heartbeat() re-arms with the steady window.
    wdArm(watchdogStartupGraceMsImpl(gWdTimeoutMs));
    return true;
}

void heartbeatImpl() {
    if (!gWdArmed)
        return;
    gWdReported = 0;     // main thread is alive → ready to report the next stall
    wdArm(gWdTimeoutMs); // push the deadline out
}
#endif // MSGA_HANG_WATCHDOG && __linux__

#else // _WIN32

wchar_t gLogPathW[1024];

void emitStr(HANDLE file, const char *s) {
    const DWORD n       = DWORD(::strlen(s));
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
                line, sizeof(line), "%p %s+0x%llx\n", frames[i], sym->Name, (unsigned long long)disp
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
        what, sizeof(what), "exception: 0x%08lx", (unsigned long)ex->ExceptionRecord->ExceptionCode
    );
    dumpTrace(what, ex->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH; // WER / an attached debugger still runs
}

// assert()/std::terminate end in abort(), which raises SIGABRT through the
// CRT instead of an SEH exception.
void abortHandler(int) {
    dumpTrace("abort()", nullptr);
    ::signal(SIGABRT, SIG_DFL);
    ::raise(SIGABRT);
}

#endif

} // namespace

namespace crash {

void install(const std::string &logPath) {
    if (!logPath.empty())
        file::makeDirs(file::dirName(logPath));

    const void *base = nullptr;
#if defined(_WIN32)
    const int n = MultiByteToWideChar(
        CP_UTF8, 0, logPath.c_str(), int(logPath.size()), gLogPathW, int(std::size(gLogPathW)) - 1
    );
    gLogPathW[n > 0 ? n : 0] = 0;
    for (wchar_t *p = gLogPathW; *p; ++p)
        if (*p == L'/')
            *p = L'\\';

    base = GetModuleHandleW(nullptr);
    SetUnhandledExceptionFilter(sehFilter);
    ::signal(SIGABRT, abortHandler);
#else
    std::snprintf(gLogPath, sizeof(gLogPath), "%s", logPath.c_str());

#if defined(__ELF__)
    base = __ehdr_start;
#elif defined(__APPLE__)
    Dl_info di{};
    if (::dladdr(reinterpret_cast<void *>(&install), &di) != 0)
        base = di.dli_fbase;
#endif

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
        MSGA_VERSION,
        MSGA_BUILD_TIMESTAMP,
        base
    );
    std::snprintf(gLogNote, sizeof(gLogNote), "crash report appended to %s\n", logPath.c_str());
}

bool startWatchdog(int timeoutMs) {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    return startWatchdogImpl(timeoutMs);
#else
    (void)timeoutMs;
    return false;
#endif
}

void heartbeat() {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    heartbeatImpl();
#endif
}

int watchdogStartupGraceMs(int steadyMs) {
#if defined(MSGA_HANG_WATCHDOG) && defined(__linux__)
    return int(watchdogStartupGraceMsImpl(steadyMs));
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

} // namespace crash

#endif // MSGA_PORT
