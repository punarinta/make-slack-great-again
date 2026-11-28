// UTF-8 helpers. All text in next is UTF-8 in std::string / std::string_view;
// offsets are byte offsets. Invalid input never crashes or loops: a bad byte
// decodes as U+FFFD and advances by one.
#pragma once

#include "prim/utf8.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace utf8 {

// The decoder and encoder live in prim (plat uses them too); declared here
// under their utf8:: names.
using prim::utf8::kReplacement;

// Decodes the code point at s[i] and advances i past it. Overlong forms,
// surrogates, values above U+10FFFF and truncated sequences yield U+FFFD
// (advancing one byte, so a resync happens at the next lead byte).
using prim::utf8::decode;
// Writes cp as UTF-8 into out[0..3] and returns the byte count (1-4); U+FFFD
// for surrogates / out of range. The one encoder: append() and the JSON
// parser's in-place unescape both use it.
using prim::utf8::encode;
// Appends cp as UTF-8 (U+FFFD for surrogates / out of range).
using prim::utf8::append;
// Byte length of cp encoded (1-4; 3 for invalid, i.e. U+FFFD).
int encodedLength(uint32_t cp);

using prim::utf8::isValid;
// Copy with every invalid sequence replaced by U+FFFD.
using prim::utf8::sanitize;
size_t countCodePoints(std::string_view s);

// Byte offset of the next / previous code point boundary (clamped to [0, size]).
size_t nextBoundary(std::string_view s, size_t i);
size_t prevBoundary(std::string_view s, size_t i);
// Largest boundary <= maxBytes: truncating there never splits a sequence.
using prim::utf8::truncateAt;
// Byte length of the first `n` code points (all of `s` when it has fewer).
size_t      prefixBytes(std::string_view s, size_t n);
// `s` when it has at most `maxCodePoints` code points; else its first `keep`
// (at most maxCodePoints) and "…". One pass.
std::string ellipsize(std::string_view s, size_t maxCodePoints, size_t keep = size_t(-1));

// ── Classification (compact approximations of the Unicode properties) ────────
bool isSpace(uint32_t cp); // White_Space
bool isDigit(uint32_t cp); // ASCII 0-9 only: the parsers only need those
// Letters and numbers. ASCII exactly; above it everything counts except the
// punctuation, symbol, space and emoji blocks — close enough for the one job
// it has: telling "snake_case" (intraword '_') from "_italic_".
bool isWordChar(uint32_t cp);

// ── Case folding for search ─────────────────────────────────────────────────
// Simple (1:1) case folding for Latin, Greek, Cyrillic, Armenian and
// fullwidth Latin — the scripts whose case matters for chat search. Other
// code points map to themselves.
uint32_t    foldCase(uint32_t cp);
std::string foldCase(std::string_view s);
// True if `haystack` contains `needle` ignoring case. Folds on the fly, so no
// copy of the haystack is made (search runs over every message).
bool        containsFolded(std::string_view haystack, std::string_view needle);
// containsFolded with the needle already through foldCase(string_view): a
// filter folds the query once, not once per row it tests.
bool        containsFoldedNeedle(std::string_view haystack, std::string_view foldedNeedle);
// containsFolded with both sides already through foldCase(string_view) — for
// filters that fold the query once and keep their labels folded. Matches
// start at a grapheme boundary, as containsFolded's do. No allocation.
bool        containsPrefolded(std::string_view foldedHaystack, std::string_view foldedNeedle);

// Upper case as the folding sees it: foldCase changes it.
bool isUpper(uint32_t cp);

// The code points of `s`, through foldCase when `fold`.
std::vector<uint32_t> codePoints(std::string_view s, bool fold = false);

} // namespace utf8
