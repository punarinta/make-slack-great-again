// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "backend/domain.h"

// Covers the notification click-target token codec (encodeNotifToken /
// decodeNotifToken). The token round-trips through the OS notifier and, on
// Windows, an msga:// activation; a thread reply must carry its root so the
// click opens the thread the reply lives in rather than the channel it's absent
// from ("notification, but nothing there"). The 0x1f field splitting is the
// easy-to-break part, so it's pinned here.

TEST_CASE("plain-message token round-trips with no thread root") {
    const QString token = encodeNotifToken("T1", ConversationId{"C1"}, Ts{});
    const auto    t     = decodeNotifToken(token);
    REQUIRE(t.has_value());
    CHECK(t->teamId == "T1");
    CHECK(t->conv == ConversationId{"C1"});
    CHECK(t->threadRoot.isEmpty());
}

TEST_CASE("thread-reply token carries the root so the click opens the thread") {
    const QString token = encodeNotifToken("T1", ConversationId{"C1"}, Ts{"1699999999.000100"});
    const auto    t     = decodeNotifToken(token);
    REQUIRE(t.has_value());
    CHECK(t->teamId == "T1");
    CHECK(t->conv == ConversationId{"C1"});
    CHECK(t->threadRoot == Ts{"1699999999.000100"});
}

TEST_CASE("a root ts (which itself contains a '.') is not split into the conv id") {
    // Regression against a naive parser: the root ts has a dot but never a 0x1f,
    // so it stays whole and never bleeds into the conversation field.
    const auto t = decodeNotifToken(encodeNotifToken("T1", ConversationId{"D5"}, Ts{"12.34"}));
    REQUIRE(t.has_value());
    CHECK(t->conv == ConversationId{"D5"});
    CHECK(t->threadRoot == Ts{"12.34"});
}

TEST_CASE("malformed tokens decode to nothing rather than opening a blank chat") {
    CHECK_FALSE(decodeNotifToken("").has_value());                          // empty
    CHECK_FALSE(decodeNotifToken("T1").has_value());                        // team only, no conv
    CHECK_FALSE(decodeNotifToken(QString("T1") + QChar(0x1f)).has_value()); // empty conv
}

TEST_CASE("reminder token on a plain message carries msgTs with an empty root") {
    // The reminded message is not a reply: the third field must be present but
    // empty, so the fourth (msgTs) keeps its position.
    const QString token =
        encodeNotifToken("T1", ConversationId{"C1"}, Ts{}, Ts{"1700000000.000200"});
    const auto t = decodeNotifToken(token);
    REQUIRE(t.has_value());
    CHECK(t->conv == ConversationId{"C1"});
    CHECK(t->threadRoot.isEmpty());
    CHECK(t->msgTs == Ts{"1700000000.000200"});
}

TEST_CASE("reminder token on a thread reply carries both root and msgTs") {
    const QString token = encodeNotifToken(
        "T1", ConversationId{"C1"}, Ts{"1699999999.000100"}, Ts{"1700000000.000200"}
    );
    const auto t = decodeNotifToken(token);
    REQUIRE(t.has_value());
    CHECK(t->threadRoot == Ts{"1699999999.000100"});
    CHECK(t->msgTs == Ts{"1700000000.000200"});
}

TEST_CASE("pre-reminder tokens (no fourth field) still decode with empty msgTs") {
    const auto t = decodeNotifToken(encodeNotifToken("T1", ConversationId{"C1"}, Ts{"12.34"}));
    REQUIRE(t.has_value());
    CHECK(t->msgTs.isEmpty());
}
