#include "prim/utf8.h"

namespace prim::utf8 {

uint32_t decode(std::string_view s, size_t &i) {
    const size_t n = s.size();
    if (i >= n)
        return kReplacement;
    const uint8_t b0 = uint8_t(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int      len;
    uint32_t cp, min;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2, cp = b0 & 0x1F, min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3, cp = b0 & 0x0F, min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4, cp = b0 & 0x07, min = 0x10000;
    } else {
        ++i; // stray continuation or 0xF8+
        return kReplacement;
    }
    if (i + len > n) {
        ++i;
        return kReplacement;
    }
    for (int k = 1; k < len; ++k) {
        const uint8_t b = uint8_t(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return kReplacement;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return kReplacement;
    }
    i += len;
    return cp;
}

size_t encode(char *out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = kReplacement;
    if (cp < 0x80) {
        out[0] = char(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = char(0xC0 | (cp >> 6));
        out[1] = char(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = char(0xE0 | (cp >> 12));
        out[1] = char(0x80 | ((cp >> 6) & 0x3F));
        out[2] = char(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = char(0xF0 | (cp >> 18));
    out[1] = char(0x80 | ((cp >> 12) & 0x3F));
    out[2] = char(0x80 | ((cp >> 6) & 0x3F));
    out[3] = char(0x80 | (cp & 0x3F));
    return 4;
}

void append(std::string &out, uint32_t cp) {
    char b[4];
    out.append(b, encode(b, cp));
}

size_t truncateAt(std::string_view s, size_t maxBytes) {
    if (maxBytes >= s.size())
        return s.size();
    // Only a sequence that starts up to three bytes back can span maxBytes;
    // a byte that isn't a continuation byte always starts one.
    size_t lead = maxBytes;
    for (int k = 0; k < 3 && lead > 0 && (uint8_t(s[lead]) & 0xC0) == 0x80; ++k)
        --lead;
    if ((uint8_t(s[lead]) & 0xC0) == 0x80)
        return maxBytes; // a stray continuation byte: a sequence of its own
    size_t end = lead;
    decode(s, end);
    return end > maxBytes ? lead : maxBytes;
}

// A decoded U+FFFD is only an error when the input did not literally hold
// EF BF BD — the one thing decode() cannot tell its caller.
static bool isLiteralReplacement(std::string_view s, size_t start, size_t end) {
    return end - start == 3 && uint8_t(s[start]) == 0xEF;
}

bool isValid(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        if (uint8_t(s[i]) < 0x80) {
            ++i;
            continue;
        }
        const size_t start = i;
        if (decode(s, i) == kReplacement && !isLiteralReplacement(s, start, i))
            return false;
    }
    return true;
}

std::string sanitize(std::string_view s) {
    if (isValid(s))
        return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t   start = i;
        const uint32_t cp    = decode(s, i);
        if (cp == kReplacement && !isLiteralReplacement(s, start, i))
            append(out, kReplacement);
        else
            out.append(s.substr(start, i - start));
    }
    return out;
}

} // namespace prim::utf8
