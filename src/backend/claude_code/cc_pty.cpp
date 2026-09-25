// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_pty.h"

#include <QDir>
#include <QFile>
#include <QTimer>
#include <algorithm>
#include <vector>

#if defined(Q_OS_WIN)
#include <QProcessEnvironment>
#include <QWinEventNotifier>
#include <thread>
#include <windows.h>
#else
#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
extern char **environ;
#endif

namespace claude_code {

#if defined(Q_OS_WIN)

namespace {
// ConPTY is looked up at run time: older Windows (and older MinGW headers)
// don't have it, and the rest of msga shouldn't need it to start.
using HPCON_                = void *;
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON_ *);
using ClosePseudoConsoleFn  = void(WINAPI *)(HPCON_);
constexpr DWORD_PTR kAttributePseudoConsole = 0x00020016; // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

QString quoteArg(const QString &a) {
    // CommandLineToArgvW rules: quotes around anything with blanks or quotes,
    // backslashes doubled only before a quote.
    const bool plain = std::none_of(a.begin(), a.end(), [](QChar c) {
        return c.isSpace() || c == QLatin1Char('"');
    });
    if (!a.isEmpty() && plain)
        return a;
    QString out = QStringLiteral("\"");
    int     bs  = 0;
    for (const QChar c : a) {
        if (c == QLatin1Char('\\')) {
            ++bs;
            continue;
        }
        if (c == QLatin1Char('"')) {
            out += QString(bs * 2 + 1, QLatin1Char('\\'));
        } else {
            out += QString(bs, QLatin1Char('\\'));
        }
        bs = 0;
        out += c;
    }
    out += QString(bs * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}
} // namespace

struct PtyProcess::Impl {
    PtyProcess          *q;
    ClosePseudoConsoleFn closePc = nullptr;
    HPCON_               pc      = nullptr;
    HANDLE               inWrite = INVALID_HANDLE_VALUE; // our typing → the program
    HANDLE               outRead = INVALID_HANDLE_VALUE; // what it draws → us
    PROCESS_INFORMATION  pi{};
    QWinEventNotifier   *exitWatch = nullptr;
    std::thread          reader;
    bool                 running = false;

