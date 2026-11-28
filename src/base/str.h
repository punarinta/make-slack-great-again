// Small string helpers. concat() exists for size: every `a + b + c` on
// std::string inlines its own allocation and copy sequence at the call site,
// and error-message building repeats that dozens of times per file.
#pragma once

#include "prim/str.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace str {

// One allocation, one out-of-line function: concat({"user ", id, ": bad tz"}).
std::string concat(std::initializer_list<std::string_view> parts);
std::string number(int64_t n); // decimal

inline bool startsWith(std::string_view s, std::string_view p) {
    return s.substr(0, p.size()) == p;
}
inline bool endsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
// ASCII whitespace (space, tab, CR, LF) off both ends.
std::string_view trim(std::string_view s);
// Unicode whitespace (utf8::isSpace: NBSP, the em space …) off both ends.
std::string_view trimSpace(std::string_view s);
// ASCII-only case changes (identifiers, extensions, hosts); for user text
// see utf8::foldCase. asciiLower (a char or a string) is prim's, shared with
// plat.
using prim::asciiLower;
std::string asciiUpper(std::string_view s);
// ASCII case-insensitive equality and containment (header names, tokens,
// CSS): prim's, shared with plat.
using prim::icontains;
using prim::iequals;
// Every run of Unicode whitespace (utf8::isSpace) becomes
// one space, none at the ends.
std::string simplified(std::string_view s);
// A hex digit's value (either case); -1 for any other character.
using prim::hexDigit;
// "0123456789abcdef" and "0123456789ABCDEF" (one copy each).
using prim::kHexLower;
using prim::kHexUpper;

// The parts of `s` between `sep`s, empty ones included: "a,,b" → "a", "",
// "b"; "a\n" → "a", ""; "" → one empty part.
std::vector<std::string_view> split(std::string_view s, char sep);
// The same parts one at a time, without the vector:
//     str::Splitter lines(text, '\n');
//     for (std::string_view line; lines.next(&line);) …
class Splitter {
public:
    Splitter(std::string_view s, char sep) : _rest(s), _sep(sep) {}
    bool next(std::string_view *part); // false once every part was seen

private:
    std::string_view _rest;
    char             _sep;
    bool             _done = false;
};

// HTML/XML character references: &amp; &lt; &gt; &quot; &apos; &nbsp; (any
// case) and &#NN; / &#xHH;. Anything else, a NUL reference, or a ';' more than
// ten bytes away stays literal. With nbspAsSpace, U+00A0 written as a
// reference comes out as a plain space (what a clipboard reader wants).
std::string decodeEntities(std::string_view s, bool nbspAsSpace = false);
// '&', '<' and '>' as &amp; &lt; &gt; (and '"' as &quot; with `quotes`, for
// an attribute value) appended to *out; every other byte as it is.
// Both are prim's, shared with plat.
using prim::appendEscapedHtml;
using prim::escapeHtml;

// Every byte but the RFC 3986 unreserved characters (A-Z a-z 0-9 - . _ ~)
// and those in `keep` as %XX (upper-case hex): a query value, a path. Both
// are prim's, shared with plat.
using prim::percentEncode;
// %XX sequences back to bytes; a malformed '%' and '+' stay as they are.
using prim::percentDecode;

// File sizes in binary units (1 KB = 1024 B), in one of these styles.
enum class ByteSize : uint8_t {
    Whole, // "812 B", "12 KB", "3 MB": all floored (composer chips, Settings)
    File,  // floored KB, MB with one decimal below 10: "1.4 MB", "23 MB";
           // "" for 0 or less, an unknown size (message file chips)
    Exact, // rounded KB, one decimal for MB and GB: "1.4 MB", "2.0 GB" (file browser)
};
std::string byteSize(int64_t bytes, ByteSize style = ByteSize::Whole);

} // namespace str
