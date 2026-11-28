#include "app/screens/common/avatar_initial.h"

#include "base/utf8.h"

#include <algorithm>
#include <cmath>

namespace screens {

std::string avatarInitial(std::string_view name) {
    if (name.empty())
        return {};
    size_t   i  = 0;
    uint32_t cp = utf8::decode(name, i);
    if (cp >= 'a' && cp <= 'z')
        cp -= 32;
    std::string out;
    utf8::append(out, cp);
    return out;
}

gfx::Color initialHue(std::string_view letter) {
    size_t         i  = 0;
    const uint32_t cp = letter.empty() ? '?' : utf8::decode(letter, i);
    // HSL(h, 130, 100) on 0-255 scales; the hue comes from the code point's
    // low 16 bits (the same number for the BMP).
    return hsl(float((cp & 0xffff) * 37 % 360), 130.f / 255, 100.f / 255);
}

gfx::Color hsl(float hue, float s, float l) {
    const float h       = hue / 60;
    const float c       = (1 - std::fabs(2 * l - 1)) * s;
    const float x       = c * (1 - std::fabs(std::fmod(h, 2.f) - 1));
    const float m       = l - c / 2;
    // Which of c, x and 0 each channel takes, by the hue's sixth.
    const float t[6][3] = {{c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x}};
    const int   sec     = int(h) % 6;
    const auto  ch = [m](float v) { return uint32_t(std::clamp(v + m, 0.f, 1.f) * 255 + 0.5f); };
    return gfx::rgb((ch(t[sec][0]) << 16) | (ch(t[sec][1]) << 8) | ch(t[sec][2]));
}

void paintInitial(
    gfx::Painter                  &p,
    const ui::View                &v,
    ui::RectF                      r,
    float                          radius,
    gfx::Color                     bg,
    const std::string             &letter,
    std::unique_ptr<text::Layout> &cache,
    float                          px
) {
    if (bg)
        p.fillRoundRect(r, radius, bg);
    if (letter.empty() || r.h <= 0)
        return;
    if (!cache) {
        // setPointSizeF(h · 0.38): points at 96 dpi are 4/3 px.
        if (px <= 0)
            px = r.h * 0.38f * 4 / 3;
        cache = text::layoutPlain(
            letter, ui::pxFont(px, text::Weight::Bold, 0xffffffffU), v.windowScale()
        );
    }
    const text::Layout &l = *cache;
    l.paint(
        p,
        v.snapPx(
            {r.x + std::floor((r.w - l.width()) / 2), r.y + std::floor((r.h - l.height()) / 2)}
        )
    );
}

} // namespace screens
