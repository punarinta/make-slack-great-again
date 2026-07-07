// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for the "Summarize down" prompt builder (llm/discussion_summary).
#include <catch2/catch_test_macros.hpp>

#include "llm/discussion_summary.h"

using DiscussionSummary::buildRequest;
using DiscussionSummary::Entry;
using DiscussionSummary::modelForProvider;

TEST_CASE("modelForProvider picks the lightest modern model per vendor") {
    CHECK(modelForProvider("anthropic") == "claude-haiku-4-5");
    CHECK(modelForProvider("openai") == "gpt-5.4-nano");
    // Unknown provider → empty → the provider's own default applies.
    CHECK(modelForProvider("acme").isEmpty());
}

TEST_CASE("buildRequest shapes the transcript") {
    const std::vector<Entry> entries = {
        {"Alice", "Shall we ship on Friday?", false},
        {"Bob", "Yes, pending QA", true},
    };
    const auto req = buildRequest(entries, "en");

    REQUIRE(req.messages.size() == 1);
    const QString &user = req.messages.first().text;
    CHECK(user.contains("Alice: Shall we ship on Friday?"));
    // Thread replies are indented and marked.
    CHECK(user.contains("↳ Bob: Yes, pending QA"));
    CHECK(req.maxTokens > 0);
    // The report format is pinned in the system prompt.
    CHECK(req.system.contains("Goal:"));
    CHECK(req.system.contains("Key decisions:"));
    CHECK(req.system.contains("Open questions / next steps:"));
    CHECK(req.system.contains("100 words"));
    // Tone: plain language, not report-speak.
    CHECK(req.system.contains("plain, everyday language"));
}

TEST_CASE("buildRequest names the output language") {
    CHECK(buildRequest({{"A", "hi", false}}, "ja").system.contains("Japanese"));
    CHECK(buildRequest({{"A", "hi", false}}, "sv").system.contains("Swedish"));
    // Unknown / empty code falls back to English.
    CHECK(buildRequest({{"A", "hi", false}}, "??").system.contains("English"));
    CHECK(buildRequest({{"A", "hi", false}}, "").system.contains("English"));
}

TEST_CASE("language instruction is repeated after the transcript") {
    // Small models drift into the transcript's language when the only language
    // instruction is a system line far behind the content — the request must
    // restate it at the end of the user message, after the transcript.
    const auto     req  = buildRequest({{"A", "привет", false}}, "en");
    const QString &user = req.messages.first().text;
    const auto     pos  = user.lastIndexOf("Write the summary in English");
    REQUIRE(pos >= 0);
    CHECK(pos > user.indexOf(QString::fromUtf8("привет"))); // after the transcript
}

TEST_CASE("buildRequest flattens newlines and caps a single huge message") {
    const QString  huge(5000, QChar('x'));
    const auto     req  = buildRequest({{"A", "line1\nline2", false}, {"B", huge, false}}, "en");
    const QString &user = req.messages.first().text;
    CHECK(user.contains("A: line1 line2"));
    // The pasted-log message was truncated with an ellipsis, not sent whole.
    CHECK(!user.contains(huge));
    CHECK(user.contains(QString(QChar(0x2026))));
}

TEST_CASE("buildRequest drops the middle of an over-long span") {
    std::vector<Entry> entries;
    for (int i = 0; i < 500; ++i)
        entries.push_back(
            {QString("User%1").arg(i), QString("message %1 ").arg(i).repeated(20), false}
        );
    const auto     req  = buildRequest(entries, "en");
    const QString &user = req.messages.first().text;
    CHECK(user.contains("messages omitted"));
    // Head and tail survive; the transcript stays bounded.
    CHECK(user.contains("User0:"));
    CHECK(user.contains("User499:"));
    CHECK(user.size() < 30000);
}
