// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Builds the LLM request for the "Summarize down" message action: a brief,
// plain-language recap of a discussion span, written in the user's native
// language. Structure (bullets/labels) is up to the model, driven by the
// content — no fixed template. Pure functions — no network, no session — so
// the prompt/transcript shaping is unit-testable.
#pragma once

#include "llm_types.h"

#include <QString>
#include <vector>

namespace DiscussionSummary {

// One transcript line: a message, or a note the transcript inserts itself
// (omission markers). Thread replies render indented under their root.
struct Entry {
    QString author;
    QString text;
    bool    threadReply = false;
};

// Summaries are short, low-stakes and potentially frequent, so they run on the
// vendor's lightest modern model instead of the provider's (heavier) default.
// Empty for unknown providers — the provider default applies.
QString modelForProvider(const QString &providerId);

// languageCode: ISO 639-1 code from LlmService::nativeLanguage().
Llm::Request buildRequest(const std::vector<Entry> &entries, const QString &languageCode);

} // namespace DiscussionSummary
