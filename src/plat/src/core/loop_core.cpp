#include "core/loop_core.h"

#include <algorithm>

namespace plat::core {

TimerId LoopCore::addTimer(int intervalMs, bool repeat, std::function<void()> fn) {
    const auto    iv = std::chrono::milliseconds(std::max(0, intervalMs));
    const TimerId id = _nextTimer++;
    _timers.push_back(Timer{id, Clock::now() + iv, iv, repeat, std::move(fn)});
    _nextDue = std::min(_nextDue, _timers.back().due);
    return id;
}

void LoopCore::cancelTimer(TimerId id) {
    for (size_t i = 0; i < _timers.size(); ++i)
        if (_timers[i].id == id) {
            // Destroyed after the erase: its destructor may add or cancel timers.
            const auto fn       = std::move(_timers[i].fn);
            const bool earliest = _timers[i].due <= _nextDue;
            _timers.erase(_timers.begin() + ptrdiff_t(i));
            if (earliest)
                this->earliest();
            return;
        }
}

void LoopCore::earliest() {
    _nextDue = Clock::time_point::max();
    for (const auto &t : _timers)
        _nextDue = std::min(_nextDue, t.due);
}

int LoopCore::msUntilNextTimer() const {
    if (_timers.empty())
        return -1;
    const auto now = Clock::now();
    if (_nextDue <= now)
        return 0;
    // Round up: waking a millisecond early just spins the loop once more.
    return int(std::chrono::ceil<std::chrono::milliseconds>(_nextDue - now).count());
}

int LoopCore::clampTimeout(int timeoutMs) const {
    const int t = msUntilNextTimer();
    if (t < 0)
        return timeoutMs;
    return timeoutMs < 0 ? t : std::min(t, timeoutMs);
}

void LoopCore::runDueTimers() {
    const auto now = Clock::now();
    if (_nextDue > now)
        return;
    // A callback may add or cancel timers (including itself), so each round
    // looks for the earliest due timer again (ties: the older one first).
    // Timers added by this round's callbacks wait for the next call.
    const TimerId fresh = _nextTimer;
    for (;;) {
        const size_t n    = _timers.size();
        size_t       best = n;
        for (size_t i = 0; i < n; ++i) {
            const Timer &t = _timers[i];
            if (t.id < fresh && t.due <= now && (best == n || t.due < _timers[best].due))
                best = i;
        }
        if (best == n)
            break;
        Timer                &t = _timers[best];
        std::function<void()> fn;
        if (t.repeat) {
            fn = t.fn; // a copy: the callback may cancel its own timer
            // Re-arm from the scheduled time, not from now, so a repeating
            // timer does not drift; skip missed ticks rather than bursting.
            do
                t.due += std::max(t.interval, std::chrono::milliseconds(1));
            while (t.due <= now);
        } else {
            fn = std::move(t.fn);
            _timers.erase(_timers.begin() + ptrdiff_t(best));
        }
        fn();
    }
    earliest();
}

void LoopCore::post(std::function<void()> fn) {
    // `fn`, when dropped, is destroyed after the lock is released. Waking
    // under the lock means no wake() can still be running once shutdown()
    // returns and the backend tears down what it signals.
    std::lock_guard lock(_postMutex);
    if (_closed)
        return;
    // Only the first post into an empty queue wakes: every backend runs
    // runPosted() at some point after a wake, and runPosted() takes the whole
    // queue under this lock, so a closure queued behind another one runs in
    // the same batch, and the next post after that batch finds the queue
    // empty and wakes again.
    const bool first = _posted.empty();
    _posted.push_back(std::move(fn));
    if (first && wake)
        wake();
}

bool LoopCore::hasPosted() {
    std::lock_guard lock(_postMutex);
    return !_posted.empty();
}

void LoopCore::runPosted() {
    // The queue takes over the last batch's emptied buffer, so the posts
    // that follow do not reallocate it.
    std::vector<std::function<void()>> batch;
    batch.swap(_spare);
    {
        std::lock_guard lock(_postMutex);
        batch.swap(_posted);
    }
    // Work posted by these callbacks runs next iteration, so a closure that
    // re-posts itself cannot starve input.
    for (auto &fn : batch)
        fn();
    batch.clear();
    if (_spare.capacity() < batch.capacity())
        _spare.swap(batch);
}

void LoopCore::shutdown() {
    // In rounds, each destroyed outside the lock and outside the members: a
    // closure holding a retired net::WebSocket joins its reader thread, which
    // posts its close event while we are still destroying the batch.
    for (;;) {
        std::vector<Timer>                 timers;
        std::vector<std::function<void()>> posted;
        timers.swap(_timers);
        _nextDue = Clock::time_point::max();
        std::lock_guard lock(_postMutex);
        posted.swap(_posted);
        if (timers.empty() && posted.empty()) {
            _closed = true;
            return;
        }
    }
}

} // namespace plat::core
