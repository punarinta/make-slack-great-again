// poll(2)-based loop shared by the Wayland, X11 and headless backends.
// The display connection is just another watched fd; `beforeWait` lets a
// backend drain already-queued events and flush its requests before sleeping
// (wl_display_prepare_read / xcb_flush), `afterWait` lets it cancel/read.
#pragma once

#include "core/loop_core.h"

#include <vector>

#include <poll.h>

namespace plat::posix {

class PosixLoop {
public:
    PosixLoop();
    ~PosixLoop();
    PosixLoop(const PosixLoop &)            = delete;
    PosixLoop &operator=(const PosixLoop &) = delete;

    core::LoopCore core;

    // Return false to skip the blocking poll this iteration (work pending).
    std::function<bool()>         beforeWait;
    // Right after the poll, before any watch callback: what it found on the
    // watch `waitWatch` names (FdRead / FdWrite; 0 = nothing or no poll), so
    // the display connection is read without polling it again.
    std::function<void(uint32_t)> afterWait;
    uint64_t                      waitWatch = 0;

    uint64_t watch(int fd, uint32_t events, std::function<void(uint32_t)> fn);
    void     unwatch(uint64_t id);

    // One iteration: wait up to timeoutMs, then fds → posted → timers.
    void iterate(int timeoutMs);
    void run();
    void quit() {
        _quit = true;
        wakeUp();
    }

    void wakeUp(); // thread-safe

private:
    struct Watch {
        uint64_t                      id;
        int                           fd;
        uint32_t                      events;
        std::function<void(uint32_t)> fn;
    };
    std::vector<Watch>    _watches; // a few, in id order
    uint64_t              _nextWatch = 1;
    std::vector<pollfd>   _pfds; // iterate()'s scratch
    std::vector<uint64_t> _ids;
    int                   _wakeRead = -1, _wakeWrite = -1;
    bool                  _quit = false;
};

} // namespace plat::posix
