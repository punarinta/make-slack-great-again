// Background work, and the jobs the user sees running (the footer's
// spinning cog lists them on hover).
//
// The rule: nothing heavy runs on the UI thread. Network calls already
// answer later (net::Client); disk and CPU work — reading a file to upload,
// writing a download, parsing a CSV, building a transcript — goes through
// runInBackground. A job the user waits on (a download, a copy, a forward,
// a summary) also registers with jobs() from the click until it ends, on the
// failure path too:
//
//   const int job = model::jobs().begin(description); // "Downloading report.pdf"
//   … async work …
//   model::jobs().end(job);
//
// Jobs is UI-thread only; runInBackground may be called from it alone too.
#pragma once

#include "base/observers.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace model {

// work() on a worker thread, then then() on the UI thread (never inside this
// call). Workers are reused: up to 4 finished ones wait for the next call,
// each for 30 s. At most 4-8 workers run (by the hardware); more calls wait
// for one, oldest first, so work must never wait on other background work.
// Nothing is cancelled: a caller that may be gone by then guards its
// callback (a weak alive flag).
void runInBackground(plat::App &app, std::function<void()> work, std::function<void()> then);
// The app is shutting down: results still to come are dropped, never posted
// to a loop that may be gone. Call before the plat::App is destroyed.
void stopBackground();
// Blocks until every worker under way has finished and posted its result
// (left in the loop, to run or be dropped with it). Before a plat::App that
// workers were started on goes while the process goes on (tests make one per
// case): a worker posting to a destroyed App is a use-after-free.
void waitBackground();

// The pool, for tests: threads alive, of them parked, the most that run,
// and threads ever started.
struct BackgroundStats {
    int      workers = 0, parked = 0, cap = 0;
    uint64_t started = 0;
};
BackgroundStats backgroundStats();
// How long a parked worker waits before it retires (tests shorten it).
void            setBackgroundIdleRetire(int ms);

class Jobs {
public:
    // A running job with what the hover list says ("Copying photo.png");
    // returns the id to pass to end().
    int                      begin(std::string description);
    // Safe with an unknown or already ended id.
    void                     end(int id);
    size_t                   count() const { return _active.size(); }
    // Running jobs' descriptions, oldest first (empty ones skipped).
    std::vector<std::string> descriptions() const;

    // After every begin / end. Returns an id for unobserve().
    using ObserverId = base::Observers::Id;
    ObserverId observe(std::function<void()> fn);
    void       unobserve(ObserverId id) { _observers.remove(id); }

private:
    void changed() { _observers.notify(); }

    struct Job {
        int         id;
        std::string description;
    };
    std::vector<Job> _active; // ids grow: oldest first
    base::Observers  _observers;
    int              _nextId = 1;
};

// The app's one registry.
Jobs &jobs();

} // namespace model
