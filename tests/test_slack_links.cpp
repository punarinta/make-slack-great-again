// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests for SlackLinks::parseMessageLink() — recognising a permalink to a
// message ("…/archives/C123/p1786008939071009") is what turns it into a
// clickable chip instead of a raw URL, so both the accept and the reject side
// matter: a false positive produces a chip that navigates nowhere.

#include <catch2/catch_test_macros.hpp>

#include "util/slack_links.h"

TEST_CASE("parseMessageLink reads conversation and ts from a permalink", "[links]") {
    const auto ref = SlackLinks::parseMessageLink(
        "https://cityteam.slack.com/archives/C6HQE8G0Z/p1786008939071009"
    );
    REQUIRE(ref.isValid());
    CHECK(ref.host == "cityteam.slack.com");
    CHECK(ref.conv == "C6HQE8G0Z");
    CHECK(ref.ts == "1786008939.071009"); // the dot Slack drops is restored
    CHECK(ref.threadTs.isEmpty());
}

TEST_CASE("parseMessageLink picks up the thread root of a reply link", "[links]") {
    const auto ref = SlackLinks::parseMessageLink(
        "https://cityteam.slack.com/archives/C1/p1786008939071009"
        "?thread_ts=1786008900.000100&cid=C1"
    );
    REQUIRE(ref.isValid());
    CHECK(ref.ts == "1786008939.071009");
    CHECK(ref.threadTs == "1786008900.000100");
}

TEST_CASE("parseMessageLink ignores a thread_ts that is the message itself", "[links]") {
    // Slack puts thread_ts on a thread ROOT's permalink too; that is not a reply
    // and must not send the click into the thread panel.
    const auto ref = SlackLinks::parseMessageLink(
        "https://cityteam.slack.com/archives/C1/p1786008939071009"
        "?thread_ts=1786008939.071009&cid=C1"
    );
    REQUIRE(ref.isValid());
    CHECK(ref.threadTs.isEmpty());
}

TEST_CASE("parseMessageLink accepts DM and private-channel ids", "[links]") {
    CHECK(
        SlackLinks::parseMessageLink("https://t.slack.com/archives/D0AB12CD/p1700000000000100")
            .conv == "D0AB12CD"
    );
    CHECK(
        SlackLinks::parseMessageLink("https://t.slack.com/archives/G0AB12CD/p1700000000000100")
            .conv == "G0AB12CD"
    );
}

TEST_CASE("parseMessageLink rejects everything that is not a message link", "[links]") {
    // Not Slack at all.
    CHECK_FALSE(
        SlackLinks::parseMessageLink("https://example.com/archives/C1/p1700000000000100").isValid()
    );
    // A lookalike host suffix ("notslack.com" ends with "slack.com" only as a
    // substring — the check is on the dotted label).
    CHECK_FALSE(
        SlackLinks::parseMessageLink("https://notslack.com/archives/C1/p1700000000000100").isValid()
    );
    // A channel link with no message.
    CHECK_FALSE(SlackLinks::parseMessageLink("https://t.slack.com/archives/C1").isValid());
    // The client deep-link form is not a permalink.
    CHECK_FALSE(SlackLinks::parseMessageLink("https://app.slack.com/client/T1/C1").isValid());
    // Lowercase path segment where the conversation id belongs.
    CHECK_FALSE(
        SlackLinks::parseMessageLink("https://t.slack.com/archives/search/p123456789012345")
            .isValid()
    );
    // p-token that isn't a timestamp.
    CHECK_FALSE(
        SlackLinks::parseMessageLink("https://t.slack.com/archives/C1/pabcdefghijklmnop").isValid()
    );
    CHECK_FALSE(SlackLinks::parseMessageLink("https://t.slack.com/archives/C1/p12345").isValid());
    CHECK_FALSE(SlackLinks::parseMessageLink("").isValid());
}

TEST_CASE("messagePermalink rebuilds the URL it was parsed from", "[links]") {
    const QString url = "https://cityteam.slack.com/archives/C1/p1786008939071009";
    CHECK(SlackLinks::messagePermalink(SlackLinks::parseMessageLink(url)) == url);

    const QString reply = "https://cityteam.slack.com/archives/C1/p1786008939071009"
                          "?thread_ts=1786008900.000100&cid=C1";
    CHECK(SlackLinks::messagePermalink(SlackLinks::parseMessageLink(reply)) == reply);
}
