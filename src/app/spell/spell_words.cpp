// The checkable words, over UTF-8: the text is decoded once
// into code points (with their byte offsets), the non-prose parts are blanked
// to spaces in place, and the words are cut from what is left.
#include "app/spell/spell.h"

#include "base/utf8.h"
#include "text/unicode.h"

#include <algorithm>

namespace spell {

namespace {

constexpr uint32_t kMaxWordLength = 64; // code points

// HarfBuzz's general categories (hb_unicode_general_category_t), which
// text::uni::category reports.
enum : int {
    kLl = 5, // lowercase … uppercase letters: 5-9
    kLu = 9,
    kMc = 10, // marks: 10-12
    kMn = 12,
    kNd = 13, // numbers: 13-15
    kNo = 15,
};

struct Cp {
    uint32_t c;   // the code point (' ' once blanked)
    uint32_t off; // its byte offset
};

bool isLetter(uint32_t c) {
    if (c < 0x80)
        return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    const int k = text::uni::category(c);
    return k >= kLl && k <= kLu;
}
bool isMark(uint32_t c) {
    if (c < 0x80)
        return false;
    const int k = text::uni::category(c);
    return k >= kMc && k <= kMn;
}
bool isNumber(uint32_t c) {
    if (c < 0x80)
        return c >= '0' && c <= '9';
    const int k = text::uni::category(c);
    return k >= kNd && k <= kNo;
}
bool isUpper(uint32_t c) {
    if (c < 0x80)
        return c >= 'A' && c <= 'Z';
    return text::uni::category(c) == kLu;
}
bool isSpace(uint32_t c) {
    return utf8::isSpace(c);
}
bool isApostrophe(uint32_t c) {
    return c == '\'' || c == 0x2019;
}
bool isWordChar(uint32_t c) {
    return isLetter(c) || isNumber(c) || isMark(c) || c == '_';
}

// An apostrophe at i that joins two letters ("don't"), not a quote mark.
bool isInnerApostrophe(const std::vector<Cp> &s, size_t i) {
    return isApostrophe(s[i].c) && i > 0 && i + 1 < s.size() && isLetter(s[i - 1].c) &&
           isLetter(s[i + 1].c);
}

std::vector<Cp> decode(std::string_view text) {
    std::vector<Cp> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const uint32_t off = uint32_t(i);
        out.push_back({utf8::decode(text, i), off});
    }
    return out;
}

void blank(std::vector<Cp> &s, size_t from, size_t to) {
    for (size_t i = from; i < std::min(to, s.size()); ++i)
        s[i].c = ' ';
}

// ``` fences (their markers included, across lines) and `inline code` outside
// them. A lone backtick with no partner on its line is literal text, as Slack
// renders it.
void blankCode(std::vector<Cp> &s) {
    const size_t n          = s.size();
    bool         inFence    = false;
    size_t       fenceStart = 0;
    size_t       i          = 0;
    while (i < n) {
        if (i + 2 < n && s[i].c == '`' && s[i + 1].c == '`' && s[i + 2].c == '`') {
            if (inFence)
                blank(s, fenceStart, i + 3);
            else
                fenceStart = i;
            inFence = !inFence;
            i += 3;
            continue;
        }
        if (!inFence && s[i].c == '`') {
            size_t close = i + 1;
            while (close < n && s[close].c != '`' && s[close].c != '\n')
                ++close;
            if (close < n && s[close].c == '`') {
                blank(s, i, close + 1);
                i = close + 1;
                continue;
            }
        }
        ++i;
    }
    if (inFence)
        blank(s, fenceStart, n);
}

bool asciiAlnum(uint32_t c) {
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}

// "scheme://…" and "www.…" up to the next whitespace; the scheme is a letter
// then letters, digits, '+', '.', '-', starting at a word boundary.
void blankUrls(std::vector<Cp> &s) {
    const size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        size_t start = n;
        if (i + 2 < n && s[i].c == ':' && s[i + 1].c == '/' && s[i + 2].c == '/') {
            size_t b = i;
            while (b > 0 && (asciiAlnum(s[b - 1].c) || s[b - 1].c == '+' || s[b - 1].c == '.' ||
                             s[b - 1].c == '-'))
                --b;
            while (b < i && !((s[b].c | 0x20) >= 'a' && (s[b].c | 0x20) <= 'z'))
                ++b; // the scheme starts with a letter
            if (b < i && (b == 0 || !isWordChar(s[b - 1].c)))
                start = b;
        } else if (
            i + 3 < n && s[i].c == 'w' && s[i + 1].c == 'w' && s[i + 2].c == 'w' &&
            s[i + 3].c == '.' && (i == 0 || !isWordChar(s[i - 1].c))
        ) {
            start = i;
        }
        if (start == n)
            continue;
        size_t e = i;
        while (e < n && !isSpace(s[e].c))
            ++e;
        blank(s, start, e);
        i = e;
    }
}

