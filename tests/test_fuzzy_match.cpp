// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for Fuzzy::score — the subsequence matcher behind the Ctrl/Cmd+K
// switcher (issue #60):
//   - Subsequence semantics: characters in order, gaps allowed, order matters
//   - Case-insensitive; empty query matches everything
//   - Ranking: substring > scattered, prefix > mid-word, word-start > mid-word,
//     a tighter alignment > a looser one
#include <catch2/catch_test_macros.hpp>

#include "util/fuzzy_match.h"

static double s(const char *q, const char *h) {
    auto r = Fuzzy::score(QString::fromUtf8(q), QString::fromUtf8(h));
    REQUIRE(r.has_value());
    return *r;
}

// ── Match / no match ──────────────────────────────────────────────────────────

TEST_CASE("Fuzzy: the issue #60 case — xdg finds xd-general", "[fuzzy][match]") {
    CHECK(Fuzzy::matches("xdg", "xd-general"));
}

TEST_CASE("Fuzzy: a plain substring matches", "[fuzzy][match]") {
    CHECK(Fuzzy::matches("gener", "xd-general"));
    CHECK(Fuzzy::matches("xd-general", "xd-general"));
}

TEST_CASE("Fuzzy: characters must appear in order", "[fuzzy][match]") {
    CHECK_FALSE(Fuzzy::matches("gdx", "xd-general"));
    CHECK_FALSE(Fuzzy::matches("lareneg", "general"));
}

TEST_CASE("Fuzzy: a character missing from the haystack fails", "[fuzzy][match]") {
    CHECK_FALSE(Fuzzy::matches("xdgz", "xd-general"));
    CHECK_FALSE(Fuzzy::matches("bob", "general"));
}

TEST_CASE("Fuzzy: a query longer than the haystack fails", "[fuzzy][match]") {
    CHECK_FALSE(Fuzzy::matches("generally", "general"));
}

TEST_CASE("Fuzzy: repeated query characters need repeated haystack characters", "[fuzzy][match]") {
    CHECK(Fuzzy::matches("ll", "hello"));
    CHECK_FALSE(Fuzzy::matches("lll", "hello"));
}

TEST_CASE("Fuzzy: case-insensitive both ways", "[fuzzy][match]") {
    CHECK(Fuzzy::matches("BB", "Bob Builder"));
    CHECK(Fuzzy::matches("bb", "BOB BUILDER"));
}

TEST_CASE("Fuzzy: empty query matches anything with score 0", "[fuzzy][match]") {
    CHECK(Fuzzy::score("", "anything") == 0.0);
    CHECK(Fuzzy::score("", "") == 0.0);
}

TEST_CASE("Fuzzy: empty haystack matches only the empty query", "[fuzzy][match]") {
    CHECK_FALSE(Fuzzy::matches("a", ""));
}

TEST_CASE("Fuzzy: non-ASCII is matched by code point", "[fuzzy][match]") {
    CHECK(Fuzzy::matches("ой", "Бой"));
    CHECK(Fuzzy::matches("БЙ", "бой")); // case-folded
}

// ── Ranking ───────────────────────────────────────────────────────────────────

TEST_CASE("Fuzzy: a verbatim substring outranks a scattered alignment", "[fuzzy][rank]") {
    CHECK(s("gen", "general") > s("gen", "go-engineering"));
}

TEST_CASE("Fuzzy: a prefix match outranks a mid-name match", "[fuzzy][rank]") {
    CHECK(s("des", "design") > s("des", "web-design"));
}

TEST_CASE("Fuzzy: word starts outrank mid-word letters", "[fuzzy][rank]") {
    // x·d·g all start words in "xd-general" (x at 0, g after '-') …
    // … but in "exdgy" they are buried mid-word.
    CHECK(s("xdg", "xd-general") > s("xdg", "exdgy-stuff"));
    // Initials of a person's name beat the same letters inside one word.
    CHECK(s("bb", "Bob Builder") > s("bb", "cabbage"));
}

TEST_CASE("Fuzzy: fewer skipped characters wins between equal alignments", "[fuzzy][rank]") {
    CHECK(s("xg", "xd-general") > s("xg", "xd-long-general"));
}

TEST_CASE("Fuzzy: leftover trailing characters do not change the score", "[fuzzy][rank]") {
    // Same alignment, different tails → a tie, left to the caller's ordering.
    CHECK(s("des", "design-review") == s("des", "design-backend"));
    CHECK(s("design-", "design-review") == s("design-", "design-backend"));
}

TEST_CASE("Fuzzy: an exact match is the best possible score for that query", "[fuzzy][rank]") {
    const double exact = s("general", "general");
    CHECK(exact > s("general", "xd-general"));
    CHECK(exact > s("general", "general-announcements"));
}
