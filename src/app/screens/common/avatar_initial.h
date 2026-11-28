// The avatar placeholder: until a photo is
// there — downloading, or none at all — a rounded tile with the person's
// first letter in white bold, 0.38 × the tile height in points.
//
// The shell's avatars put it on presence.away; the message list's on a tile
// in the letter's own hue (HSL(code · 37 mod 360, 130, 100)).
#pragma once

#include "ui/ui.h"

#include <memory>
#include <string>
#include <string_view>

namespace screens {

// The first character of `name`, upper-cased (ASCII); "" when empty (the
// tile alone).
std::string avatarInitial(std::string_view name);
// The message list's tile colour for that letter.
gfx::Color  initialHue(std::string_view letter);
// An opaque colour from hue (degrees, 0-360), saturation and lightness (0-1).
gfx::Color  hsl(float hue, float s, float l);
// The letter centred in `r` (the tile is the caller's, or `bg` when not 0).
// `cache` keeps the shaped letter; reset it when the letter or style changes.
// `px`: the letter's size; 0 = the 0.38 × height (pt) rule.
void        paintInitial(
    gfx::Painter                  &p,
    const ui::View                &v,
    ui::RectF                      r,
    float                          radius,
    gfx::Color                     bg,
    const std::string             &letter,
    std::unique_ptr<text::Layout> &cache,
    float                          px = 0
);

} // namespace screens
