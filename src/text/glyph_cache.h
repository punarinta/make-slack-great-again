// Rasterised glyph cache: A8 masks and colour bitmaps packed into shared
// pages (no per-glyph allocation), keyed by (font, glyph, pixel size,
// subpixel phase), bounded by bytes with page-granular LRU eviction.
// Internal to text/.
#pragma once

#include "text/fonts.h"

namespace text::cache {

struct Glyph {
    int16_t  left = 0, top = 0; // bitmap origin relative to the pen, top = above baseline
    uint16_t w = 0, h = 0;
    uint16_t page = 0xFFFF; // 0xFFFF: nothing to draw (space, missing)
    uint16_t x = 0, y = 0;  // position inside the page
    bool     color = false;
};

// The glyph, rasterising it on a miss. The pointer and the page pixels stay
// valid until the next get() (a miss may evict pages).
const Glyph    *get(fonts::FontKey font, uint32_t glyph, uint32_t ppem64, int phase);
gfx::Mask8      mask(const Glyph &g);
gfx::BitmapView colorView(const Glyph &g);

// Marks the start of a paint pass: pages touched since are "recent".
void   tick();
void   setBudget(size_t bytes); // default 8 MB
size_t bytesUsed();
size_t glyphCount();
// Tests: every entry is reachable from its home slot, unique, on a live page,
// and the count matches. Unused by the app (dropped by the linker).
bool   consistent();

} // namespace text::cache
