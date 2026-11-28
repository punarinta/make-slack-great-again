// Small string helpers every backend shares: ASCII case folding and trimming,
// markup escaping for notification text, and URL-scheme bookkeeping (which
// schemes this process registered, which launch arguments are URLs of them).
// Pure string code, no OS calls. Folding, trimming and escaping are prim's
// (src/prim), shared with the app's base library.
#pragma once

#include "prim/str.h"

#include <string>
#include <string_view>
#include <vector>

namespace plat::core {

// A–Z folded to a–z; every other byte (UTF-8 included) is left alone.
using prim::asciiLower; // a char or a string

// `s` without leading and trailing bytes from `chars`.
inline std::string_view trim(std::string_view s, std::string_view chars = " \t") {
    return prim::trim(s, chars);
}

// &, < and > as entities (and " too when `quotes`): text placed inside
// notification-body markup or an XML element or attribute.
inline std::string escapeMarkup(std::string_view s, bool quotes = false) {
    return prim::escapeHtml(s, quotes);
}

// RFC 3986 scheme syntax: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
bool validScheme(std::string_view s);

// Adds the lower-cased `scheme` to `schemes` unless it is there already.
void rememberScheme(std::vector<std::string> &schemes, std::string_view scheme);
// True when `arg` is "<scheme>:…" for one of `schemes` (lower case), in any case.
bool isSchemeUrl(std::string_view arg, const std::vector<std::string> &schemes);
// The arguments that are URLs of `schemes`, in order: what a forwarded launch
// delivers as OpenUrls.
std::vector<std::string>
schemeUrls(const std::vector<std::string> &args, const std::vector<std::string> &schemes);

} // namespace plat::core
