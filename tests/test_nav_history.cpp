// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/nav_history.h"

#include <catch2/catch_test_macros.hpp>

namespace {

NavLocation loc(const char *team, const char *conv) {
    return {QString::fromLatin1(team), ConversationId{QString::fromLatin1(conv)}};
}

const NavHistory::Validator acceptAll = [](const NavLocation &) { return true; };

} // namespace

TEST_CASE("empty history cannot navigate") {
    NavHistory h;
    CHECK_FALSE(h.canGoBack());
    CHECK_FALSE(h.canGoForward());
    CHECK_FALSE(h.goBack(acceptAll).has_value());
    CHECK_FALSE(h.goForward(acceptAll).has_value());
}

TEST_CASE("back returns previously opened conversations in reverse order") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T1", "C2"));
    h.recordOpen(loc("T1", "C3"));

    CHECK(h.goBack(acceptAll) == loc("T1", "C2"));
    CHECK(h.goBack(acceptAll) == loc("T1", "C1"));
    CHECK_FALSE(h.goBack(acceptAll).has_value());
}

TEST_CASE("going back keeps the forward stack; forward replays it") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T1", "C2"));
    h.recordOpen(loc("T1", "C3"));

    REQUIRE(h.goBack(acceptAll) == loc("T1", "C2"));
    REQUIRE(h.goBack(acceptAll) == loc("T1", "C1"));
    CHECK(h.canGoForward());
    CHECK(h.goForward(acceptAll) == loc("T1", "C2"));
    CHECK(h.goForward(acceptAll) == loc("T1", "C3"));
    CHECK_FALSE(h.goForward(acceptAll).has_value());
}

TEST_CASE("a direct open discards the forward stack, like editor undo/redo") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T1", "C2"));
    h.recordOpen(loc("T1", "C3"));

    REQUIRE(h.goBack(acceptAll) == loc("T1", "C2"));
    h.recordOpen(loc("T1", "C4")); // manual click while forward stack is non-empty

    CHECK_FALSE(h.canGoForward());
    CHECK(h.goBack(acceptAll) == loc("T1", "C2"));
    CHECK(h.goBack(acceptAll) == loc("T1", "C1"));
}

TEST_CASE("re-opening the current conversation records nothing") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T1", "C1"));
    CHECK_FALSE(h.canGoBack());
}

TEST_CASE("setCurrent does not touch the stacks") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T1", "C2"));
    REQUIRE(h.goBack(acceptAll) == loc("T1", "C1"));

    h.setCurrent(loc("T1", "C1")); // the jump landed
    CHECK(h.canGoForward());
    CHECK(h.goForward(acceptAll) == loc("T1", "C2"));
}

TEST_CASE("history spans workspaces") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T2", "C2"));
    h.recordOpen(loc("T1", "C3"));

    CHECK(h.goBack(acceptAll) == loc("T2", "C2"));
    CHECK(h.goBack(acceptAll) == loc("T1", "C1"));
    CHECK(h.goForward(acceptAll) == loc("T2", "C2"));
}

TEST_CASE("depth is capped at 32, dropping the oldest entries") {
    NavHistory h;
    for (int i = 0; i < 40; ++i)
        h.recordOpen(loc("T1", qPrintable(QString("C%1").arg(i))));

    int steps = 0;
    while (h.goBack(acceptAll))
        ++steps;
    CHECK(steps == NavHistory::kMaxDepth);
    // Oldest reachable entry is C7: C0..C6 fell off the 32-deep back stack.
    CHECK(h.current() == loc("T1", "C7"));
}

TEST_CASE("validator-rejected entries are dropped and skipped") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T2", "C2"));
    h.recordOpen(loc("T1", "C3"));

    const NavHistory::Validator rejectT2 = [](const NavLocation &l) { return l.teamId != "T2"; };
    CHECK(h.goBack(rejectT2) == loc("T1", "C1"));
    // The rejected entry is gone for good — forward goes straight to C3.
    CHECK(h.goForward(acceptAll) == loc("T1", "C3"));
}

TEST_CASE("purgeTeam removes a logged-out workspace from both stacks") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T2", "C2"));
    h.recordOpen(loc("T1", "C3"));
    h.recordOpen(loc("T2", "C4"));
    REQUIRE(h.goBack(acceptAll) == loc("T1", "C3")); // put a T2 entry on the forward stack

    h.purgeTeam("T2");
    CHECK_FALSE(h.canGoForward());
    CHECK(h.goBack(acceptAll) == loc("T1", "C1"));
}

TEST_CASE("purging the current location's team clears it") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.purgeTeam("T1");
    CHECK_FALSE(h.current().valid());
}

TEST_CASE("navigating to an entry equal to the current location is skipped") {
    NavHistory h;
    h.recordOpen(loc("T1", "C1"));
    h.recordOpen(loc("T1", "C2"));
    h.setCurrent(loc("T1", "C1")); // drifted back without a recorded jump
    CHECK_FALSE(h.goBack(acceptAll).has_value());
}
