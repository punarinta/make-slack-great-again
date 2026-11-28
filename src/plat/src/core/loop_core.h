// Backend-neutral half of the event loop: timers and the cross-thread post
// queue. Each backend owns the blocking wait (poll, MsgWaitForMultipleObjects,
// CFRunLoop) and calls in here before and after it.
#pragma once

#include "plat/plat.h"

#include <chrono>
#include <mutex>
#include <vector>

namespace plat::core {

using Clock = std::chrono::steady_clock;

class LoopCore {
public:
    LoopCore() = default;
    ~LoopCore() { shutdown(); }
    LoopCore(const LoopCore &)            = delete;
    LoopCore &operator=(const LoopCore &) = delete;

    // Called (from any thread) after post() queued work, so the backend can
    // wake its blocking wait: write to an eventfd, PostMessage, CFRunLoopWakeUp.
    std::function<void()> wake;

    TimerId addTimer(int intervalMs, bool repeat, std::function<void()> fn);
    void    cancelTimer(TimerId id);
    // Milliseconds until the earliest timer is due: -1 when none, 0 when overdue.
    int     msUntilNextTimer() const;
    // Clamp a backend wait timeout (-1 = forever) by the next timer.
    int     clampTimeout(int timeoutMs) const;
    void    runDueTimers();

    void post(std::function<void()> fn); // thread-safe
    void runPosted();
    bool hasPosted();

    // Destroys every pending timer and posted closure without running them,
    // then drops whatever is posted afterwards. Backends call it first thing
    // in their destructor, while `wake` and their own state still work: those
    // closures' destructors may post, add or cancel timers, or join threads
    // that post on the way out. Idempotent; the destructor calls it as well.
    void shutdown();

private:
    struct Timer {
        TimerId                   id;
        Clock::time_point         due;
        std::chrono::milliseconds interval;
        bool                      repeat;
        std::function<void()>     fn;
    };
    void earliest(); // recomputes _nextDue

    // A handful of timers: a vector in id (= creation) order, with the
    // earliest due time cached (never later than the real one) so an idle
    // loop turn does not walk it.
    std::vector<Timer> _timers;
    Clock::time_point  _nextDue   = Clock::time_point::max();
    TimerId            _nextTimer = 1;

    std::mutex                         _postMutex;
    std::vector<std::function<void()>> _posted;
    std::vector<std::function<void()>> _spare;          // loop thread: a drained batch's buffer
    bool                               _closed = false; // guarded by _postMutex
};

} // namespace plat::core
