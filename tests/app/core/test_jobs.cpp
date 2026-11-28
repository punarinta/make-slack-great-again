// model::Jobs (the footer's background-task list) and runInBackground (work
// on a worker thread, the answer on the UI thread).
#include "app/model/jobs.h"
#include "app/model/timers.h"
#include "support/test.h"

#ifndef _WIN32
#include "support/fake_slack_server.h"
#endif

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

using model::Jobs;

TEST("jobs: begin and end keep the running list, oldest first") {
    Jobs j;
    CHECK(j.count() == 0);
    const int a = j.begin("Downloading a.csv");
    const int b = j.begin(""); // counted, not listed
    const int c = j.begin("Copying photo.png");
    CHECK(a != b && b != c);
    CHECK(j.count() == 3);
    auto d = j.descriptions();
    REQUIRE(d.size() == 2);
    CHECK_STR(d[0], "Downloading a.csv");
    CHECK_STR(d[1], "Copying photo.png");
    j.end(a);
    j.end(a);    // twice: nothing
    j.end(9999); // unknown: nothing
    CHECK(j.count() == 2);
    REQUIRE(j.descriptions().size() == 1);
    CHECK_STR(j.descriptions()[0], "Copying photo.png");
    j.end(b);
    j.end(c);
    CHECK(j.count() == 0);
    CHECK(j.descriptions().empty());
    // Ids are never reused.
    CHECK(j.begin("x") > c);
}

TEST("jobs: observers hear every change, and may (un)subscribe from inside one") {
    Jobs                   j;
    int                    calls = 0, late = 0;
    Jobs::ObserverId       self  = 0;
    const Jobs::ObserverId first = j.observe([&] { ++calls; });
    self                         = j.observe([&] {
        j.unobserve(self); // once only
        j.observe([&] { ++late; });
    });
    const int id                 = j.begin("one");
    CHECK(calls == 1);
    CHECK(late == 1); // added during the dispatch: runs in it
    j.end(id);
    CHECK(calls == 2);
    CHECK(late == 2);
    j.end(id); // no change: no call
    CHECK(calls == 2);
    j.unobserve(first);
    j.begin("two");
    CHECK(calls == 2);
    CHECK(late == 3);
    j.unobserve(12345); // unknown: nothing
}

TEST("jobs: the app's registry is one object") {
    CHECK(&model::jobs() == &model::jobs());
}

#ifndef _WIN32
TEST("jobs: runInBackground works on a worker, answers later on the UI thread") {
    plat::App            &app = fakeslack::app();
    const std::thread::id ui  = std::this_thread::get_id();
    std::thread::id       worker;
    std::atomic<bool>     worked{false};
    bool                  answered = false, inside = true;
    int                   result = 0;
    model::runInBackground(
        app,
        [&] {
            worker = std::this_thread::get_id();
            result = 42; // seen by then(): it runs after work() returned
            worked = true;
        },
        [&] {
            CHECK(std::this_thread::get_id() == ui);
            CHECK(result == 42);
            answered = true;
        }
    );
    inside = answered; // never inside the call
    CHECK_FALSE(inside);
    REQUIRE(fakeslack::pumpUntil([&] { return answered; }, 5000));
    CHECK(worked);
    CHECK(worker != ui);
    // Several at once: each answers once.
    int done = 0;
    for (int i = 0; i < 8; ++i)
        model::runInBackground(app, [] { std::this_thread::yield(); }, [&] { ++done; });
    REQUIRE(fakeslack::pumpUntil([&] { return done == 8; }, 5000));
    fakeslack::pumpFor(30);
    CHECK(done == 8);
    // Null work or then: fine.
    bool ran = false;
    model::runInBackground(app, nullptr, [&] { ran = true; });
    model::runInBackground(app, [] {}, nullptr);
    REQUIRE(fakeslack::pumpUntil([&] { return ran; }, 5000));
}

