// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "backend/domain.h"

// Covers the pure huddle-notification policy (shouldNotifyHuddleStart). The
// dedup, settings and capability gates live in MainWindow and are exercised
// manually; this nails down the conversation/participant decision.

namespace {

Conversation makeConv(ConvKind kind, NotificationLevel level = NotificationLevel::Default) {
    Conversation c;
    c.id         = ConversationId{"C1"};
    c.kind       = kind;
    c.isMember   = true;
    c.isMuted    = false;
    c.notifLevel = level;
    return c;
}

const UserId kMe{"U_ME"};
const UserId kOther{"U_OTHER"};

} // namespace

TEST_CASE("DM huddle always notifies regardless of default level") {
    const auto dm = makeConv(ConvKind::Im);
    CHECK(shouldNotifyHuddleStart(dm, {kOther}, kMe, NotificationLevel::All));
    CHECK(shouldNotifyHuddleStart(dm, {kOther}, kMe, NotificationLevel::Mentions));
}

TEST_CASE("MPDM huddle always notifies") {
    const auto mpim = makeConv(ConvKind::Mpim);
    CHECK(shouldNotifyHuddleStart(mpim, {kOther}, kMe, NotificationLevel::Mentions));
}

TEST_CASE("Channel huddle notifies only when effective level is All") {
    SECTION("default All") {
        const auto ch = makeConv(ConvKind::PublicChannel, NotificationLevel::Default);
        CHECK(shouldNotifyHuddleStart(ch, {kOther}, kMe, NotificationLevel::All));
    }
    SECTION("default Mentions → suppressed") {
        const auto ch = makeConv(ConvKind::PublicChannel, NotificationLevel::Default);
        CHECK_FALSE(shouldNotifyHuddleStart(ch, {kOther}, kMe, NotificationLevel::Mentions));
    }
    SECTION("per-conv All overrides a Mentions default") {
        const auto ch = makeConv(ConvKind::PublicChannel, NotificationLevel::All);
        CHECK(shouldNotifyHuddleStart(ch, {kOther}, kMe, NotificationLevel::Mentions));
    }
    SECTION("per-conv Mentions overrides an All default") {
        const auto ch = makeConv(ConvKind::PublicChannel, NotificationLevel::Mentions);
        CHECK_FALSE(shouldNotifyHuddleStart(ch, {kOther}, kMe, NotificationLevel::All));
    }
}

TEST_CASE("A huddle I'm already in never notifies") {
    auto dm = makeConv(ConvKind::Im);
    CHECK_FALSE(shouldNotifyHuddleStart(dm, {kMe}, kMe, NotificationLevel::All));
    CHECK_FALSE(shouldNotifyHuddleStart(dm, {kOther, kMe}, kMe, NotificationLevel::All));
}

TEST_CASE("Muted or non-member conversations never notify") {
    SECTION("muted") {
        auto dm    = makeConv(ConvKind::Im);
        dm.isMuted = true;
        CHECK_FALSE(shouldNotifyHuddleStart(dm, {kOther}, kMe, NotificationLevel::All));
    }
    SECTION("level Mute") {
        const auto ch = makeConv(ConvKind::PublicChannel, NotificationLevel::Mute);
        CHECK_FALSE(shouldNotifyHuddleStart(ch, {kOther}, kMe, NotificationLevel::All));
    }
    SECTION("not a member") {
        auto ch     = makeConv(ConvKind::PublicChannel, NotificationLevel::All);
        ch.isMember = false;
        CHECK_FALSE(shouldNotifyHuddleStart(ch, {kOther}, kMe, NotificationLevel::All));
    }
}

TEST_CASE("Empty self id (not yet loaded) still applies the other gates") {
    const auto dm = makeConv(ConvKind::Im);
    CHECK(shouldNotifyHuddleStart(dm, {kOther}, UserId{}, NotificationLevel::All));
}
