// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>
#include "text/mrkdwn_parser.h"

// Asserts exactly one entity with the given properties.
static void checkOne(const TextWithEntities &r,
                     const QString &text,
                     EntityType type, int offset, int length,
                     const QString &data = {})
{
    CHECK(r.text == text);
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].type == type);
    CHECK(r.entities[0].offset == offset);
    CHECK(r.entities[0].length == length);
    CHECK(r.entities[0].data == data);
}

TEST_CASE("empty string", "[mrkdwn]") {
    auto r = MrkdwnParser::parse("");
    CHECK(r.text.isEmpty());
    CHECK(r.entities.empty());
}

TEST_CASE("plain text passes through unchanged", "[mrkdwn]") {
    auto r = MrkdwnParser::parse("hello world");
    CHECK(r.text == "hello world");
    CHECK(r.entities.empty());
}

TEST_CASE("bold *text*", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("*bold*"), "bold", EntityType::Bold, 0, 4);
}

TEST_CASE("italic _text_", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("_italic_"), "italic", EntityType::Italic, 0, 6);
}

TEST_CASE("strikethrough ~text~", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("~strike~"), "strike", EntityType::Strike, 0, 6);
}

TEST_CASE("inline code", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("`code`"), "code", EntityType::Code, 0, 4);
}

TEST_CASE("code fence no language hint", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("```hello```"), "hello", EntityType::Pre, 0, 5);
}

TEST_CASE("code fence with language hint skips first line", "[mrkdwn]") {
    auto r = MrkdwnParser::parse("```python\nprint('x')\n```");
    CHECK(r.text == "print('x')\n");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].type == EntityType::Pre);
    CHECK(r.entities[0].offset == 0);
    CHECK(r.entities[0].length == 11);
}

TEST_CASE("user mention no label", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<@U123ABC>"), "@U123ABC",
             EntityType::UserMention, 0, 8, "U123ABC");
}

TEST_CASE("user mention with label", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<@U123|alice>"), "alice",
             EntityType::UserMention, 0, 5, "U123");
}

TEST_CASE("channel mention", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<#C456|general>"), "#general",
             EntityType::ChannelMention, 0, 8, "C456");
}

TEST_CASE("link no label", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<https://example.com>"), "https://example.com",
             EntityType::Link, 0, 19, "https://example.com");
}

TEST_CASE("link with label", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<https://example.com|click here>"), "click here",
             EntityType::Link, 0, 10, "https://example.com");
}

TEST_CASE("<!here> broadcast", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<!here>"), "@here", EntityType::HereCommand, 0, 5);
}

TEST_CASE("<!channel> broadcast", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse("<!channel>"), "@channel", EntityType::ChannelCommand, 0, 8);
}

TEST_CASE("emoji :name:", "[mrkdwn]") {
    checkOne(MrkdwnParser::parse(":rocket:"), ":rocket:", EntityType::Emoji, 0, 8, "rocket");
}

TEST_CASE("single-line blockquote", "[mrkdwn]") {
    auto r = MrkdwnParser::parse("> hello");
    // Parser appends \n after blockquote span to ensure visual line-break in HTML.
    CHECK(r.text == "hello\n");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].type == EntityType::Blockquote);
    CHECK(r.entities[0].offset == 0);
    CHECK(r.entities[0].length == 5);
}

TEST_CASE("multi-line blockquote", "[mrkdwn]") {
    auto r = MrkdwnParser::parse("> line1\n> line2");
    CHECK(r.text == "line1\nline2\n");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].type == EntityType::Blockquote);
    CHECK(r.entities[0].offset == 0);
    CHECK(r.entities[0].length == 11);
}

TEST_CASE("entity mid-sentence has correct offset and length", "[mrkdwn]") {
    auto r = MrkdwnParser::parse("hello *world* today");
    CHECK(r.text == "hello world today");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].type == EntityType::Bold);
    CHECK(r.entities[0].offset == 6);
    CHECK(r.entities[0].length == 5);
}

TEST_CASE("unmatched delimiter passes through as plain text", "[mrkdwn]") {
    // '*' at end of input has no closing partner — must not create a spurious entity
    auto r = MrkdwnParser::parse("price: $5*2");
    CHECK(r.text == "price: $5*2");
    CHECK(r.entities.empty());
}

TEST_CASE("emoji name with space is not an emoji", "[mrkdwn]") {
    auto r = MrkdwnParser::parse(":not valid:");
    CHECK(r.text == ":not valid:");
    CHECK(r.entities.empty());
}
