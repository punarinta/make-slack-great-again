// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>
#include "util/emoji.h"

// ── Emoji::fromName ───────────────────────────────────────────────────────────

TEST_CASE("fromName common names resolve to unicode", "[emoji][fromName]") {
    CHECK(Emoji::fromName("palm_tree") == "🌴");
    CHECK(Emoji::fromName("wave")      == "👋");
    CHECK(Emoji::fromName("fire")      == "🔥");
    CHECK(Emoji::fromName("tada")      == "🎉");
    CHECK(Emoji::fromName("baby")      == "👶");
}

TEST_CASE("fromName Slack aliases resolve correctly", "[emoji][fromName]") {
    // +1 and thumbsup are both aliases for the same emoji
    CHECK(Emoji::fromName("+1")       == "👍");
    CHECK(Emoji::fromName("thumbsup") == "👍");
    CHECK(Emoji::fromName("-1")       == "👎");
}

TEST_CASE("fromName person_feeding_baby resolves — regression for ZWJ sequence", "[emoji][fromName]") {
    // This was the bug: person_feeding_baby is a ZWJ sequence (U+1F9D1 U+200D U+1F37C)
    // and must be in the table so status emoji doesn't fall back to literal text.
    const QString result = Emoji::fromName("person_feeding_baby");
    CHECK(!result.startsWith(':'));
    CHECK(result.length() > 1); // ZWJ sequences have multiple code units
}

TEST_CASE("fromName unknown name falls back to :name:", "[emoji][fromName]") {
    CHECK(Emoji::fromName("not_a_real_emoji_xyzzy") == ":not_a_real_emoji_xyzzy:");
    CHECK(Emoji::fromName("") == "::");
}

// ── Emoji::expandCodes ────────────────────────────────────────────────────────

TEST_CASE("expandCodes no colons returns string unchanged", "[emoji][expandCodes]") {
    CHECK(Emoji::expandCodes("Hello world") == "Hello world");
    CHECK(Emoji::expandCodes("")            == "");
    CHECK(Emoji::expandCodes("Petter Kristoffersson") == "Petter Kristoffersson");
}

TEST_CASE("expandCodes single code at start", "[emoji][expandCodes]") {
    CHECK(Emoji::expandCodes(":palm_tree:") == "🌴");
}

TEST_CASE("expandCodes single code inline", "[emoji][expandCodes]") {
    CHECK(Emoji::expandCodes("Kamil :palm_tree:") == "Kamil 🌴");
}

TEST_CASE("expandCodes multiple codes", "[emoji][expandCodes]") {
    const QString result = Emoji::expandCodes(":wave: hello :fire:");
    CHECK(result == "👋 hello 🔥");
}

TEST_CASE("expandCodes person_feeding_baby inline — the sidebar regression", "[emoji][expandCodes]") {
    // A user whose display name is `:person_feeding_baby:` should render as the
    // actual emoji, not as a long literal string that squeezes the name out of view.
    const QString result = Emoji::expandCodes(":person_feeding_baby:");
    CHECK(!result.startsWith(':'));
}

TEST_CASE("expandCodes unknown code is left unchanged", "[emoji][expandCodes]") {
    CHECK(Emoji::expandCodes(":not_a_real_emoji:") == ":not_a_real_emoji:");
}

TEST_CASE("expandCodes known and unknown codes mixed", "[emoji][expandCodes]") {
    const QString result = Emoji::expandCodes(":wave: :not_real: :fire:");
    CHECK(result == "👋 :not_real: 🔥");
}

TEST_CASE("expandCodes code with space inside is not a code", "[emoji][expandCodes]") {
    // Our scanner stops at spaces, so `:hello world:` must not consume `world:`.
    const QString result = Emoji::expandCodes(":hello world:");
    CHECK(result == ":hello world:");
}

TEST_CASE("expandCodes adjacent colons are not treated as codes", "[emoji][expandCodes]") {
    CHECK(Emoji::expandCodes("::") == "::");
}