    explicit Impl(PtyProcess *owner) : q(owner) {}
    ~Impl() {
        if (running)
            TerminateProcess(pi.hProcess, 1);
        delete exitWatch;
        // Closing the console flushes the last output: the reader still
        // drains it, then sees the pipe break and ends.
        if (pc && closePc)
            closePc(pc);
        if (inWrite != INVALID_HANDLE_VALUE)
            CloseHandle(inWrite);
        if (reader.joinable())
            reader.join();
        if (outRead != INVALID_HANDLE_VALUE)
            CloseHandle(outRead);
        if (pi.hProcess)
            CloseHandle(pi.hProcess);
        if (pi.hThread)
            CloseHandle(pi.hThread);
    }
};

PtyProcess::PtyProcess(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}

PtyProcess::~PtyProcess() = default;

bool PtyProcess::start(
    const QString &program, const QStringList &args, const QString &cwd, int rows, int cols
) {
    HMODULE k32    = GetModuleHandleW(L"kernel32.dll");
    auto    create = reinterpret_cast<CreatePseudoConsoleFn>(
        reinterpret_cast<void *>(GetProcAddress(k32, "CreatePseudoConsole"))
    );
    d->closePc = reinterpret_cast<ClosePseudoConsoleFn>(
        reinterpret_cast<void *>(GetProcAddress(k32, "ClosePseudoConsole"))
    );
    if (!create || !d->closePc) {
        _error = QStringLiteral("this Windows has no pseudo console (ConPTY)");
        return false;
    }
    HANDLE inRead = INVALID_HANDLE_VALUE, outWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&inRead, &d->inWrite, nullptr, 0) ||
        !CreatePipe(&d->outRead, &outWrite, nullptr, 0)) {
        _error = QStringLiteral("CreatePipe failed");
        return false;
    }
    const COORD   size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    const HRESULT hr = create(size, inRead, outWrite, 0, &d->pc);
    CloseHandle(inRead); // the console holds its own
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        d->pc  = nullptr;
        _error = QStringLiteral("CreatePseudoConsole failed");
        return false;
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    STARTUPINFOEXW    si{};
    si.StartupInfo.cb         = sizeof(si);
    // No standard handles of ours: without this a child inherits them when
    // they're redirected, and writes there instead of to the pseudo console.
    // Invalid ones (not null: that reads as an empty input) make it use the
    // pseudo console's.
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdError  = INVALID_HANDLE_VALUE;
    si.lpAttributeList        = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(
            si.lpAttributeList, 0, kAttributePseudoConsole, d->pc, sizeof(d->pc), nullptr, nullptr
        )) {
        _error = QStringLiteral("UpdateProcThreadAttribute failed");
        return false;
    }
    QString line = quoteArg(QDir::toNativeSeparators(program));
    for (const QString &a : args)
        line += QLatin1Char(' ') + quoteArg(a);
    std::wstring cmd = line.toStdWString();
    // Our environment, with TERM for the program: a block of "K=V\0" strings.
    auto         env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    std::wstring envBlock;
    for (const QString &kv : env.toStringList())
        envBlock += kv.toStdWString() + L'\0';
    envBlock += L'\0';
    const std::wstring dir = QDir::toNativeSeparators(cwd).toStdWString();
    const BOOL         ok  = CreateProcessW(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(),
        cwd.isEmpty() ? nullptr : dir.c_str(),
        &si.StartupInfo,
        &d->pi
    );
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        _error = QStringLiteral("CreateProcess failed (%1)").arg(GetLastError());
        return false;
    }
    d->running = true;

    HANDLE out   = d->outRead;
    d->reader    = std::thread([this, out] {
        char buf[8192];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(out, buf, sizeof buf, &n, nullptr) || n == 0)
                break;
            QByteArray chunk(buf, static_cast<int>(n));
            QMetaObject::invokeMethod(
                this, [this, chunk] { emit output(chunk); }, Qt::QueuedConnection
            );
        }
    });
    d->exitWatch = new QWinEventNotifier(d->pi.hProcess);
    connect(d->exitWatch, &QWinEventNotifier::activated, this, [this] {
        d->exitWatch->setEnabled(false);
        d->running = false;
        emit finished();
    });
    return true;
}

void PtyProcess::write(const QByteArray &bytes) {
    if (!d->running)
        return;
    DWORD n = 0;
    WriteFile(d->inWrite, bytes.constData(), static_cast<DWORD>(bytes.size()), &n, nullptr);
}

void PtyProcess::terminate() {
    if (d->running)
        TerminateProcess(d->pi.hProcess, 1); // finished() follows from the exit watch
}

bool PtyProcess::isRunning() const {
    return d->running;
}

#else // POSIX

struct PtyProcess::Impl {
    PtyProcess      *q;
    int              master   = -1;
    pid_t            pid      = -1;
    bool             running  = false;
    QSocketNotifier *notifier = nullptr;

    explicit Impl(PtyProcess *owner) : q(owner) {}
    ~Impl() {
        delete notifier;
        if (master >= 0)
            ::close(master);
        if (pid > 0) {
            if (running)
                ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, running ? 0 : WNOHANG);
        }
    }
    void reap() {
        if (pid <= 0 || !running)
            return;
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            running = false;
    }
};

PtyProcess::PtyProcess(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}

PtyProcess::~PtyProcess() = default;

