// The UTF-8 decoder and encoder (see prim's CMakeLists.txt): base declares
// them as utf8::…, plat calls them here. Invalid input never crashes or
// loops: a bad byte decodes as U+FFFD and advances by one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace prim::utf8 {

constexpr uint32_t kReplacement = 0xFFFD;

// Decodes the code point at s[i] and advances i past it. Overlong forms,
// surrogates, values above U+10FFFF and truncated sequences yield U+FFFD
// (advancing one byte, so a resync happens at the next lead byte).
uint32_t decode(std::string_view s, size_t &i);
// Writes cp as UTF-8 into out[0..3] and returns the byte count (1-4); U+FFFD
// for surrogates / out of range. The one encoder: append() and the JSON
// parser's in-place unescape both use it.
size_t   encode(char *out, uint32_t cp);
// Appends cp as UTF-8 (U+FFFD for surrogates / out of range).
void     append(std::string &out, uint32_t cp);

// Largest boundary <= maxBytes: truncating there never splits a sequence.
size_t truncateAt(std::string_view s, size_t maxBytes);

bool        isValid(std::string_view s);
// Copy with every invalid sequence replaced by U+FFFD.
std::string sanitize(std::string_view s);

} // namespace prim::utf8
