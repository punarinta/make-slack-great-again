// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
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

} // namespace CrashHandler
