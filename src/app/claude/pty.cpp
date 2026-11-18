#include "app/claude/pty.h"

#include "base/host.h"
#include "base/process.h"
#include "base/str.h"
#include "base/thread.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include "base/winstr.h"

#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace claude {

#ifdef _WIN32

namespace {

// ConPTY is looked up at run time: older Windows (and older MinGW headers)
// don't have it, and the rest of msga shouldn't need it to start.
using HPCON_                = void *;
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON_ *);
using ClosePseudoConsoleFn  = void(WINAPI *)(HPCON_);
constexpr DWORD_PTR kAttributePseudoConsole = 0x00020016; // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

} // namespace

struct Pty::Impl {
    Pty                 *q;
    ClosePseudoConsoleFn closePc = nullptr;
    HPCON_               pc      = nullptr;
    HANDLE               inWrite = INVALID_HANDLE_VALUE; // our typing → the program
    HANDLE               outRead = INVALID_HANDLE_VALUE; // what it draws → us
    PROCESS_INFORMATION  pi{};
    base::Thread         reader;
    base::Thread         exitWatch;
    bool                 running = false;

    explicit Impl(Pty *owner) : q(owner) {}
    ~Impl() {
        if (running)
            TerminateProcess(pi.hProcess, 1);
        exitWatch.join(); // returns once the process has ended
        // Closing the console flushes the last output: the reader still
        // drains it, then sees the pipe break and ends.
        if (pc && closePc)
            closePc(pc);
        if (inWrite != INVALID_HANDLE_VALUE)
            CloseHandle(inWrite);
        reader.join();
        if (outRead != INVALID_HANDLE_VALUE)
            CloseHandle(outRead);
        if (pi.hProcess)
            CloseHandle(pi.hProcess);
        if (pi.hThread)
            CloseHandle(pi.hThread);
    }
};

Pty::Pty(plat::App &app)
    : _app(app), d(std::make_unique<Impl>(this)), _alive(std::make_shared<bool>(true)) {}

Pty::~Pty() {
    *_alive = false;
}

bool Pty::start(
    const std::string              &program,
    const std::vector<std::string> &args,
    const std::string              &cwd,
    int                             rows,
    int                             cols
) {
    HMODULE k32    = GetModuleHandleW(L"kernel32.dll");
    auto    create = reinterpret_cast<CreatePseudoConsoleFn>(
        reinterpret_cast<void *>(GetProcAddress(k32, "CreatePseudoConsole"))
    );
    d->closePc = reinterpret_cast<ClosePseudoConsoleFn>(
        reinterpret_cast<void *>(GetProcAddress(k32, "ClosePseudoConsole"))
    );
    if (!create || !d->closePc) {
        _error = "this Windows has no pseudo console (ConPTY)";
        return false;
    }
    HANDLE inRead = INVALID_HANDLE_VALUE, outWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&inRead, &d->inWrite, nullptr, 0) ||
        !CreatePipe(&d->outRead, &outWrite, nullptr, 0)) {
        _error = "CreatePipe failed";
        return false;
    }
    const COORD   size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    const HRESULT hr = create(size, inRead, outWrite, 0, &d->pc);
    CloseHandle(inRead); // the console holds its own
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        d->pc  = nullptr;
        _error = "CreatePseudoConsole failed";
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
        _error = "UpdateProcThreadAttribute failed";
        return false;
    }
    std::wstring cmd;
    base::appendQuoted(cmd, base::widePath(program));
    for (const std::string &a : args)
        base::appendQuoted(cmd, base::wide(a));
    // Our environment, with TERM for the program.
    std::wstring       envBlock = base::envBlock({"TERM=xterm-256color"});
    const std::wstring dir      = base::widePath(cwd);
    const BOOL         ok       = CreateProcessW(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(),
        cwd.empty() ? nullptr : dir.c_str(),
        &si.StartupInfo,
        &d->pi
    );
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        _error = "CreateProcess failed (" + str::number(GetLastError()) + ")";
        return false;
    }
    d->running = true;

    plat::App &app   = _app;
    auto       alive = _alive;
    Impl      *impl  = d.get();
    HANDLE     out   = d->outRead;
    d->reader.start([&app, alive, impl, out] {
        char buf[8192];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(out, buf, sizeof buf, &n, nullptr) || n == 0)
                break;
            app.post([alive, impl, chunk = std::string(buf, n)] {
                if (!*alive || !impl->q->onOutput)
                    return;
                auto cb = impl->q->onOutput; // it may destroy us
                cb(chunk);
            });
        }
    });
    HANDLE proc = d->pi.hProcess;
    d->exitWatch.start([&app, alive, impl, proc] {
        WaitForSingleObject(proc, INFINITE);
        app.post([alive, impl] {
            if (!*alive || !impl->running)
                return;
            impl->running = false;
            if (auto cb = impl->q->onFinished)
                cb();
        });
    });
    return true;
}

