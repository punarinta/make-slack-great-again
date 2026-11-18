// Unicode helpers for layout: UTF-8 decoding, grapheme clusters (UAX #29
// essentials), line-break opportunities (UAX #14 essentials), bidi classes
// and levels (a UAX #9 subset). Character properties come from HarfBuzz's
// built-in UCD tables, so this file carries almost no data of its own.
#pragma once

#include <cstddef>
#include <cstdint>

namespace text::uni {

// UTF-8 decoding is base/utf8.h's utf8::decode (strict: overlong forms and
// surrogates are U+FFFD, as HarfBuzz's own decoder treats them).

// HarfBuzz general category (hb_unicode_general_category_t) and script.
int      category(uint32_t cp);
uint32_t script(uint32_t cp); // hb_script_t

bool isExtPict(uint32_t cp);      // Extended_Pictographic (emoji bases), approximated by blocks
bool isEmojiDefault(uint32_t cp); // emoji presentation by default (no VS16 needed)
bool isRegional(uint32_t cp);     // regional indicator (flags)
// Not utf8::isSpace (White_Space): NBSP, U+2007 and the newlines are left out.
bool isBreakSpace(uint32_t cp); // a break-after space (not NBSP)
bool isNewline(uint32_t cp);    // mandatory paragraph break
// Exact (UCD general category), unlike utf8::isWordChar's block approximation
// that base/ and the mrkdwn parser use without HarfBuzz.
bool isSelectWordChar(uint32_t cp); // letters, digits, marks, '_' — double-click words
bool isLetter(uint32_t cp);         // general category L* (Lu, Ll, Lt, Lm, Lo)

// Grapheme cluster boundaries: stateful scan over a code point sequence.
struct GraphemeScanner {
    // True when a cluster boundary lies before `cp`. Call for every code point
    // in order; the first call always returns true.
    bool next(uint32_t cp);

private:
    int  _prev    = -1;    // GCB class of the previous code point
    int  _riCount = 0;     // consecutive regional indicators
    bool _pictZwj = false; // saw ExtPict Extend* ZWJ, so an ExtPict joins
    bool _inPict  = false; // inside ExtPict Extend*
};

// Line breaking between two adjacent code points (grapheme boundaries only).
enum class Break : uint8_t { None, Allowed, Mandatory };
Break breakBetween(uint32_t before, uint32_t after);
bool  isIdeographic(uint32_t cp); // breaks allowed on both sides (CJK, emoji)

// Bidi: resolved embedding levels for one paragraph (no newlines inside).
// Covered: strong/weak/neutral types W1–W7, N1–N2, I1–I2, L1 for trailing
// whitespace, LRM/RLM/ALM; explicit embeddings/overrides/isolates are
// ignored (treated as boundary neutrals). `levels` gets one entry per code
// point; returns the paragraph level (0 LTR, 1 RTL; auto from the first
// strong character).
uint8_t resolveBidi(const uint32_t *cps, size_t n, uint8_t *levels, uint8_t *scratch);

} // namespace text::uni
