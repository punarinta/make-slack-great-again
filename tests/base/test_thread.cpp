// base::Thread and base::WorkerPool (the pool behind runInBackground, the
// net::Client workers and the image caches' serial workers).
#include "base/thread.h"
#include "support/test.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef __linux__
#include <dirent.h>
#endif

namespace {

// Polls `cond` for up to `ms`.
template <class F>
bool waitFor(F cond, int ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// A gate the first task of a test waits behind.
struct Gate {
    std::atomic<bool> entered{false}, open{false};
    void              pass() {
        entered = true;
        while (!open)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};

#ifdef __linux__
int threadCount() {
    int  n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return -1;
    while (const dirent *e = readdir(d))
        n += e->d_name[0] != '.';
    closedir(d);
    return n;
}
#endif

} // namespace

TEST("thread: runs, joins, detaches") {
    std::atomic<int> ran{0};
    {
        base::Thread t;
        REQUIRE(t.start([&] { ++ran; }));
        CHECK_FALSE(t.start([&] { ++ran; })); // one at a time
        t.join();
        CHECK(ran == 1);
        t.join();                                    // twice: nothing
        REQUIRE(t.start([&] { ++ran; }, 64 * 1024)); // free again after join
    } // the destructor joins
    CHECK(ran == 2);
    // Detached: it runs to its end without anyone joining it.
    auto         alive = std::make_shared<int>(0);
    base::Thread t;
    REQUIRE(t.start([&ran, alive] { ++ran; }));
    t.detach();
    CHECK(t.start([] {})); // the object is free at once
    t.join();
    CHECK(waitFor([&] { return ran == 3 && alive.use_count() == 1; }));
}

TEST("pool: one worker runs tasks in order; a front post jumps the queue") {
    base::WorkerPool pool({.maxWorkers = 1, .maxParked = 1});
    std::mutex       m;
    std::string      log;
    Gate             gate;
    pool.post([&] { gate.pass(); });
    REQUIRE(waitFor([&] { return bool(gate.entered); }));
    for (char c : std::string("abcd"))
        pool.post([&, c] {
            std::lock_guard<std::mutex> lock(m);
            log += c;
        });
    pool.post(
        [&] {
            std::lock_guard<std::mutex> lock(m);
            log += 'U';
        },
        true
    );
    gate.open = true;
    REQUIRE(waitFor([&] {
        std::lock_guard<std::mutex> lock(m);
        return log.size() == 5;
    }));
    CHECK_STR(log, "Uabcd");
    CHECK(pool.stats().started == 1);
    CHECK(pool.stats().workers == 1);
}

TEST("pool: at most maxWorkers run; past that tasks wait; extra ones don't park") {
    base::WorkerPool  pool({.maxWorkers = 3, .maxParked = 1, .idleMs = 60'000});
    std::atomic<int>  under{0}, most{0}, done{0};
    std::atomic<bool> release{false};
    for (int i = 0; i < 10; ++i)
        pool.post([&] {
            const int n = ++under;
            for (int seen = most; n > seen && !most.compare_exchange_weak(seen, n);)
                ;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            --under;
            ++done;
        });
    REQUIRE(waitFor([&] { return under == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(under == 3);
    CHECK(pool.stats().workers == 3);
    release = true;
    REQUIRE(waitFor([&] { return done == 10; }));
    CHECK(most == 3);
    // One stays parked for the next task, the others end.
    REQUIRE(waitFor([&] { return pool.stats().workers == 1; }));
    CHECK(pool.stats().parked == 1);
    CHECK(pool.stats().started == 3);
}

TEST("pool: stop drops the queued tasks, waits for the one under way") {
    auto             held = std::make_shared<int>(0);
    std::atomic<int> ran{0};
    base::WorkerPool pool({.maxWorkers = 1, .maxParked = 1});
    Gate             gate;
    pool.post([&] {
        gate.pass();
        ++ran;
    });
    REQUIRE(waitFor([&] { return bool(gate.entered); }));
    for (int i = 0; i < 5; ++i)
        pool.post([&ran, held] { ran += 100; });
    CHECK(held.use_count() == 6);
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gate.open = true;
    });
    pool.stop(); // returns once the running task has ended
    CHECK(ran == 1);
    CHECK(held.use_count() == 1); // the queued ones are gone
    CHECK(pool.stats().workers == 0);
    pool.post([&ran, held] { ran += 1000; }); // stopped: dropped
    CHECK(held.use_count() == 1);
    opener.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ran == 1);
}

TEST("pool: idle workers end; the next task starts a fresh one") {
#ifdef __linux__
    const int before = threadCount();
#endif
    base::WorkerPool pool({.maxWorkers = 2, .maxParked = 2, .idleMs = 30});
    std::atomic<int> done{0};
    pool.post([&] { ++done; });
    REQUIRE(waitFor([&] { return done == 1; }));
    REQUIRE(waitFor([&] { return pool.stats().workers == 0; }));
#ifdef __linux__
    CHECK(waitFor([&] { return threadCount() == before; })); // gone, not just parked
#endif
    pool.post([&] { ++done; });
    REQUIRE(waitFor([&] { return done == 2; }));
    CHECK(pool.stats().started == 2);
    // A long idle time shortened while a worker waits: it ends by the new one.
    pool.setIdleMs(60'000);
    pool.post([&] { ++done; });
    REQUIRE(waitFor([&] { return done == 3 && pool.stats().parked == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(pool.stats().workers == 1);
    pool.setIdleMs(10);
    CHECK(waitFor([&] { return pool.stats().workers == 0; }, 1000));
}
