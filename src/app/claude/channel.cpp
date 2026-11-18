#include "app/claude/channel.h"

#include "app/claude/relay_protocol.h"
#include "base/process.h"
#include "base/str.h"
#include "base/thread.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <utility>

#ifdef _WIN32
#include "base/winstr.h"
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace claude {

Channel::~Channel() = default;

namespace {

// The last line of what a program said on stderr, for the user.
std::string lastLine(std::string_view s) {
    s               = str::trim(s);
    const size_t nl = s.rfind('\n');
    return std::string(str::trim(nl == std::string_view::npos ? s : s.substr(nl + 1)));
}

class ProcessChannel final : public Channel {
public:
    ProcessChannel(std::string exe, std::vector<std::string> args)
        : _exe(std::move(exe)), _args(std::move(args)) {}
    ~ProcessChannel() override { close(); }

    bool open(Line onLine, Closed onClosed, std::string *error) override;
    void send(std::string line) override {
        line.push_back('\n');
        {
            base::Lock lock(_lock);
            if (_stopping)
                return;
            _queue.push_back(std::move(line));
        }
        _wake.notifyOne();
    }
    void close() override;

private:
    void readLoop();  // stdout → onLine, then onClosed
    void writeLoop(); // the queue → stdin
    void errLoop();   // stderr, its tail kept
    bool writeBytes(std::string_view s);
    int  readBytes(char *buf, int size);
    void endProcess(); // the child ended (made to, when it still runs)

    std::string              _exe;
    std::vector<std::string> _args;
    Line                     _onLine;
    Closed                   _onClosed;
    base::Thread             _reader, _writer, _errReader;
    base::Mutex              _lock;
    base::CondVar            _wake;
    std::deque<std::string>  _queue;
    bool                     _stopping = false, _started = false;
    std::atomic<bool>        _closing{false};
    std::string              _err; // stderr's last 4 KB
    base::Mutex              _errLock;
#ifdef _WIN32
    HANDLE _process = nullptr, _in = nullptr, _out = nullptr, _errPipe = nullptr;
#else
    pid_t _pid = -1;
    int   _in = -1, _out = -1, _errPipe = -1;
#endif
};

bool ProcessChannel::open(Line onLine, Closed onClosed, std::string *error) {
    _onLine   = std::move(onLine);
    _onClosed = std::move(onClosed);
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE              inR = nullptr, outW = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &_in, &sa, 0) || !CreatePipe(&_out, &outW, &sa, 0) ||
        !CreatePipe(&_errPipe, &errW, &sa, 0)) {
        *error = "pipe";
        return false;
    }
    SetHandleInformation(_in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(_out, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(_errPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    base::appendQuoted(cmd, base::wide(_exe));
    for (const auto &a : _args)
        base::appendQuoted(cmd, base::wide(a));
    STARTUPINFOW si{};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = errW;
    PROCESS_INFORMATION pi{};
    const std::wstring  exe = base::wide(_exe);
    const BOOL          ok  = CreateProcessW(
        exe.c_str(),
        cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        *error = "can't start " + _exe;
        return false;
    }
    CloseHandle(pi.hThread);
    _process = pi.hProcess;
#else
    int in[2], out[2], err[2];
    if (::pipe(in) != 0 || ::pipe(out) != 0 || ::pipe(err) != 0) {
        *error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    for (int fd : {in[1], out[0], err[0]})
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    // Everything exec needs is made before fork: the child only calls what
    // is safe between fork and exec.
    std::vector<std::string> env = base::mergedEnv({});
    std::vector<char *>      argv{const_cast<char *>(_exe.c_str())};
    for (auto &a : _args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<char *> envp;
    for (auto &e : env)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    _pid = ::fork();
    if (_pid < 0) {
        *error = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (_pid == 0) {
        // A session of its own: no controlling terminal, so ssh can't ask
        // for a password or a host key on one (BatchMode says no instead).
        ::setsid();
        ::dup2(in[0], 0);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]})
            if (fd > 2)
                ::close(fd);
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    _in      = in[1];
    _out     = out[0];
    _errPipe = err[0];
#endif
    _started = true;
    _errReader.start([this] { errLoop(); }, 64 * 1024);
    _writer.start([this] { writeLoop(); }, 64 * 1024);
    _reader.start([this] { readLoop(); });
    return true;
}

bool ProcessChannel::writeBytes(std::string_view s) {
    while (!s.empty()) {
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(_in, s.data(), DWORD(std::min<size_t>(s.size(), 1 << 20)), &n, nullptr))
            return false;
#else
        const ssize_t n = ::write(_in, s.data(), s.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
#endif
        s.remove_prefix(size_t(n));
    }
    return true;
}

int ProcessChannel::readBytes(char *buf, int size) {
#ifdef _WIN32
    DWORD n = 0;
    return ReadFile(_out, buf, DWORD(size), &n, nullptr) ? int(n) : 0;
#else
    for (;;) {
        const ssize_t n = ::read(_out, buf, size_t(size));
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 ? 0 : int(n);
    }
#endif
}

void ProcessChannel::writeLoop() {
#ifndef _WIN32
    // A relay gone (its pipe closed) is a failed write here, never a SIGPIPE
    // that ends msga: blocked on this thread, which only writes.
    sigset_t pipe;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
#ifdef F_SETNOSIGPIPE
    ::fcntl(_in, F_SETNOSIGPIPE, 1);
#endif
#endif
    for (;;) {
        std::string line;
        {
            base::Lock lock(_lock);
            _wake.wait(lock, [this] { return _stopping || !_queue.empty(); });
            if (_queue.empty())
                break; // stopping, all written
            line = std::move(_queue.front());
            _queue.pop_front();
        }
        if (!writeBytes(line))
            break;
    }
    // Its stdin closed: a relay reads that as msga hanging up.
#ifdef _WIN32
    CloseHandle(std::exchange(_in, nullptr));
#else
    ::close(std::exchange(_in, -1));
#endif
}

void ProcessChannel::errLoop() {
    char buf[4096];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(_errPipe, buf, sizeof buf, &n, nullptr) || n == 0)
            break;
#else
        const ssize_t n = ::read(_errPipe, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
#endif
        base::Lock lock(_errLock);
        _err.append(buf, size_t(n));
        if (_err.size() > 4096)
            _err.erase(0, _err.size() - 4096);
    }
}

void ProcessChannel::readLoop() {
    std::string pending;
    char        buf[64 * 1024];
    bool        tooLong = false;
    for (;;) {
        const int n = readBytes(buf, int(sizeof buf));
        if (n <= 0)
            break;
        pending.append(buf, size_t(n));
        size_t at = 0;
        for (size_t nl; (nl = pending.find('\n', at)) != std::string::npos; at = nl + 1)
            if (!_closing)
                _onLine(std::string_view(pending).substr(at, nl - at));
        pending.erase(0, at);
        if (pending.size() > relay::kMaxLine) {
            tooLong = true;
            break;
        }
    }
    endProcess();
    _errReader.join();
    std::string why;
    {
        base::Lock lock(_errLock);
        why = lastLine(_err);
    }
    if (tooLong)
        why = "line too long";
    if (!_closing)
        _onClosed(std::move(why));
}

void ProcessChannel::endProcess() {
#ifdef _WIN32
    if (_process) {
        if (WaitForSingleObject(_process, 2000) != WAIT_OBJECT_0)
            TerminateProcess(_process, 1);
        WaitForSingleObject(_process, INFINITE);
        CloseHandle(std::exchange(_process, nullptr));
    }
#else
    if (_pid > 0) {
        int status = 0;
        // Its stdout closed: it is ending (or, a line too long, made to).
        for (int i = 0; i < 40 && ::waitpid(_pid, &status, WNOHANG) == 0; ++i)
            ::usleep(50'000);
        if (::waitpid(_pid, &status, WNOHANG) == 0) {
            ::kill(-_pid, SIGKILL);
            ::waitpid(_pid, &status, 0);
        }
        _pid = -1;
    }
#endif
}

void ProcessChannel::close() {
    if (!_started || _closing.exchange(true))
        return;
    {
        base::Lock lock(_lock);
        _stopping = true;
    }
    _wake.notifyAll();
    // Ended first: a writer stuck on a full pipe (a relay not reading) then
    // fails instead of holding this up. The relay sees its link go.
#ifdef _WIN32
    if (_process)
        TerminateProcess(_process, 1);
#else
    if (_pid > 0)
        ::kill(-_pid, SIGTERM); // its whole session: ssh, a ProxyCommand
#endif
    _writer.join();
    _reader.join();
#ifdef _WIN32
    if (_out)
        CloseHandle(std::exchange(_out, nullptr));
    if (_errPipe)
        CloseHandle(std::exchange(_errPipe, nullptr));
#else
    ::close(std::exchange(_out, -1));
    ::close(std::exchange(_errPipe, -1));
#endif
}

} // namespace

std::unique_ptr<Channel> processChannel(std::string exe, std::vector<std::string> args) {
    return std::make_unique<ProcessChannel>(std::move(exe), std::move(args));
}

} // namespace claude
