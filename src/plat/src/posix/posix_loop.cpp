#include "posix/posix_loop.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace plat::posix {

namespace {

// poll()'s revents as FdRead / FdWrite (a hang-up or error reads: the read
// finds out).
uint32_t readiness(short revents) {
    uint32_t ready = 0;
    if (revents & (POLLIN | POLLHUP | POLLERR))
        ready |= FdRead;
    if (revents & POLLOUT)
        ready |= FdWrite;
    return ready;
}

} // namespace

PosixLoop::PosixLoop() {
    // A self-pipe rather than eventfd so the same code builds on macOS/BSD
    // should a test ever want it there.
    int fds[2];
    if (pipe(fds) == 0) {
        _wakeRead  = fds[0];
        _wakeWrite = fds[1];
        for (int fd : fds) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    core.wake = [this] { wakeUp(); };
}

PosixLoop::~PosixLoop() {
    core.wake = nullptr;
    if (_wakeRead >= 0)
        close(_wakeRead);
    if (_wakeWrite >= 0)
        close(_wakeWrite);
}

void PosixLoop::wakeUp() {
    if (_wakeWrite < 0)
        return;
    const char            b = 1;
    // EAGAIN means the pipe is already full of wake bytes: the loop will wake.
    [[maybe_unused]] auto n = write(_wakeWrite, &b, 1);
}

uint64_t PosixLoop::watch(int fd, uint32_t events, std::function<void(uint32_t)> fn) {
    const uint64_t id = _nextWatch++;
    _watches.push_back(Watch{id, fd, events, std::move(fn)});
    return id;
}

void PosixLoop::unwatch(uint64_t id) {
    for (size_t i = 0; i < _watches.size(); ++i)
        if (_watches[i].id == id) {
            // Destroyed after the erase: the closure's destructor may unwatch.
            const auto fn = std::move(_watches[i].fn);
            _watches.erase(_watches.begin() + ptrdiff_t(i));
            return;
        }
}

void PosixLoop::iterate(int timeoutMs) {
    bool mayBlock = true;
    if (beforeWait)
        mayBlock = beforeWait();
    if (core.hasPosted())
        mayBlock = false;

    // The poll arrays are reused across iterations (taken here and handed
    // back at the end, so a nested iterate() from a callback gets its own).
    std::vector<pollfd>   pfds = std::move(_pfds);
    std::vector<uint64_t> ids  = std::move(_ids);
    pfds.clear();
    ids.clear();
    pfds.push_back({_wakeRead, POLLIN, 0});
    for (const auto &w : _watches) {
        short ev = 0;
        if (w.events & FdRead)
            ev |= POLLIN;
        if (w.events & FdWrite)
            ev |= POLLOUT;
        pfds.push_back({w.fd, ev, 0});
        ids.push_back(w.id);
    }

    const int timeout = mayBlock ? core.clampTimeout(timeoutMs) : 0;
    int       n;
    do
        n = poll(pfds.data(), pfds.size(), timeout);
    while (n < 0 && errno == EINTR);

    if (afterWait) {
        uint32_t ready = 0;
        for (size_t i = 0; n > 0 && waitWatch && i < ids.size(); ++i)
            if (ids[i] == waitWatch)
                ready = readiness(pfds[i + 1].revents);
        afterWait(ready);
    }

    if (n > 0) {
        if (pfds[0].revents) {
            char buf[64];
            while (read(_wakeRead, buf, sizeof buf) > 0) {
            }
        }
        for (size_t i = 1; i < pfds.size(); ++i) {
            if (!pfds[i].revents)
                continue;
            const Watch *w = nullptr;
            for (const auto &x : _watches)
                if (x.id == ids[i - 1])
                    w = &x;
            if (!w)
                continue;    // unwatched by an earlier callback
            auto fn = w->fn; // copy: the callback may unwatch itself
            fn(readiness(pfds[i].revents));
        }
    }
    _pfds = std::move(pfds);
    _ids  = std::move(ids);
    core.runPosted();
    core.runDueTimers();
}

void PosixLoop::run() {
    _quit = false;
    while (!_quit)
        iterate(-1);
}

} // namespace plat::posix
