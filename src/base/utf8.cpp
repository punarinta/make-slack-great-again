#include "base/utf8.h"

#include <algorithm>

namespace utf8 {

int encodedLength(uint32_t cp) {
    if (cp < 0x80)
        return 1;
    if (cp < 0x800)
        return 2;
    if (cp < 0x10000)
        return 3;
    return cp <= 0x10FFFF ? 4 : 3;
}

size_t countCodePoints(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n)
        decode(s, i);
    return n;
}

size_t nextBoundary(std::string_view s, size_t i) {
    if (i >= s.size())
        return s.size();
    decode(s, i);
    return i;
}

size_t prevBoundary(std::string_view s, size_t i) {
    if (i == 0)
        return 0;
    if (i > s.size())
        return s.size();
    // Step back over at most three continuation bytes, then verify the lead
    // really spans up to i (otherwise the byte before i stands alone).
    size_t j = i - 1;
    for (int k = 0; k < 3 && j > 0 && (uint8_t(s[j]) & 0xC0) == 0x80; ++k)
        --j;
    size_t probe = j;
    decode(s, probe);
    return probe == i ? j : i - 1;
}

size_t prefixBytes(std::string_view s, size_t n) {
    size_t i = 0;
    for (size_t k = 0; k < n && i < s.size(); ++k)
        decode(s, i);
    return i;
}

std::string ellipsize(std::string_view s, size_t maxCodePoints, size_t keep) {
    keep       = std::min(keep, maxCodePoints);
    size_t cut = 0;
    for (size_t i = 0, n = 0; i < s.size(); ++n) {
        if (n == keep)
            cut = i;
        if (n == maxCodePoints) // a code point past the cap
            return std::string(s.substr(0, cut)) + "\xE2\x80\xA6";
        decode(s, i);
    }
    return std::string(s);
}

bool isSpace(uint32_t cp) {
    return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

bool isDigit(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

bool isWordChar(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z');
    // Non-letter blocks. Everything else above ASCII (Latin-1 letters, Greek,
    // Cyrillic, CJK, …) is treated as a letter.
    if (cp < 0xC0)
        return cp == 0xAA || cp == 0xB5 || cp == 0xBA; // ª µ º
    if (cp == 0xD7 || cp == 0xF7)
        return false; // × ÷
    if (isSpace(cp))
        return false;
    if (cp >= 0x2000 && cp <= 0x2BFF) // punctuation, symbols, arrows, dingbats
        return false;
    if (cp >= 0x3000 && cp <= 0x303F) // CJK punctuation
        return false;
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFF00 && cp <= 0xFF0F) ||
        (cp >= 0xFF1A && cp <= 0xFF20))
        return false;
    if (cp >= 0x1F000 && cp <= 0x1FAFF) // emoji and pictographs
        return false;
    return true;
}

uint32_t foldCase(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    if (cp < 0x100)
        return (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) ? cp + 32 : cp;
    if (cp < 0x180) { // Latin Extended-A: mostly even upper / odd lower pairs
        if (cp == 0x130)
            return 'i';
        if (cp == 0x178)
            return 0xFF;
        if (cp == 0x17F)
            return 's';
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
            return (cp & 1) ? cp + 1 : cp;
        if (cp == 0x131 || cp == 0x138 || cp == 0x149)
            return cp;
        return cp | 1;
    }
    if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2)
        return cp + 32; // Greek capitals
    if (cp == 0x3C2)
        return 0x3C3; // final sigma
    if (cp >= 0x410 && cp <= 0x42F)
        return cp + 32; // Cyrillic А-Я
    if (cp >= 0x400 && cp <= 0x40F)
        return cp + 80; // Ѐ-Џ
    if ((cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF))
        return cp | 1;
    if (cp >= 0x531 && cp <= 0x556)
        return cp + 48; // Armenian
    if (cp >= 0x1E00 && cp <= 0x1EFF && !(cp >= 0x1E96 && cp <= 0x1E9F))
        return cp | 1; // Latin Extended Additional (Vietnamese)
    if (cp >= 0xFF21 && cp <= 0xFF3A)
        return cp + 32; // fullwidth A-Z
    return cp;
}

bool isUpper(uint32_t cp) {
    return foldCase(cp) != cp;
}

std::vector<uint32_t> codePoints(std::string_view s, bool fold) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = decode(s, i);
        out.push_back(fold ? foldCase(cp) : cp);
    }
    return out;
}

std::string foldCase(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (uint8_t(s[i]) < 0x80) { // fast path
            const char c = s[i++];
            out += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
            continue;
        }
        append(out, foldCase(decode(s, i)));
    }
    return out;
}

bool containsFolded(std::string_view haystack, std::string_view needle) {
    return needle.empty() || containsFoldedNeedle(haystack, foldCase(needle));
}

// Whether `n` (folded) matches `h` folded from byte i on. ASCII pairs compare
// inline; anything else goes through the decoder (a non-ASCII code point can
// fold to ASCII: U+0130 → 'i').
static bool foldedMatchAt(std::string_view h, size_t i, std::string_view n) {
    for (size_t k = 0; k < n.size();) {
        if (i >= h.size())
            return false;
        const uint8_t a = uint8_t(h[i]), b = uint8_t(n[k]);
        if ((a | b) < 0x80) {
            if (((a >= 'A' && a <= 'Z') ? a + 32 : a) != b)
                return false;
            ++i, ++k;
        } else if (foldCase(decode(h, i)) != decode(n, k)) {
            return false;
        }
    }
    return true;
}

bool containsFoldedNeedle(std::string_view h, std::string_view n) {
    if (n.empty())
        return true;
    const uint8_t n0 = uint8_t(n[0]);
    for (size_t start = 0; start < h.size();) {
        const uint8_t c = uint8_t(h[start]);
        if (c < 0x80) { // folds to itself or its lower case: the first byte rules most out
            if (((c >= 'A' && c <= 'Z') ? c + 32 : c) == n0 && foldedMatchAt(h, start, n))
                return true;
            ++start;
            continue;
        }
        if (foldedMatchAt(h, start, n))
            return true;
        start = nextBoundary(h, start);
    }
    return false;
}

bool containsPrefolded(std::string_view h, std::string_view n) {
    // A plain byte search: folded text is valid UTF-8, so the needle starts
    // with a lead (or ASCII) byte, and a match can only begin at a boundary.
    return n.empty() || h.find(n) != std::string_view::npos;
}

} // namespace utf8
