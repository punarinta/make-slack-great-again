// `msga --cc-relay` (relay_server.h): no window system at all — not plat's
// backends, which need a display — but a loop of its own, enough for what the
// Claude Code parts ask of a plat::App: posted closures, timers, readable fds.
// POSIX only: a Windows machine isn't served (its `claude` sessions, ConPTY
// included, are reached by running msga there).
#include "app/claude/relay_server.h"

#include "app/claude/cli.h"
#include "app/claude/common.h"
#include "app/claude/relay_protocol.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/thread.h"
#include "base/time.h"
#include "plat/plat.h"

#include <cstdio>

#ifndef _WIN32
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#endif

namespace claude {

#ifdef _WIN32

int runRelay(int, char **) {
    std::fprintf(stderr, "msga --cc-relay: not available on Windows\n");
    return 2;
}

#else

namespace {

// The loop: poll() over a wake pipe and the watched fds, timers by deadline.
class Loop final : public plat::App {
public:
    Loop() {
        if (::pipe(_wake) == 0) {
            ::fcntl(_wake[0], F_SETFL, O_NONBLOCK);
            ::fcntl(_wake[1], F_SETFL, O_NONBLOCK);
            ::fcntl(_wake[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(_wake[1], F_SETFD, FD_CLOEXEC);
        }
    }
    ~Loop() override {
        ::close(_wake[0]);
        ::close(_wake[1]);
    }

    const char                   *backendName() const override { return "relay"; }
    std::unique_ptr<plat::Window> createWindow(const plat::WindowDesc &) override {
        return nullptr;
    }
    void run() override {
        while (!_quit)
            pump(-1);
    }
    void quit() override {
        _quit = true;
        wake();
    }
    void post(std::function<void()> fn) override {
        {
            base::Lock lock(_postLock);
            _posted.push_back(std::move(fn));
        }
        wake();
    }
    plat::TimerId addTimer(int intervalMs, bool repeat, std::function<void()> fn) override {
        const plat::TimerId id = ++_nextId;
        _timers.push_back(
            {id, base::monotonicMs() + std::max(intervalMs, 0), intervalMs, repeat, std::move(fn)}
        );
        return id;
    }
    void cancelTimer(plat::TimerId id) override {
        std::erase_if(_timers, [id](const Timer &t) { return t.id == id; });
    }
    uint64_t watchFd(int fd, uint32_t events, std::function<void(uint32_t)> fn) override {
        const uint64_t id = ++_nextId;
        _watches.push_back({id, fd, events, std::move(fn)});
        return id;
    }
    void unwatchFd(uint64_t id) override {
        std::erase_if(_watches, [id](const Watch &w) { return w.id == id; });
    }
    void setClipboard(std::vector<plat::DataItem>, plat::Selection) override {}
    void requestClipboard(
        std::string_view, std::function<void(std::optional<std::string>)> cb, plat::Selection
    ) override {
        post([cb = std::move(cb)] { cb(std::nullopt); });
    }
    bool darkMode() const override { return false; }
    int  doubleClickMs() const override { return 400; }
    bool openUrl(std::string_view) override { return false; }

    void pump(int timeoutMs) override {
        int64_t wait = timeoutMs < 0 ? 60'000 : timeoutMs;
        for (const Timer &t : _timers)
            wait = std::min<int64_t>(wait, std::max<int64_t>(0, t.dueMs - base::monotonicMs()));
        {
            base::Lock lock(_postLock);
            if (!_posted.empty())
                wait = 0;
        }
        std::vector<pollfd>   fds{{_wake[0], POLLIN, 0}};
        std::vector<uint64_t> ids{0};
        for (const Watch &w : _watches) {
            short ev = 0;
            if (w.events & plat::FdRead)
                ev |= POLLIN;
            if (w.events & plat::FdWrite)
                ev |= POLLOUT;
            fds.push_back({w.fd, ev, 0});
            ids.push_back(w.id);
        }
        if (::poll(fds.data(), nfds_t(fds.size()), int(wait)) > 0) {
            if (fds[0].revents) {
                char buf[64];
                while (::read(_wake[0], buf, sizeof buf) > 0) {
                }
            }
            // Each callback may unwatch others: looked up by id.
            for (size_t i = 1; i < fds.size(); ++i) {
                if (!fds[i].revents)
                    continue;
                uint32_t ev = 0;
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
                    ev |= plat::FdRead;
                if (fds[i].revents & POLLOUT)
                    ev |= plat::FdWrite;
                const auto w = std::find_if(_watches.begin(), _watches.end(), [&](const Watch &x) {
                    return x.id == ids[i];
                });
                if (w != _watches.end()) {
                    auto fn = w->fn; // the callback may unwatch itself
                    fn(ev);
                }
            }
        }
        std::vector<std::function<void()>> posted;
        {
            base::Lock lock(_postLock);
            posted.swap(_posted);
        }
        for (auto &fn : posted)
            fn();
        // Timers due now; one may add or cancel others.
        const int64_t              now = base::monotonicMs();
        std::vector<plat::TimerId> due;
        for (const Timer &t : _timers)
            if (t.dueMs <= now)
                due.push_back(t.id);
        for (plat::TimerId id : due) {
            const auto t = std::find_if(_timers.begin(), _timers.end(), [id](const Timer &x) {
                return x.id == id;
            });
            if (t == _timers.end())
                continue;
            auto fn = t->fn;
            if (t->repeat)
                t->dueMs = now + std::max(t->interval, 1);
            else
                _timers.erase(t);
            fn();
        }
    }

private:
    void wake() {
        const char                  c = 1;
        [[maybe_unused]] const auto n = ::write(_wake[1], &c, 1);
    }

    struct Timer {
        plat::TimerId         id;
        int64_t               dueMs;
        int                   interval;
        bool                  repeat;
        std::function<void()> fn;
    };
    struct Watch {
        uint64_t                      id;
        int                           fd;
        uint32_t                      events;
        std::function<void(uint32_t)> fn;
    };
    int                                _wake[2] = {-1, -1};
    bool                               _quit    = false;
    uint64_t                           _nextId  = 0;
    std::vector<Timer>                 _timers;
    std::vector<Watch>                 _watches;
    base::Mutex                        _postLock;
    std::vector<std::function<void()>> _posted;
};

// Everything to stdout, whole lines, blocking (a slow ssh link slows the
// relay, never loses a line). False once msga is gone.
bool writeAll(std::string_view s) {
    while (!s.empty()) {
        const ssize_t n = ::write(STDOUT_FILENO, s.data(), s.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        s.remove_prefix(size_t(n));
    }
    return true;
}

// The relay's own folder: what msga's permission hook and uploads need here.
// Private to the user (0700): questions name commands, uploads are theirs.
std::string privateDir(std::string path) {
    file::makeDirs(path);
    ::chmod(path.c_str(), 0700);
    return path;
}

} // namespace

int runRelay(int, char **) {
    ::signal(SIGPIPE, SIG_IGN); // a gone msga is a failed write, not a death
    // stdout carries the protocol and nothing else: logs go to stderr (ssh
    // passes that on, msga keeps the last lines for its error message).
    const std::string home = base::homeDir();
    std::string       tmp  = base::env("TMPDIR");
    if (tmp.empty())
        tmp = "/tmp";
    const std::string self = str::concat({tmp, "/msga-relay-", str::number(int64_t(::getuid()))});
    privateDir(self); // its own first: what it holds is no one else's to list
    Dirs d;
    d.data  = privateDir(home.empty() ? self + "/data" : home + "/.local/share/msga-relay");
    d.cache = privateDir(self + "/cache");
    setDirs(d);

    Loop      loop;
    LocalHost host(loop, findClaudeExecutable(), Paths::detect(), d.data + "/approvals");
    // The sessions it starts ask their permission questions through it, and
    // report their progress through it (`msga --claude-status` here).
    host.useHook(base::executablePath());
    bool        gone = false;
    RelayServer server(loop, host, privateDir(self + "/uploads"), [&](std::string line) {
        if (!gone && !writeAll(line)) {
            gone = true;
            loop.quit();
        }
    });
    server.onIdle = [&] { loop.quit(); };

    std::string pending;
    ::fcntl(STDIN_FILENO, F_SETFL, ::fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    loop.watchFd(STDIN_FILENO, plat::FdRead, [&](uint32_t) {
        char          buf[64 * 1024];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        if (n <= 0) { // msga hung up: done
            loop.quit();
            return;
        }
        pending.append(buf, size_t(n));
        size_t at = 0;
        for (size_t nl; (nl = pending.find('\n', at)) != std::string::npos; at = nl + 1)
            server.receive(std::string_view(pending).substr(at, nl - at));
        pending.erase(0, at);
        if (pending.size() > relay::kMaxLine)
            loop.quit();
    });
    server.start();
    loop.run();
    // What runs on workers reports to nobody now.
    model::stopBackground();
    return 0;
}

#endif

} // namespace claude