void Pty::write(std::string_view bytes) {
    if (!d->running)
        return;
    DWORD n = 0;
    WriteFile(d->inWrite, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr);
}

void Pty::terminate() {
    if (d->running)
        TerminateProcess(d->pi.hProcess, 1); // onFinished follows from the exit watch
}

bool Pty::isRunning() const {
    return d->running;
}

#else // POSIX

struct Pty::Impl {
    Pty          *q;
    int           master    = -1;
    pid_t         pid       = -1;
    bool          running   = false;
    uint64_t      watch     = 0; // the loop's fd watch on `master`
    plat::TimerId poll      = 0; // …or, on a loop without fd watches (headless), a poll
    plat::TimerId killTimer = 0; // terminate()'s SIGKILL

    explicit Impl(Pty *owner) : q(owner) {}

    void reap() {
        if (pid <= 0 || !running)
            return;
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            running = false;
    }

    void stopWatching() {
        if (watch)
            q->_app.unwatchFd(watch);
        if (poll)
            q->_app.cancelTimer(poll);
        watch = poll = 0;
    }

    void readable() {
        auto alive = q->_alive;
        char buf[8192];
        for (;;) {
            const ssize_t n = ::read(master, buf, sizeof buf);
            if (n > 0) {
                if (q->onOutput) {
                    auto cb = q->onOutput; // it may destroy us
                    cb(std::string_view(buf, size_t(n)));
                    if (!*alive)
                        return;
                }
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && errno == EAGAIN)
                return;
            // 0 or EIO: the program's side of the terminal is closed — it exited.
            stopWatching();
            reap();
            if (running) {
                // Closed its terminal yet still running: it goes now.
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
                running = false;
            }
            if (auto cb = q->onFinished)
                cb();
            return;
        }
    }
};

Pty::Pty(plat::App &app)
    : _app(app), d(std::make_unique<Impl>(this)), _alive(std::make_shared<bool>(true)) {}

Pty::~Pty() {
    *_alive = false;
    d->stopWatching();
    if (d->killTimer)
        _app.cancelTimer(d->killTimer);
    if (d->master >= 0)
        ::close(d->master);
    if (d->pid > 0 && d->running) {
        ::kill(d->pid, SIGKILL);
        ::waitpid(d->pid, nullptr, 0);
    }
}