// Slack's <…> tokens: '<', something not blank, then anything but < > up
// to the '>'.
void blankTokens(std::vector<Cp> &s) {
    const size_t n = s.size();
    for (size_t i = 0; i + 2 < n; ++i) {
        if (s[i].c != '<' || s[i + 1].c == '<' || s[i + 1].c == '>' || isSpace(s[i + 1].c))
            continue;
        size_t j = i + 1;
        while (j < n && s[j].c != '<' && s[j].c != '>' && s[j].c != '\n')
            ++j;
        if (j < n && s[j].c == '>') {
            blank(s, i, j + 1);
            i = j;
        }
    }
}

// :shortcode: — letters, digits, _ + ' - between colons.
void blankEmoji(std::vector<Cp> &s) {
    const auto inCode = [](uint32_t c) {
        return asciiAlnum(c) || c == '_' || c == '+' || c == '\'' || c == '-';
    };
    const size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        if (s[i].c != ':')
            continue;
        size_t j = i + 1;
        while (j < n && inCode(s[j].c))
            ++j;
        if (j > i + 1 && j < n && s[j].c == ':') {
            blank(s, i, j + 1);
            i = j;
        }
    }
}

// A whitespace-separated chunk that is a mention, address, channel, command,
// path or host name as a whole: none of its words are prose.
bool isSkippedChunk(const std::vector<Cp> &s, size_t from, size_t to) {
    if (s[from].c == '#' || s[from].c == '/')
        return true;
    for (size_t i = from; i < to; ++i) {
        if (s[i].c == '@' || s[i].c == '\\')
            return true;
        if (s[i].c == '.' && i > from && i + 1 < to && isLetter(s[i - 1].c) && isLetter(s[i + 1].c))
            return true;
    }
    return false;
}

bool isCheckableWord(const std::vector<Cp> &s, size_t from, size_t to) {
    const size_t len = to - from;
    if (len < 2 || len > kMaxWordLength)
        return false;
    int  letters = 0, uppers = 0;
    bool innerUpper = false;
    for (size_t i = from; i < to; ++i) {
        const uint32_t c = s[i].c;
        if (isNumber(c) || c == '_')
            return false;
        if (!isLetter(c))
            continue;
        ++letters;
        if (isUpper(c)) {
            ++uppers;
            if (i > from)
                innerUpper = true;
        }
    }
    if (letters >= 2 && uppers == letters)
        return false;   // ALL CAPS: an acronym
    return !innerUpper; // camelCase / iPhone-style names
}

uint32_t byteAt(const std::vector<Cp> &s, size_t i, size_t textSize) {
    return i < s.size() ? s[i].off : uint32_t(textSize);
}

} // namespace

std::vector<Span> checkableWords(std::string_view text, const std::vector<Span> &excluded) {
    std::vector<Cp> s = decode(text);
    for (const Span &x : excluded) { // pills read as whitespace; s is in byte order
        auto c = std::lower_bound(s.begin(), s.end(), x.start, [](const Cp &a, uint32_t off) {
            return a.off < off;
        });
        for (; c != s.end() && c->off < x.end(); ++c)
            c->c = ' ';
    }
    blankCode(s);
    blankUrls(s);
    blankTokens(s);
    blankEmoji(s);

    std::vector<Span> words;
    const size_t      n = s.size();
    size_t            i = 0;
    while (i < n) {
        while (i < n && isSpace(s[i].c))
            ++i;
        const size_t chunkStart = i;
        while (i < n && !isSpace(s[i].c))
            ++i;
        const size_t chunkEnd = i;
        if (chunkStart == chunkEnd || isSkippedChunk(s, chunkStart, chunkEnd))
            continue;
        size_t j = chunkStart;
        while (j < chunkEnd) {
            while (j < chunkEnd && !isWordChar(s[j].c))
                ++j;
            size_t from = j;
            while (j < chunkEnd && (isWordChar(s[j].c) || isInnerApostrophe(s, j)))
                ++j;
            size_t to = j;
            // _italic_ markers belong to the markup, not the word.
            while (from < to && s[from].c == '_')
                ++from;
            while (to > from && s[to - 1].c == '_')
                --to;
            if (isCheckableWord(s, from, to)) {
                const uint32_t a = s[from].off, b = byteAt(s, to, text.size());
                words.push_back({a, b - a});
            }
        }
    }
    return words;
}

Span wordAt(std::string_view text, uint32_t pos) {
    const std::vector<Cp> s  = decode(text);
    // The code point index at byte pos (s.size() at the end).
    size_t                at = 0;
    while (at < s.size() && s[at].off < pos)
        ++at;
    const auto inWord = [&](size_t i) {
        return i < s.size() && (isWordChar(s[i].c) || isInnerApostrophe(s, i));
    };
    size_t from = at, to = at;
    while (from > 0 && inWord(from - 1))
        --from;
    while (inWord(to))
        ++to;
    const uint32_t a = byteAt(s, from, text.size()), b = byteAt(s, to, text.size());
    return {a, b - a};
}

} // namespace spell
