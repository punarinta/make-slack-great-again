#include "app/llm/discussion_summary.h"

#include "app/spell/spell.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"

namespace llm {

namespace {

// Transcript bounds (characters): a pasted log can't eat the budget, and an
// over-long span drops its middle — the head says what the discussion was
// about, the tail holds the conclusions a summary needs most.
constexpr size_t kMaxEntryChars      = 600;
constexpr size_t kMaxTranscriptChars = 24000;

// Newlines and whitespace runs as single spaces, trimmed, at most
// kMaxEntryChars characters (the last one an ellipsis when cut).
std::string entryLine(const SummaryEntry &e) {
    std::string text;
    bool        space = false;
    size_t      chars = 0, keep = 0; // keep: bytes of the first kMaxEntryChars - 1
    for (size_t i = 0; i < e.text.size() && chars <= kMaxEntryChars;) {
        const size_t   start = i;
        const uint32_t cp    = utf8::decode(e.text, i);
        if (utf8::isSpace(cp)) {
            space = !text.empty();
            continue;
        }
        if (space) {
            text += ' ';
            if (++chars == kMaxEntryChars - 1)
                keep = text.size();
        }
        space = false;
        text.append(e.text, start, i - start);
        if (++chars == kMaxEntryChars - 1)
            keep = text.size();
    }
    if (chars > kMaxEntryChars) {
        text.resize(keep);
        text += "\xE2\x80\xA6";
    }
    std::string line;
    if (e.threadReply)
        line += "    \xE2\x86\xB3 "; // "↳" marks a thread reply
    if (!e.author.empty())
        line += e.author + ": ";
    return line + text;
}

} // namespace

const char *const kAiLanguages[16] = {
    "de", "en", "es", "fr", "hi", "it", "ja", "ko", "nl", "pl", "pt", "ru", "sv", "tr", "uk", "zh"
};

const char *languageName(std::string_view code) {
    code = code.substr(0, code.find_first_of("-_")); // "pt-BR" → "pt"
    for (const char *c : kAiLanguages)
        if (code == c)
            return spell::englishName(c);
    return "English";
}

Request summaryRequest(const std::vector<SummaryEntry> &entries, std::string_view languageCode) {
    std::vector<std::string> lines;
    std::vector<size_t>      sizes; // in characters
    size_t                   total = 0;
    lines.reserve(entries.size());
    for (const SummaryEntry &e : entries) {
        lines.push_back(entryLine(e));
        sizes.push_back(utf8::countCodePoints(lines.back()));
        total += sizes.back() + 1;
    }
    // Over budget: keep a head [0, i) and a tail [j, n), drop the middle.
    size_t i = lines.size(), j = lines.size();
    if (total > kMaxTranscriptChars) {
        size_t headSize = 0, tailSize = 0;
        i = 0;
        while (i < j) {
            if (headSize <= tailSize) {
                if (headSize + sizes[i] > kMaxTranscriptChars / 2)
                    break;
                headSize += sizes[i++] + 1;
            } else {
                if (tailSize + sizes[j - 1] > kMaxTranscriptChars / 2)
                    break;
                tailSize += sizes[--j] + 1;
            }
        }
    }
    std::string transcript;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k == i && i < j)
            transcript += str::concat(
                {k ? "\n" : "",
                 "[\xE2\x80\xA6 ",
                 str::number(int64_t(j - i)),
                 " messages omitted \xE2\x80\xA6]"}
            );
        if (k >= i && k < j)
            continue;
        if (!transcript.empty())
            transcript += '\n';
        transcript += lines[k];
    }

    const char *language = languageName(languageCode);
    Request     req;
    req.maxTokens = 512;
    req.system    = i18n::arg(
        "You summarize workplace chat discussions.\n"
        "Write in %1 only, no matter what language the transcript is in.\n"
        "Use plain, everyday language \xE2\x80\x94 short sentences, simple words, like a "
        "colleague catching someone up. No corporate or bureaucratic phrasing.\n"
        "Say what the discussion was about and the details that matter: anything "
        "decided, done, or left open. A short paragraph is usually enough. Use "
        "Markdown bullet points or bold labels only when the content genuinely "
        "calls for them \xE2\x80\x94 never force a fixed template or add a section that "
        "would be empty.\n"
        "Emphasize the key stuff with Markdown: bold (**\xE2\x80\xA6**) the few words that "
        "matter most \xE2\x80\x94 decisions, deadlines, names, numbers \xE2\x80\x94 so the "
        "summary can be skimmed. Emphasis, not decoration: a handful of bolded phrases, not "
        "whole sentences.\n"
        "Keep the whole summary under 100 words. No preamble, no closing remarks.",
        language
    );
    // The language instruction is repeated AFTER the transcript: small models
    // otherwise drift into the transcript's language — a system-prompt line
    // thousands of tokens back loses to the content in front of the answer.
    req.messages.push_back(
        {Message::Role::User,
         "Summarize the following discussion transcript. Lines starting with "
         "\"\xE2\x86\xB3\" are thread replies to the message above them.\n\n" +
             transcript +
             i18n::arg(
                 "\n\nWrite the summary in %1, even if the transcript is in a different "
                 "language. Keep the wording simple and conversational.",
                 language
             )}
    );
    return req;
}

} // namespace llm