bool Pty::start(
    const std::string              &program,
    const std::vector<std::string> &args,
    const std::string              &cwd,
    int                             rows,
    int                             cols
) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        _error = std::string("no pseudo-terminal: ") + std::strerror(errno);
        if (master >= 0)
            ::close(master);
        return false;
    }
    const char *slaveName = ::ptsname(master);
    if (!slaveName) {
        _error = "no pseudo-terminal name";
        ::close(master);
        return false;
    }
    // Everything the child needs is made before fork(): after it, only
    // async-signal-safe calls until exec.
    const std::string        slave  = slaveName;
    // In a Flatpak sandbox the program runs on the host (base/host.h), and
    // the terminal must not be ours: the host process makes it its
    // controlling one (a terminal belongs to one session), so it gets
    // /dev/tty and the resize signals. Measured 2026-11-18 on Flatpak 1.16.
    const bool               onHost = base::host::sandboxed();
    std::vector<std::string> argStore;
    std::vector<std::string> envStore;
    if (onHost) {
        base::host::Command c = base::host::wrap(program, args, cwd, {"TERM=xterm-256color"}, -1);
        argStore.push_back(std::move(c.exe));
        argStore.insert(argStore.end(), c.args.begin(), c.args.end());
        envStore = base::mergedEnv({});
    } else {
        argStore.push_back(program);
        argStore.insert(argStore.end(), args.begin(), args.end());
        envStore = base::mergedEnv({"TERM=xterm-256color"});
    }
    std::vector<char *> argv;
    for (auto &a : argStore)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<char *> envp;
    for (auto &e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    struct winsize ws{};
    ws.ws_row        = static_cast<unsigned short>(rows);
    ws.ws_col        = static_cast<unsigned short>(cols);
    const long maxFd = std::min(::sysconf(_SC_OPEN_MAX), 4096L);
    sigset_t   none;
    sigemptyset(&none);

    const pid_t pid = ::fork();
    if (pid < 0) {
        _error = std::string("fork failed: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    if (pid == 0) {
        // A clean signal state: the app's ignored signals (SIGPIPE) and its
        // threads' masks are not the program's business.
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        for (int sig = 1; sig < 32; ++sig)
            ::sigaction(sig, &dfl, nullptr);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::setsid(); // a session of its own, the terminal its controlling one
        const int s = ::open(slave.c_str(), onHost ? O_RDWR | O_NOCTTY : O_RDWR);
        if (s < 0)
            ::_exit(127);
#ifdef TIOCSCTTY
        if (!onHost)
            ::ioctl(s, TIOCSCTTY, 0);
#endif
        ::ioctl(s, TIOCSWINSZ, &ws);
        ::dup2(s, 0);
        ::dup2(s, 1);
        ::dup2(s, 2);
        for (long fd = 3; fd < maxFd; ++fd)
            ::close(static_cast<int>(fd));
        if (!onHost && !cwd.empty() && ::chdir(cwd.c_str()) != 0)
            ::_exit(127);
        ::execve(argStore[0].c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    d->master  = master;
    d->pid     = pid;
    d->running = true;
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    Impl *impl = d.get();
    d->watch   = _app.watchFd(master, plat::FdRead, [impl](uint32_t) { impl->readable(); });
    if (!d->watch) // a loop without fd watches: looked at often instead
        d->poll = _app.addTimer(20, true, [impl] { impl->readable(); });
    return true;
}

void Pty::write(std::string_view bytes) {
    if (!d->running || d->master < 0)
        return;
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(d->master, bytes.data() + done, bytes.size() - done);
        if (n > 0) {
            done += size_t(n);
        } else if (n < 0 && errno == EAGAIN) {
            ::usleep(1000); // the terminal's buffer is full: it drains in a moment
        } else if (n < 0 && errno != EINTR) {
            return;
        }
    }
}

void Pty::terminate() {
    if (!d->running)
        return;
    ::kill(d->pid, SIGTERM);
    // What ignores it goes anyway.
    const pid_t pid  = d->pid;
    Impl       *impl = d.get();
    if (d->killTimer)
        _app.cancelTimer(d->killTimer);
    d->killTimer = _app.addTimer(2000, false, [impl, pid] {
        impl->killTimer = 0;
        if (impl->running && impl->pid == pid)
            ::kill(pid, SIGKILL);
    });
}

bool Pty::isRunning() const {
    return d->running;
}

#endif

} // namespace claude
