#include "base/process.h"

#include "base/file.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include "base/winstr.h"

#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <climits>
#include <cstdio>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

namespace base {

std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v ? v : "";
}

std::string homeDir() {
#ifdef _WIN32
    std::string home = env("USERPROFILE");
    std::replace(home.begin(), home.end(), '\\', '/');
    return home;
#else
    return env("HOME");
#endif
}

namespace {
bool g_testProcess = false;
}

bool testProcess() {
    return g_testProcess || std::getenv("MSGA_TEST_ISOLATED");
}

void markTestProcess() {
    g_testProcess = true;
}

namespace {
bool g_isolatedStorage = false;
}

bool isolatedStorage() {
    return g_isolatedStorage || testProcess();
}

void markIsolatedStorage() {
    g_isolatedStorage = true;
}

std::string findExecutable(std::string_view name) {
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos)
        return file::exists(name) && !file::isDir(name) ? std::string(name) : std::string();
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const std::string path = env("PATH");
    str::Splitter     dirs(path, sep);
    for (std::string_view dir; dirs.next(&dir);) {
        if (dir.empty())
            continue;
        std::string cand = file::join(dir, name);
#ifdef _WIN32
        if (!file::exists(cand))
            cand += ".exe";
#endif
        if (file::exists(cand) && !file::isDir(cand)) {
#ifndef _WIN32
            if (::access(cand.c_str(), X_OK) == 0)
#endif
                return cand;
        }
    }
    return {};
}

Process::~Process() {
#ifdef _WIN32
    if (_handle)
        CloseHandle(static_cast<HANDLE>(_handle));
#endif
}

#ifdef _WIN32

