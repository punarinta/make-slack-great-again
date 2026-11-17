#include "app/mrkdwn/smileys.h"

#include "app/mrkdwn/emoji.h"
#include "base/str.h"

#include <algorithm>
#include <cctype>

namespace mrkdwn {

namespace {

// The usual set, Slack's own table as far as these emoji go. Typed as the
// settings file would hold it, so there is one syntax to read.
constexpr const char *kDefaults = ":) slightly_smiling_face\n"
                                  ":-) slightly_smiling_face\n"
                                  ":( slightly_frowning_face\n"
                                  ":-( slightly_frowning_face\n"
                                  ":D smiley\n"
                                  ":-D smiley\n"
                                  ";) wink\n"
                                  ";-) wink\n"
                                  ":P stuck_out_tongue\n"
                                  ":-P stuck_out_tongue\n"
                                  ":p stuck_out_tongue\n"
                                  ":-p stuck_out_tongue\n"
                                  ";P stuck_out_tongue_winking_eye\n"
                                  ";p stuck_out_tongue_winking_eye\n"
                                  ":O open_mouth\n"
                                  ":-O open_mouth\n"
                                  ":o open_mouth\n"
                                  ":-o open_mouth\n"
                                  ":| neutral_face\n"
                                  ":-| neutral_face\n"
                                  ":/ confused\n"
                                  ":S confused\n"
                                  ":s confused\n"
                                  ":* kiss\n"
                                  ":-* kiss\n"
                                  ":'( cry\n"
                                  ">:( angry\n"
                                  "<3 heart\n";

// The emoji a right-hand side stands for: the character itself, or a shortcode
// resolved through the compiled-in table. An unknown shortcode stays as typed.
std::string resolveTo(std::string_view rhs) {
    std::string_view name = rhs;
    if (name.size() > 1 && name.front() == ':' && name.back() == ':')
        name = name.substr(1, name.size() - 2);
    const std::string glyph = emoji::toUnicode(name);
    return glyph.empty() ? std::string(rhs) : glyph;
}

// A letter, a digit or "_" in any script: any byte of a multi-byte UTF-8
// character counts, so "café:)" is a word with a colon in it too.
bool wordByte(unsigned char c) {
    return c >= 0x80 || std::isalnum(c) || c == '_';
}

// A smiley stands alone: not inside a word, and not part of a shortcode or a
// longer token (":D" in ":Date:" is the start of a word).
bool standsAlone(std::string_view text, size_t at, size_t len) {
    if (at > 0) {
        const auto c = static_cast<unsigned char>(text[at - 1]);
        if (wordByte(c) || c == ':' || c == ';' || c == '<' || c == '>' || c == '@' || c == '#')
            return false;
    }
    const size_t end = at + len;
    return end >= text.size() || !wordByte(static_cast<unsigned char>(text[end]));
}

// Where the run of code (or the fenced block) starting at `at` ends: the text
// after it is the user's again.
size_t codeEnd(std::string_view text, size_t at) {
    const bool   fence = text.compare(at, 3, "```") == 0;
    const size_t close = fence ? text.find("```", at + 3) : text.find('`', at + 1);
    if (close == std::string_view::npos)
        return text.size();
    return fence ? close + 3 : close + 1;
}

// Whether `at` sits inside a code span or a fenced block.
bool inCode(std::string_view text, size_t at) {
    size_t i = 0;
    while (i < at) {
        if (text[i] != '`') {
            ++i;
            continue;
        }
        const size_t end = codeEnd(text, i);
        if (at < end)
            return true;
        i = end;
    }
    return false;
}

} // namespace

std::string_view defaultSmileyMap() {
    return kDefaults;
}

std::vector<Smiley> parseSmileys(std::string_view map) {
    const std::string_view src = str::trim(map).empty() ? std::string_view(kDefaults) : map;
    std::vector<Smiley>    out;
    size_t                 i = 0;
    while (i <= src.size()) {
        size_t j = src.find('\n', i);
        if (j == std::string_view::npos)
            j = src.size();
        const std::string_view line = str::trim(src.substr(i, j - i));
        i                           = j + 1;
        if (line.empty() || line.front() == '#')
            continue;
        // The smiley never holds a space, the emoji is the rest of the line.
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string_view::npos)
            continue;
        const std::string_view from = line.substr(0, sp);
        const std::string_view to   = str::trim(line.substr(sp));
        if (from.empty() || to.empty())
            continue;
        const bool seen =
            std::any_of(out.begin(), out.end(), [&](const Smiley &s) { return s.from == from; });
        if (!seen) // the first line for a smiley wins, as in the file
            out.push_back({std::string(from), resolveTo(to)});
    }
    // The longest first: ":-)" before ":)", and "xD" before "x".
    std::stable_sort(out.begin(), out.end(), [](const Smiley &a, const Smiley &b) {
        return a.from.size() > b.from.size();
    });
    return out;
}

SmileyHit smileyEndingAt(std::string_view text, size_t stop, std::string_view map) {
    return smileyEndingAt(text, stop, parseSmileys(map));
}

SmileyHit smileyEndingAt(std::string_view text, size_t stop, const std::vector<Smiley> &smileys) {
    if (stop == 0 || stop > text.size())
        return {};
    // Longest first, so ":-)" wins over ":)"; the first hit is the one to take.
    for (const Smiley &s : smileys) {
        if (s.from.size() > stop)
            continue;
        const size_t at = stop - s.from.size();
        if (text.compare(at, s.from.size(), s.from) != 0 || !standsAlone(text, at, s.from.size()) ||
            inCode(text, at))
            continue;
        return {uint32_t(at), uint32_t(s.from.size()), s.to};
    }
    return {};
}

} // namespace mrkdwn
