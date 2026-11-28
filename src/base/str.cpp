#include "base/str.h"

#include "base/utf8.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace str {

std::string concat(std::initializer_list<std::string_view> parts) {
    size_t n = 0;
    for (auto p : parts)
        n += p.size();
    std::string out;
    out.reserve(n);
    for (auto p : parts)
        out.append(p);
    return out;
}

std::string number(int64_t n) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, n);
    return std::string(buf, r.ptr);
}

std::string_view trim(std::string_view s) {
    return prim::trim(s, " \t\r\n");
}

std::string_view trimSpace(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        size_t i = b;
        if (!utf8::isSpace(utf8::decode(s, i)))
            break;
        b = i;
    }
    while (e > b) {
        size_t p = utf8::prevBoundary(s, e), i = p;
        if (!utf8::isSpace(utf8::decode(s, i)))
            break;
        e = p;
    }
    return s.substr(b, e - b);
}

std::string asciiUpper(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        if (c >= 'a' && c <= 'z')
            c = char(c - 32);
    return out;
}

std::string simplified(std::string_view s) {
    std::string out;
    bool        gap = false;
    for (size_t i = 0; i < s.size();) {
        const size_t   at = i;
        const uint32_t cp = utf8::decode(s, i);
        if (utf8::isSpace(cp)) {
            gap = !out.empty();
            continue;
        }
        if (gap)
            out += ' ';
        gap = false;
        out.append(s.substr(at, i - at));
    }
    return out;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    Splitter                      parts(s, sep);
    for (std::string_view p; parts.next(&p);)
        out.push_back(p);
    return out;
}

bool Splitter::next(std::string_view *part) {
    if (_done)
        return false;
    const size_t at = _rest.find(_sep);
    *part           = _rest.substr(0, at);
    if (at == std::string_view::npos)
        _done = true;
    else
        _rest.remove_prefix(at + 1);
    return true;
}

std::string decodeEntities(std::string_view s, bool nbspAsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        // The ';' is looked for nearby only: a stray '&' never scans the rest.
        const size_t semi = s[i] == '&' ? s.substr(0, i + 11).find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        uint32_t               cp   = 0;
        if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] | 0x20) == 'x';
            for (size_t k = hex ? 2 : 1; k < name.size(); ++k) {
                const int d = hex                                ? hexDigit(name[k])
                              : name[k] >= '0' && name[k] <= '9' ? name[k] - '0'
                                                                 : -1;
                if (d < 0)
                    break;
                // Saturate: anything past U+10FFFF encodes as U+FFFD anyway.
                cp = std::min<uint32_t>(cp * (hex ? 16 : 10) + uint32_t(d), 0x110000);
            }
        } else if (iequals(name, "amp"))
            cp = '&';
        else if (iequals(name, "lt"))
            cp = '<';
        else if (iequals(name, "gt"))
            cp = '>';
        else if (iequals(name, "quot"))
            cp = '"';
        else if (iequals(name, "apos"))
            cp = '\'';
        else if (iequals(name, "nbsp"))
            cp = 0xA0;
        if (cp == 0) {
            out += '&';
            continue;
        }
        utf8::append(out, nbspAsSpace && cp == 0xA0 ? ' ' : cp);
        i = semi;
    }
    return out;
}

std::string byteSize(int64_t n, ByteSize style) {
    constexpr int64_t kKB = 1024, kMB = kKB * 1024, kGB = kMB * 1024;
    char              buf[32];
    if (style == ByteSize::File && n <= 0)
        return {};
    if (n < kKB) {
        std::snprintf(buf, sizeof buf, "%lld B", (long long)n);
    } else if (n < kMB) {
        const int64_t kb = (style == ByteSize::Exact ? n + kKB / 2 : n) / kKB;
        std::snprintf(buf, sizeof buf, "%lld KB", (long long)kb);
    } else if (style == ByteSize::Whole) {
        std::snprintf(buf, sizeof buf, "%lld MB", (long long)(n / kMB));
    } else if (style == ByteSize::Exact && n >= kGB) {
        std::snprintf(buf, sizeof buf, "%.1f GB", double(n) / double(kGB));
    } else {
        const double mb    = double(n) / double(kMB);
        const bool   whole = style == ByteSize::File && mb >= 10;
        std::snprintf(buf, sizeof buf, whole ? "%.0f MB" : "%.1f MB", mb);
    }
    return buf;
}

} // namespace str