// CommandLineToArgvW's quoting rules, in reverse.
void appendQuoted(std::wstring &cmd, const std::wstring &arg) {
    if (!cmd.empty())
        cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"')
            cmd.append(slashes * 2 + 1, L'\\');
        else
            cmd.append(slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}

namespace {

// exe and its arguments as one command line.
std::wstring commandLine(const std::string &exe, const std::vector<std::string> &args) {
    std::wstring cmd;
    appendQuoted(cmd, wide(exe));
    for (const auto &a : args)
        appendQuoted(cmd, wide(a));
    return cmd;
}
} // namespace

bool Process::start(const std::string &exe, const std::vector<std::string> &args) {
    const std::wstring  wexe = wide(exe);
    std::wstring        cmd  = commandLine(exe, args);
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof si;
    if (!CreateProcessW(
            wexe.c_str(),
            cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        ))
        return false;
    CloseHandle(pi.hThread);
    _handle = pi.hProcess;
    _pid    = pi.dwProcessId;
    _exited = false;
    return true;
}

bool Process::running() {
    if (!_handle || _exited)
        return false;
    if (WaitForSingleObject(static_cast<HANDLE>(_handle), 0) == WAIT_OBJECT_0)
        _exited = true;
    return !_exited;
}

void Process::kill(bool) {
    if (running())
        TerminateProcess(static_cast<HANDLE>(_handle), 1);
}

// ── run() ───────────────────────────────────────────────────────────────────

namespace {

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

struct ReadCtx {
    HANDLE       pipe;
    std::string *out;
};
DWORD WINAPI readAll(void *p) {
    auto *c = static_cast<ReadCtx *>(p);
    char  buf[4096];
    DWORD n = 0;
    while (ReadFile(c->pipe, buf, sizeof buf, &n, nullptr) && n > 0)
        c->out->append(buf, n);
    return 0;
}

struct WriteCtx {
    HANDLE             pipe;
    const std::string *in;
};
DWORD WINAPI writeAll(void *p) {
    auto  *c    = static_cast<WriteCtx *>(p);
    size_t done = 0;
    while (done < c->in->size()) {
        DWORD n = 0;
        if (!WriteFile(c->pipe, c->in->data() + done, DWORD(c->in->size() - done), &n, nullptr) ||
            n == 0)
            break; // the child closed its stdin (or exited)
        done += n;
    }
    CloseHandle(c->pipe); // the end of its input
    return 0;
}

size_t envNameLen(const std::wstring &kv) {
    // A leading '=' belongs to the name ("=C:=C:\dir", a drive's cwd).
    const size_t eq = kv.find(L'=', 1);
    return eq == std::wstring::npos ? kv.size() : eq;
}

bool sameEnvName(const std::wstring &a, const std::wstring &b) {
    const size_t alen = envNameLen(a), blen = envNameLen(b);
    return alen == blen &&
           CompareStringOrdinal(a.c_str(), int(alen), b.c_str(), int(blen), TRUE) == CSTR_EQUAL;
}

void closeHandle(HANDLE &h) {
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    h = nullptr;
}

// Waits for a pipe thread; one still blocked in ReadFile/WriteFile (a pipe a
// grandchild holds open, a child that never reads) is cancelled first.
void joinPipeThread(HANDLE &thread, DWORD graceMs) {
    if (!thread)
        return;
    if (WaitForSingleObject(thread, graceMs) == WAIT_TIMEOUT) {
        CancelSynchronousIo(thread);
        WaitForSingleObject(thread, INFINITE);
    }
    closeHandle(thread);
}

} // namespace

// Our environment with `overrides` applied ("NAME=value" sets, "NAME=" unsets;
// names compare case-insensitively, as Windows does): a block of "K=V\0"
// strings ending in an empty one.
std::wstring envBlock(const std::vector<std::string> &overrides) {
    std::vector<std::wstring> sets;
    for (const auto &o : overrides)
        sets.push_back(wide(o));
    std::wstring block;
    if (wchar_t *env = GetEnvironmentStringsW()) {
        for (const wchar_t *e = env; *e; e += wcslen(e) + 1) {
            const std::wstring kv(e);
            bool               overridden = false;
            for (const auto &o : sets)
                overridden = overridden || sameEnvName(kv, o);
            if (!overridden)
                block.append(kv).push_back(L'\0');
        }
        FreeEnvironmentStringsW(env);
    }
    for (size_t i = 0; i < sets.size(); ++i) {
        const size_t n = envNameLen(sets[i]);
        if (n == 0 || n + 1 >= sets[i].size())
            continue;       // no name, or "NAME=": unset
        bool later = false; // the last word on a name wins
        for (size_t j = i + 1; j < sets.size(); ++j)
            later = later || sameEnvName(sets[i], sets[j]);
        if (!later)
            block.append(sets[i]).push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

RunResult run(const std::string &exe, const std::vector<std::string> &args, const RunOptions &o) {
    RunResult           r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE              outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    HANDLE              errNul  = nullptr; // stderr's null device when it isn't merged
    const bool          feed    = !o.input.empty();
    const DWORD         share   = FILE_SHARE_READ | FILE_SHARE_WRITE;
    auto                cleanup = [&] {
        closeHandle(outRead);
        closeHandle(outWrite);
        closeHandle(inRead);
        closeHandle(inWrite);
        closeHandle(errNul);
    };
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        r.output = "CreatePipe failed";
        return r;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    if (feed) {
        if (CreatePipe(&inRead, &inWrite, &sa, 0))
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    } else {
        // Never waits on a prompt.
        inRead = CreateFileW(L"NUL", GENERIC_READ, share, &sa, OPEN_EXISTING, 0, nullptr);
    }
    if (!o.mergeStderr)
        errNul = CreateFileW(L"NUL", GENERIC_WRITE, share, &sa, OPEN_EXISTING, 0, nullptr);
    const HANDLE errWrite = o.mergeStderr ? outWrite : errNul;
    if (!inRead || inRead == INVALID_HANDLE_VALUE || !errWrite ||
        errWrite == INVALID_HANDLE_VALUE) {
        r.output = "no standard handles for the child";
        cleanup();
        return r;
    }

    // Only the three standard handles are inherited, not every inheritable
    // handle the app happens to have open.
    HANDLE inherit[3];
    DWORD  nInherit     = 0;
    inherit[nInherit++] = inRead;
    inherit[nInherit++] = outWrite;
    if (errWrite != outWrite)
        inherit[nInherit++] = errWrite;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    STARTUPINFOEXW    si{};
    si.StartupInfo.cb         = sizeof si;
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = inRead;
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError  = errWrite;
    si.lpAttributeList        = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize)) {
        r.output = "InitializeProcThreadAttributeList failed";
        cleanup();
        return r;
    }
    if (!UpdateProcThreadAttribute(
            si.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherit,
            nInherit * sizeof(HANDLE),
            nullptr,
            nullptr
        )) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        r.output = "UpdateProcThreadAttribute failed";
        cleanup();
        return r;
    }

    std::wstring        cmd = commandLine(exe, args);
    std::wstring        env = o.env.empty() ? std::wstring() : envBlock(o.env);
    const std::wstring  dir = wide(o.cwd);
    PROCESS_INFORMATION pi{};
    // Suspended until it is in the job: nothing it starts can slip past the
    // timeout's kill by being quick.
    const BOOL          ok = CreateProcessW(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
            EXTENDED_STARTUPINFO_PRESENT,
        env.empty() ? nullptr : env.data(),
        o.cwd.empty() ? nullptr : dir.c_str(),
        &si.StartupInfo,
        &pi
    );
    const DWORD startError = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    // The child holds its own copies now: ours must go, or the output pipe
    // never reports its end.
    closeHandle(outWrite);
    closeHandle(inRead);
    closeHandle(errNul);
    if (!ok) {
        r.output = "CreateProcess failed (error " + str::number(startError) + ")";
        cleanup();
        return r;
    }
    r.started  = true;
    // The job stands in for the process group: a timeout ends all of it.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job && !AssignProcessToJobObject(job, pi.hProcess))
        closeHandle(job);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    ReadCtx  rc{outRead, &r.output};
    WriteCtx wc{inWrite, &o.input};
    HANDLE   reader = CreateThread(nullptr, 0, readAll, &rc, 0, nullptr);
    HANDLE   writer = nullptr;
    if (feed && inWrite) {
        writer = CreateThread(nullptr, 0, writeAll, &wc, 0, nullptr);
        if (writer)
            inWrite = nullptr; // the writer closes it
    }
    closeHandle(inWrite); // no writer: the child sees the end of its input at once

    const DWORD wait = o.timeoutMs > 0 ? DWORD(o.timeoutMs) : INFINITE;
    if (WaitForSingleObject(pi.hProcess, wait) == WAIT_TIMEOUT) {
        r.timedOut = true;
        if (job)
            TerminateJobObject(job, 1);
        else
            TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    r.code     = GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE ? int(code) : -1;
    // What it wrote before exiting is in the pipe; a process it left running
    // that kept the handle (and so the pipe open) is not waited for.
    if (reader)
        joinPipeThread(reader, 1000);
    else
        readAll(&rc); // no thread: read here (the pipe ends with the child)
    joinPipeThread(writer, 0);
    CloseHandle(pi.hProcess);
    closeHandle(job);
    cleanup();
    return r;
}

#else

namespace {

// exec's argv: exe, the arguments, a null.
std::vector<char *> argvOf(const std::string &exe, const std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char *>(exe.c_str()));
    for (const auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

// A clean signal state — our threads' masks (SIGCHLD blocked) and ignored
// signals are not the child's business — and a session of its own, so the
// group can be signalled as a whole (see process.h).
void initSpawnAttr(posix_spawnattr_t *attr) {
    posix_spawnattr_init(attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(attr, &none);
    posix_spawnattr_setsigdefault(attr, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;
#else
    flags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setpgroup(attr, 0);
#endif
    posix_spawnattr_setflags(attr, flags);
}

} // namespace

bool Process::start(const std::string &exe, const std::vector<std::string> &args) {
    std::vector<char *>        argv = argvOf(exe, args);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    initSpawnAttr(&attr);
    pid_t     pid = 0;
    const int rc  = posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
        return false;
    _pid    = pid;
    _exited = false;
    return true;
}

bool Process::running() {
    if (_pid <= 0 || _exited)
        return false;
    int status = 0;
    if (waitpid(pid_t(_pid), &status, WNOHANG) == pid_t(_pid))
        _exited = true, _status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return !_exited;
}

void Process::kill(bool force) {
    if (!running())
        return;
    const int sig = force ? SIGKILL : SIGTERM;
    // The group the child leads (see start); the child alone if that failed.
    if (::getpgid(pid_t(_pid)) == pid_t(_pid))
        ::kill(-pid_t(_pid), sig);
    else
        ::kill(pid_t(_pid), sig);
}

// ── run() ───────────────────────────────────────────────────────────────────

namespace {

bool makePipe(int fds[2]) {
    if (::pipe(fds) != 0)
        return false;
    // Only the child's dup2'd copies (0, 1, 2) cross the exec.
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

void closeFd(int &fd) {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

std::string_view envName(std::string_view kv) {
    return kv.substr(0, kv.find('='));
}

int statusCode(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

// Reads what is there now; false once the pipe has ended (or failed).
bool drain(int fd, std::string &out) {
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 && errno == EAGAIN;
    }
}

// Writes what the pipe takes now; true once all of `in` is written — or
// dropped, the child having closed its end (*epipe then set).
bool feedInput(int fd, const std::string &in, size_t &written, bool *epipe) {
    while (written < in.size()) {
        const ssize_t w = ::write(fd, in.data() + written, in.size() - written);
        if (w > 0) {
            written += size_t(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN)
            return false;
        *epipe = *epipe || (w < 0 && errno == EPIPE);
        return true;
    }
    return true;
}

} // namespace

// Our environment with `overrides` applied: "NAME=value" sets, "NAME=" unsets.
std::vector<std::string> mergedEnv(const std::vector<std::string> &overrides) {
    std::vector<std::string> out;
    for (char **e = environ; *e; ++e) {
        const std::string_view kv(*e);
        bool                   overridden = false;
        for (const auto &o : overrides)
            overridden = overridden || envName(o) == envName(kv);
        if (!overridden)
            out.emplace_back(kv);
    }
    for (size_t i = 0; i < overrides.size(); ++i) {
        const std::string     &o    = overrides[i];
        const std::string_view name = envName(o);
        if (name.empty() || name.size() + 1 >= o.size())
            continue;       // no name, or "NAME=": unset
        bool later = false; // the last word on a name wins
        for (size_t j = i + 1; j < overrides.size(); ++j)
            later = later || envName(overrides[j]) == name;
        if (!later)
            out.push_back(o);
    }
    return out;
}

RunResult run(const std::string &exe, const std::vector<std::string> &args, const RunOptions &o) {
    RunResult  r;
    const bool feed       = !o.input.empty();
    int        outPipe[2] = {-1, -1}, inPipe[2] = {-1, -1};
    if (!makePipe(outPipe) || (feed && !makePipe(inPipe))) {
        r.output = std::strerror(errno);
        closeFd(outPipe[0]);
        closeFd(outPipe[1]);
        return r;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (feed)
        posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    else // never waits on a prompt
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    if (o.mergeStderr)
        posix_spawn_file_actions_adddup2(&fa, outPipe[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (!o.cwd.empty()) {
#ifdef __APPLE__
// Deprecated in macOS 26 for posix_spawn_file_actions_addchdir, which older
// macOS lacks.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
    }
    posix_spawnattr_t attr;
    initSpawnAttr(&attr); // as Process::start
    std::vector<char *>      argv = argvOf(exe, args);
    std::vector<std::string> envStore;
    std::vector<char *>      envp;
    if (!o.env.empty()) {
        envStore = mergedEnv(o.env);
        for (auto &e : envStore)
            envp.push_back(e.data());
        envp.push_back(nullptr);
    }
    pid_t     pid = 0;
    const int rc  = posix_spawnp(
        &pid, exe.c_str(), &fa, &attr, argv.data(), envp.empty() ? environ : envp.data()
    );
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    closeFd(outPipe[1]);
    closeFd(inPipe[0]);
    if (rc != 0) {
        r.output = std::strerror(rc);
        closeFd(outPipe[0]);
        closeFd(inPipe[1]);
        return r;
    }
    r.started = true;
    ::fcntl(outPipe[0], F_SETFL, ::fcntl(outPipe[0], F_GETFL) | O_NONBLOCK);

    // A child that exits without reading all its input must not SIGPIPE us:
    // the signal is blocked on this thread while we write (or, where a pipe
    // can say so, never raised).
    sigset_t pipeSet, oldMask;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    bool epipe = false;
    if (feed) {
        ::fcntl(inPipe[1], F_SETFL, ::fcntl(inPipe[1], F_GETFL) | O_NONBLOCK);
#ifdef F_SETNOSIGPIPE
        ::fcntl(inPipe[1], F_SETNOSIGPIPE, 1);
#endif
        pthread_sigmask(SIG_BLOCK, &pipeSet, &oldMask);
    }

    const int64_t deadline = o.timeoutMs > 0 ? monotonicMs() + o.timeoutMs : 0;
    auto   left    = [&]() -> int64_t { return deadline ? deadline - monotonicMs() : INT64_MAX; };
    size_t written = 0;
    bool   outOpen = true, exited = false;
    int    status = 0;
    auto   reap   = [&](int opts) {
        const pid_t w = ::waitpid(pid, &status, opts);
        if (w == pid || (w < 0 && errno == ECHILD)) { // ECHILD: reaped elsewhere, status gone
            exited = true;
            if (w != pid)
                status = -1;
        }
    };
    // Its output until it ends — or until the child has exited: a process it
    // left running in the background may hold the pipe open for good.
    while (outOpen && !exited && left() > 0) {
        pollfd fds[2];
        nfds_t n = 0;
        fds[n++] = {outPipe[0], POLLIN, 0};
        if (inPipe[1] >= 0)
            fds[n++] = {inPipe[1], POLLOUT, 0};
        if (::poll(fds, n, int(std::min<int64_t>(left(), 50))) > 0) {
            if (fds[0].revents)
                outOpen = drain(outPipe[0], r.output);
            if (n > 1 && fds[1].revents && feedInput(inPipe[1], o.input, written, &epipe))
                closeFd(inPipe[1]); // the end of its input
        }
        reap(WNOHANG);
    }
    closeFd(inPipe[1]);
    // The output has ended: the exit follows within moments.
    for (int nap = 1; !exited && left() > 0; nap = std::min(nap * 2, 20)) {
        reap(WNOHANG);
        if (!exited)
            ::poll(nullptr, 0, int(std::min<int64_t>(left(), nap)));
    }
    if (!exited) {
        r.timedOut = true;
        if (::kill(-pid, SIGKILL) != 0) // the group it leads; the child alone if that failed
            ::kill(pid, SIGKILL);
        reap(0);
    }
    if (outOpen)
        drain(outPipe[0], r.output); // what it wrote before it exited
    closeFd(outPipe[0]);
    if (feed) {
#ifndef F_SETNOSIGPIPE
        if (epipe) {
            // The SIGPIPE the failed write raised is pending on this thread:
            // taken here, so unblocking does not deliver it.
            const timespec zero{0, 0};
            while (sigtimedwait(&pipeSet, nullptr, &zero) == SIGPIPE) {
            }
        }
#endif
        pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
    }
    r.code = status < 0 ? -1 : statusCode(status);
    return r;
}

#endif

// ── Restart ─────────────────────────────────────────────────────────────────

std::string executablePath() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::string out = narrow(buf);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string out(size, '\0');
    if (_NSGetExecutablePath(out.data(), &size) != 0)
        return {};
    out.resize(std::strlen(out.c_str()));
    char real[PATH_MAX];
    return ::realpath(out.c_str(), real) ? std::string(real) : out;
#else
    char       buf[4096];
    const auto n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
#endif
}

namespace {

std::vector<std::string> *g_relaunchArgs = nullptr;

void relaunchNow() {
    if (!g_relaunchArgs)
        return;
    const std::string exe = executablePath();
    if (exe.empty())
        return;
#ifdef _WIN32
    // Main has returned: the window, the tray and the single-instance pipe
    // are gone, so the new process becomes the primary instance.
    Process p;
    p.start(exe, *g_relaunchArgs);
#else
    std::fflush(nullptr);
    std::vector<char *> argv = argvOf(exe, *g_relaunchArgs);
    // exec keeps the signal mask; the new image starts with a clean one.
    sigset_t            none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, nullptr);
    ::execv(exe.c_str(), argv.data());
    // Only returns on failure: the normal exit goes on.
#endif
}

} // namespace

void relaunchOnExit(std::vector<std::string> args) {
    if (!g_relaunchArgs) {
        g_relaunchArgs = new std::vector<std::string>();
        std::atexit(relaunchNow);
    }
    *g_relaunchArgs = std::move(args);
}

} // namespace base
