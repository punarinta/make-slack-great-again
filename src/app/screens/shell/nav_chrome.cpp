#include "screens/shell/nav_chrome.h"

#include "gfx/icons_generated.h"

#include <algorithm>

using namespace ui;

namespace shell {

namespace {

Color scaled(Color c, float f) {
    const auto ch = [&](int s) {
        return uint32_t(std::clamp(float((c >> s) & 0xff) * f, 0.f, 255.f)) << s;
    };
    return (c & 0xff000000u) | ch(16) | ch(8) | ch(0);
}

// White-alpha overlays for the ghost buttons: not tokens, they read on any
// nav tint.
constexpr Color kGhostIcon = 0xffb4a5b4, kGhostIconHover = 0xfff5f0f5;

} // namespace

void paintNavGradient(const View &v, gfx::Painter &p, C token) {
    const Color base = color(token);
    const bool  flat =
        palette(app() && app()->dark()) == Palette::Custom && !customPalette().gradient;
    if (flat || !v.window()) {
        p.fillRect(v.bounds(), base);
        return;
    }
    // Local coordinates of the window's top and bottom edges.
    const float top = -v.mapToWindow({0, 0}).y, h = std::max(1.f, v.window()->size().h);
    p.fillRectGradient(
        v.bounds(), {0, top}, scaled(base, 1.12f), {0, top + h}, scaled(base, 0.90f)
    );
}

GhostButton::GhostButton(gfx::Icon icon, std::string tooltip, bool plus)
    : _icon(uint16_t(icon)), _plus(plus) {
    style().size(40, 40).noShrink();
    setLook({C::None, C::None, C::None, C::None, 10});
    setTooltip(std::move(tooltip));
    setRole(Role::Button);
    setHoverRepaint(true);
}

void GhostButton::setIcon(gfx::Icon icon) {
    _icon = uint16_t(icon);
    update();
}

void GhostButton::paint(gfx::Painter &p) {
    paintChrome(p);
    paintGlyph(p, gfx::Icon(_icon));
}

void GhostButton::paintChrome(gfx::Painter &p) {
    const bool  hov = hovered();
    const RectF r{0.75f, 0.75f, width() - 1.5f, height() - 1.5f};
    p.fillRoundRect(r, 10, hov ? 0x37ffffffu : 0x16ffffffu);
    p.strokeRoundRect(r, 10, 1.5f, hov ? 0xb4ffffffu : 0x5affffffu);
}

void GhostButton::paintGlyph(gfx::Painter &p, gfx::Icon icon) {
    const Color ink = hovered() ? kGhostIconHover : kGhostIcon;
    if (_plus) { // two opaque strokes: no alpha seam where they cross
        const float cx = width() / 2, cy = height() / 2, arm = 5;
        p.drawLine({cx - arm, cy}, {cx + arm, cy}, 2.5f, ink);
        p.drawLine({cx, cy - arm}, {cx, cy + arm}, 2.5f, ink);
        return;
    }
    const float d = width() * 0.52f;
    gfx::drawIcon(p, icon, {snapPx((width() - d) / 2), snapPx((height() - d) / 2), d, d}, ink);
}

void GhostButton::paintGlyphTurned(gfx::Painter &p, gfx::Icon icon, float degrees) {
    const float d = width() * 0.52f;
    gfx::drawIconRotated(
        p,
        icon,
        {(width() - d) / 2, (height() - d) / 2, d, d},
        hovered() ? kGhostIconHover : kGhostIcon,
        degrees
    );
}

GlyphButton::GlyphButton(gfx::Icon icon, float box, float iconPx, C tint, std::string tooltip)
    : _icon(uint16_t(icon)), _px(iconPx), _tint(tint) {
    style().size(box, box).noShrink();
    setLook({C::None, C::None, C::None, C::None, 0});
    setTooltip(std::move(tooltip));
    setRole(Role::Button);
}

void GlyphButton::setIcon(gfx::Icon icon) {
    _icon = uint16_t(icon);
    update();
}

void GlyphButton::setTint(C tint) {
    _tint = tint;
    update();
}

void GlyphButton::paint(gfx::Painter &p) {
    Clickable::paint(p);
    gfx::drawIcon(
        p,
        gfx::Icon(_icon),
        {snapPx((width() - _px) / 2), snapPx((height() - _px) / 2), _px, _px},
        color(enabled() ? _tint : C::DropArrow)
    );
}

} // namespace shell
