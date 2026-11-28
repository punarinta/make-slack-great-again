// The request behind the "Summarize down" message action: a brief, plain-language recap of a span
// of a discussion, written in the user's own language. Pure — no network, no Store — so the
// transcript and prompt shaping are unit-tested.
#pragma once

#include "app/llm/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace llm {

// One transcript line: a message, or a note the transcript adds itself.
// Thread replies are indented under their root.
struct SummaryEntry {
    std::string author;
    std::string text;
    bool        threadReply = false;
};

// languageCode: ISO 639-1 (Service::language()); unknown → English.
Request summaryRequest(const std::vector<SummaryEntry> &entries, std::string_view languageCode);
// The languages AI features can address the user in (ISO 639-1, in the
// order Settings lists them); their names are spell's language table.
extern const char *const kAiLanguages[16];
// The language's English name ("ja" → "Japanese"): the model is told in
// English, with the language named, which works better than a bare code.
const char              *languageName(std::string_view code);

} // namespace llm