TEST("jobs: runInBackground reuses parked workers; past the cap, calls wait their turn") {
    plat::App &app = fakeslack::app();
    model::setBackgroundIdleRetire(60'000);
    // One after another: the first worker takes them all.
    model::waitBackground();
    int done = 0;
    model::runInBackground(app, [] {}, [&] { ++done; });
    REQUIRE(fakeslack::pumpUntil([&] { return done == 1; }, 5000));
    REQUIRE(fakeslack::pumpUntil([] { return model::backgroundStats().parked > 0; }, 5000));
    const uint64_t            started = model::backgroundStats().started;
    std::set<std::thread::id> threads;
    std::mutex                m;
    for (int i = 0; i < 20; ++i) {
        model::runInBackground(
            app,
            [&] {
                std::lock_guard<std::mutex> lock(m);
                threads.insert(std::this_thread::get_id());
            },
            [&] { ++done; }
        );
        REQUIRE(fakeslack::pumpUntil([&] { return done == 2 + i; }, 5000));
        REQUIRE(fakeslack::pumpUntil([] { return model::backgroundStats().parked > 0; }, 5000));
    }
    CHECK(model::backgroundStats().started == started);
    CHECK(threads.size() <= size_t(model::backgroundStats().workers));
    // Twenty that all block: the cap runs at once, the rest wait for a free
    // worker, oldest first; once done, at most four stay parked.
    const int cap = model::backgroundStats().cap;
    REQUIRE((cap >= 4 && cap <= 8));
    std::atomic<int>  under{0}, most{0};
    std::atomic<bool> release{false};
    int               answered = 0;
    for (int i = 0; i < 20; ++i)
        model::runInBackground(
            app,
            [&] {
                const int n = ++under;
                for (int seen = most; n > seen && !most.compare_exchange_weak(seen, n);)
                    ;
                while (!release)
                    std::this_thread::yield();
                --under;
            },
            [&] { ++answered; }
        );
    REQUIRE(fakeslack::pumpUntil([&] { return under == cap; }, 5000));
    fakeslack::pumpFor(50);
    CHECK(under == cap); // no thread beyond the cap
    CHECK(model::backgroundStats().workers == cap);
    CHECK(answered == 0);
    release = true;
    REQUIRE(fakeslack::pumpUntil([&] { return answered == 20; }, 5000));
    CHECK(most == cap);
    model::waitBackground();
    REQUIRE(fakeslack::pumpUntil([] { return model::backgroundStats().workers <= 4; }, 5000));
    CHECK(model::backgroundStats().parked <= 4);
}

TEST("jobs: idle workers retire; a later call starts a fresh one") {
    plat::App &app = fakeslack::app();
    model::waitBackground();
    model::setBackgroundIdleRetire(20); // the parked ones too
    REQUIRE(fakeslack::pumpUntil([] { return model::backgroundStats().workers == 0; }, 5000));
    const uint64_t started = model::backgroundStats().started;
    bool           ran     = false;
    model::runInBackground(app, [] {}, [&] { ran = true; });
    REQUIRE(fakeslack::pumpUntil([&] { return ran; }, 5000));
    CHECK(model::backgroundStats().started == started + 1);
    REQUIRE(fakeslack::pumpUntil([] { return model::backgroundStats().workers == 0; }, 5000));
    model::setBackgroundIdleRetire(30'000);
}
#endif

#ifndef _WIN32
TEST("timers: one-shots forget themselves; the owner cancels the rest") {
    plat::App &app   = fakeslack::app();
    int        fired = 0;
    {
        model::OneShotTimers t(app);
        t.after(0, [&] { ++fired; });
        t.after(-5, [&] { ++fired; }); // < 0 is 0
        t.after(60'000, [&] { fired += 100; });
        CHECK(t.pending() == 3);
        REQUIRE(fakeslack::pumpUntil([&] { return fired == 2; }, 5000));
        CHECK(t.pending() == 1); // the two that fired are gone
    } // the owner goes: the minute-long one is cancelled
    fakeslack::pumpFor(30);
    CHECK(fired == 2);
}

TEST("timers: postWhileAlive drops the call once its owner is gone") {
    plat::App &app  = fakeslack::app();
    int        ran  = 0;
    auto       flag = std::make_shared<bool>(true);
    model::postWhileAlive(app, flag, [&] { ++ran; });
    model::postWhileAlive(app, flag, [&] { ran += 10; });
    *flag      = false; // the owner goes before the loop turns: neither runs
    auto owned = std::make_shared<char>(0);
    model::postWhileAlive(app, std::weak_ptr<void>(owned), [&] { ran += 100; });
    auto gone = std::make_shared<char>(0);
    model::postWhileAlive(app, std::weak_ptr<void>(gone), [&] { ran += 1000; });
    gone.reset();
    CHECK(ran == 0); // never inside the call
    fakeslack::pumpFor(30);
    CHECK(ran == 100);
}
#endif
