// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Functional tests for the app-wide background-task registry that drives the
// conv-footer spinner. Pure logic — no UI, no event loop: countChanged is a
// direct (synchronous) signal, so a plain connected lambda captures every
// emission. BackgroundTasks is a process-wide singleton, so every case snapshots
// the baseline count and cleans up after itself rather than assuming it starts
// at zero.
#include <catch2/catch_test_macros.hpp>

#include "util/background_tasks.h"

#include <QObject>

#include <vector>

TEST_CASE("BackgroundTasks tracks the running count", "[background_tasks]") {
    auto     &bt   = BackgroundTasks::instance();
    const int base = bt.count();

    std::vector<int> seen; // every value carried by countChanged, in order
    QObject          ctx;  // scopes the connection to this case run
    QObject::connect(&bt, &BackgroundTasks::countChanged, &ctx, [&](int c) { seen.push_back(c); });

    SECTION("begin increments + emits, end decrements + emits") {
        const int a = bt.begin();
        CHECK(bt.count() == base + 1);
        REQUIRE(seen.size() == 1);
        CHECK(seen.back() == base + 1);

        const int b = bt.begin();
        CHECK(bt.count() == base + 2);
        CHECK(seen.back() == base + 2);

        bt.end(a);
        CHECK(bt.count() == base + 1);
        CHECK(seen.back() == base + 1);

        bt.end(b);
        CHECK(bt.count() == base);
        CHECK(seen.back() == base);
        CHECK(seen.size() == 4); // exactly one emission per begin/end
    }

    SECTION("ids are unique and tasks are tracked independently") {
        const int a = bt.begin();
        const int b = bt.begin();
        const int c = bt.begin();
        CHECK(a != b);
        CHECK(b != c);
        CHECK(a != c);
        CHECK(bt.count() == base + 3);

        // Ending the middle task leaves the other two running.
        bt.end(b);
        CHECK(bt.count() == base + 2);

        bt.end(a);
        bt.end(c);
        CHECK(bt.count() == base);
    }

    SECTION("end with an unknown id is a no-op and emits nothing") {
        const auto before = seen.size();
        bt.end(-12345);
        bt.end(0);
        CHECK(bt.count() == base);
        CHECK(seen.size() == before);
    }

    SECTION("end is idempotent — ending the same id twice counts once") {
        const int a = bt.begin();
        bt.end(a);
        const int  count1 = bt.count();
        const auto emits1 = seen.size();

        bt.end(a); // already ended — must not double-decrement or re-emit
        CHECK(bt.count() == count1);
        CHECK(bt.count() == base);
        CHECK(seen.size() == emits1);
    }

    SECTION("a reused-looking id from a finished task does not collide") {
        // ids are monotonic, never recycled, so a stale end() can never take down
        // a later, unrelated task.
        const int a = bt.begin();
        bt.end(a);
        const int b = bt.begin();
        CHECK(b != a);
        bt.end(a); // stale: must not touch b
        CHECK(bt.count() == base + 1);
        bt.end(b);
        CHECK(bt.count() == base);
    }
}
