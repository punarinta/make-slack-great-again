#include "app/model/jobs.h"

#include "base/thread.h"
#include "plat/plat.h"

#include <algorithm>

namespace model {

namespace {

// A pool of parked workers (base::WorkerPool): a call wakes one, else starts
// a new thread while fewer than workerCap() run; past that the task waits
// for the next free worker. No task waits on another (that would deadlock a
// full pool), but a CLI run may hold its worker up to its timeout, so the
// cap stays above the few such runs at once. Up to kMaxParked finished
// workers wait 30 s for the next call, so a quiet app holds no threads.
//
// Workers outlive nothing they post to: once stopBackground() ran, a worker
// that finishes drops its result (the app is going away; the loop may be gone).
constexpr int kMaxParked = 4;

// The hardware threads, clamped to 4..8: enough for blocking CLI runs beside
// file work, few enough that a burst (every transcript at launch) doesn't
// start a thread, and a malloc arena, per task.
int workerCap() {
    static const int cap = std::clamp(base::hardwareThreads(), 4, 8);
    return cap;
}

struct Background {
    base::Mutex      mutex;
    base::CondVar    idle; // running dropped to 0
    bool             stopped = false;
    int              running = 0; // tasks queued or under way (waitBackground)
    base::WorkerPool pool{{.maxWorkers = workerCap(), .maxParked = kMaxParked}};
};

// Never destroyed: a parked worker may still be waiting on it as the
// process exits.
Background &background() {
    static Background *b = new Background;
    return *b;
}

} // namespace

void runInBackground(plat::App &app, std::function<void()> work, std::function<void()> then) {
    Background &b = background();
    {
        base::Lock lock(b.mutex);
        ++b.running;
    }
    b.pool.post([&b, app = &app, work = std::move(work), then = std::move(then)]() mutable {
        if (work)
            work();
        work = nullptr; // what it held goes before waitBackground returns
        base::Lock lock(b.mutex);
        if (!b.stopped)
            app->post([then = std::move(then)] {
                if (then)
                    then();
            });
        if (--b.running == 0)
            b.idle.notifyAll();
        // A dropped then() is released by the pool, outside the lock.
    });
}

void stopBackground() {
    Background &b = background();
    base::Lock  lock(b.mutex);
    b.stopped = true;
}

void waitBackground() {
    Background &b = background();
    base::Lock  lock(b.mutex);
    b.idle.wait(lock, [&b] { return b.running == 0; });
}

BackgroundStats backgroundStats() {
    const base::WorkerPool::Stats s = background().pool.stats();
    return {s.workers, s.parked, workerCap(), s.started};
}

void setBackgroundIdleRetire(int ms) {
    background().pool.setIdleMs(ms);
}

int Jobs::begin(std::string description) {
    const int id = _nextId++;
    _active.push_back({id, std::move(description)});
    changed();
    return id;
}

void Jobs::end(int id) {
    for (size_t i = 0; i < _active.size(); ++i)
        if (_active[i].id == id) {
            _active.erase(_active.begin() + ptrdiff_t(i));
            changed();
            return;
        }
}

std::vector<std::string> Jobs::descriptions() const {
    std::vector<std::string> out;
    for (const Job &j : _active)
        if (!j.description.empty())
            out.push_back(j.description);
    return out;
}

Jobs::ObserverId Jobs::observe(std::function<void()> fn) {
    return _observers.add([fn = std::move(fn)](const std::string &) { fn(); });
}

Jobs &jobs() {
    static Jobs j;
    return j;
}

} // namespace model
