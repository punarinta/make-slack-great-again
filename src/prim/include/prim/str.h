// ASCII and URL string primitives (see prim's CMakeLists.txt): base declares
// them as str::…, plat as core::…. Pure string code, no OS calls.
#pragma once

#include <string>
#include <string_view>

namespace prim {

// A–Z folded to a–z; every other byte (UTF-8 included) is left alone.
inline char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c + 32) : c;
}
std::string asciiLower(std::string_view s);
// ASCII case-insensitive equality and containment (header names, tokens,
// CSS); other bytes compare as they are.
bool        iequals(std::string_view a, std::string_view b);
bool        icontains(std::string_view hay, std::string_view needle);

// `s` without leading and trailing bytes from `chars`.
std::string_view trim(std::string_view s, std::string_view chars);

// A hex digit's value (either case); -1 for any other character.
int               hexDigit(char c);
// The hex digits by value, "0123456789abcdef" and upper case: one copy each.
extern const char kHexLower[17];
extern const char kHexUpper[17];

// '&', '<' and '>' as &amp; &lt; &gt; (and '"' as &quot; with `quotes`, for
// an attribute value) appended to *out; every other byte as it is.
void        appendEscapedHtml(std::string *out, std::string_view s, bool quotes = false);
std::string escapeHtml(std::string_view s, bool quotes = false);

// Every byte but the RFC 3986 unreserved characters (A-Z a-z 0-9 - . _ ~)
// and those in `keep` as %XX (upper-case hex): a query value, a path.
std::string percentEncode(std::string_view s, std::string_view keep = {});
// %XX sequences back to bytes; a malformed '%' and '+' stay as they are.
std::string percentDecode(std::string_view s);

} // namespace prim