bool PtyProcess::start(
    const QString &program, const QStringList &args, const QString &cwd, int rows, int cols
) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        _error = QStringLiteral("no pseudo-terminal: %1").arg(qt_error_string(errno));
        if (master >= 0)
            ::close(master);
        return false;
    }
    const char *slaveName = ::ptsname(master);
    if (!slaveName) {
        _error = QStringLiteral("no pseudo-terminal name");
        ::close(master);
        return false;
    }
    // Everything the child needs is made before fork(): after it, only
    // async-signal-safe calls until exec.
    const QByteArray        slave = slaveName;
    const QByteArray        prog  = QFile::encodeName(program);
    const QByteArray        dir   = QFile::encodeName(cwd);
    std::vector<QByteArray> argStore{prog};
    for (const QString &a : args)
        argStore.push_back(a.toUtf8());
    std::vector<char *> argv;
    for (auto &a : argStore)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<QByteArray> envStore;
    for (char **e = environ; *e; ++e)
        if (qstrncmp(*e, "TERM=", 5) != 0)
            envStore.emplace_back(*e);
    envStore.emplace_back("TERM=xterm-256color");
    std::vector<char *> envp;
    for (auto &e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    struct winsize ws{};
    ws.ws_row        = static_cast<unsigned short>(rows);
    ws.ws_col        = static_cast<unsigned short>(cols);
    const long maxFd = std::min(::sysconf(_SC_OPEN_MAX), 4096L);

    const pid_t pid = ::fork();
    if (pid < 0) {
        _error = QStringLiteral("fork failed: %1").arg(qt_error_string(errno));
        ::close(master);
        return false;
    }
    if (pid == 0) {
        ::setsid(); // a session of its own, the terminal its controlling one
        const int s = ::open(slave.constData(), O_RDWR);
        if (s < 0)
            ::_exit(127);
#ifdef TIOCSCTTY
        ::ioctl(s, TIOCSCTTY, 0);
#endif
        ::ioctl(s, TIOCSWINSZ, &ws);
        ::dup2(s, 0);
        ::dup2(s, 1);
        ::dup2(s, 2);
        for (long fd = 3; fd < maxFd; ++fd)
            ::close(static_cast<int>(fd));
        if (!dir.isEmpty() && ::chdir(dir.constData()) != 0)
            ::_exit(127);
        ::execve(prog.constData(), argv.data(), envp.data());
        ::_exit(127);
    }
    d->master  = master;
    d->pid     = pid;
    d->running = true;
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    d->notifier = new QSocketNotifier(master, QSocketNotifier::Read, this);
    connect(d->notifier, &QSocketNotifier::activated, this, [this] {
        char buf[8192];
        for (;;) {
            const ssize_t n = ::read(d->master, buf, sizeof buf);
            if (n > 0) {
                emit output(QByteArray(buf, static_cast<int>(n)));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            // 0 or EIO: the program's side of the terminal is closed — it exited.
            d->notifier->setEnabled(false);
            d->reap();
            if (d->running) {
                // Closed its terminal yet still running: it goes now.
                ::kill(d->pid, SIGKILL);
                ::waitpid(d->pid, nullptr, 0);
                d->running = false;
            }
            emit finished();
            return;
        }
    });
    return true;
}

void PtyProcess::write(const QByteArray &bytes) {
    if (!d->running || d->master < 0)
        return;
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(d->master, bytes.constData() + done, bytes.size() - done);
        if (n > 0) {
            done += n;
        } else if (n < 0 && errno == EAGAIN) {
            ::usleep(1000); // the terminal's buffer is full: it drains in a moment
        } else if (n < 0 && errno != EINTR) {
            return;
        }
    }
}

void PtyProcess::terminate() {
    if (!d->running)
        return;
    ::kill(d->pid, SIGTERM);
    // What ignores it goes anyway.
    const pid_t pid = d->pid;
    QTimer::singleShot(2000, this, [this, pid] {
        if (d->running && d->pid == pid)
            ::kill(pid, SIGKILL);
    });
}

bool PtyProcess::isRunning() const {
    return d->running;
}

#endif

} // namespace claude_code
