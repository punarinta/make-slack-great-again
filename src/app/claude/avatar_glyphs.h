// Glyph avatars: a white line glyph (Lucide, ISC licence — https://lucide.dev/)
// on a rounded colour tile, like the Claude Code teammates' pictures. The
// glyphs and colours on offer, and the SVG for one pick — written to a file
// for the avatar decoder, and drawn by pickers directly.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace claude::avatar_glyphs {

struct Glyph {
    const char *id;       // Lucide's name: "code-xml"
    const char *elements; // its SVG elements, in Lucide's 24-unit box
};

std::span<const Glyph>    glyphs();
// The tile colours on offer, 0xRRGGBB; the first is Claude orange, the last a
// neutral slate (former teammates).
std::span<const uint32_t> colors();
bool                      hasGlyph(std::string_view id);

// A 128-unit tile of `color` (0xRRGGBB) with glyph `id` on it; the first glyph
// for an unknown id.
std::string svg(std::string_view id, uint32_t color);

} // namespace claude::avatar_glyphs
