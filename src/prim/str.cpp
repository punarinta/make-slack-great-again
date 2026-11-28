#include "prim/str.h"

#include <cstdint>

namespace prim {

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        c = asciiLower(c);
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    return true;
}

bool icontains(std::string_view hay, std::string_view needle) {
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (iequals(hay.substr(i, needle.size()), needle))
            return true;
    return false;
}

std::string_view trim(std::string_view s, std::string_view chars) {
    const size_t b = s.find_first_not_of(chars);
    if (b == std::string_view::npos)
        return {};
    return s.substr(b, s.find_last_not_of(chars) - b + 1);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = asciiLower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void appendEscapedHtml(std::string *out, std::string_view s, bool quotes) {
    for (const char c : s) {
        switch (c) {
        case '&':
            *out += "&amp;";
            break;
        case '<':
            *out += "&lt;";
            break;
        case '>':
            *out += "&gt;";
            break;
        case '"':
            *out += quotes ? "&quot;" : "\"";
            break;
        default:
            *out += c;
        }
    }
}

std::string escapeHtml(std::string_view s, bool quotes) {
    std::string out;
    out.reserve(s.size());
    appendEscapedHtml(&out, s, quotes);
    return out;
}

const char kHexLower[17] = "0123456789abcdef";
const char kHexUpper[17] = "0123456789ABCDEF";

std::string percentEncode(std::string_view s, std::string_view keep) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const auto c = uint8_t(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~' ||
            keep.find(ch) != std::string_view::npos) {
            out += ch;
        } else {
            out += '%';
            out += kHexUpper[c >> 4];
            out += kHexUpper[c & 15];
        }
    }
    return out;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        int hi = -1, lo = -1;
        if (s[i] == '%' && i + 2 < s.size() && (hi = hexDigit(s[i + 1])) >= 0 &&
            (lo = hexDigit(s[i + 2])) >= 0) {
            out += char(hi << 4 | lo);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace prim
