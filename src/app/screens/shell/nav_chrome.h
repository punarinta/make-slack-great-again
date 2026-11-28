// The dark nav column's shared looks:
//  - the vertical gradient the rail, the sidebar and its footer share
//    (~12% lighter at the window's top edge, ~10% darker at
//    its bottom, so sibling views line up and scrolling rows never move it);
//  - NavGhostButton: the translucent white chrome of the rail's "+" and gear
//    and the footer's presence toggle.
#pragma once

#include "ui/ui.h"

namespace shell {

// Fills `v`'s bounds with the nav gradient of `token` (C::Rail or
// C::Sidebar); flat when the custom palette turned the gradient off.
void paintNavGradient(const ui::View &v, gfx::Painter &p, ui::C token);

// A 40×40 rounded-square ghost button: white-alpha fill and hairline border
// that brighten on hover, a recoloured icon at 52% of its width (or a
// hand-drawn "+" for the rail's add button).
class GhostButton : public ui::Clickable {
public:
    GhostButton(gfx::Icon icon, std::string tooltip, bool plus = false);
    void        setIcon(gfx::Icon icon);
    void        paint(gfx::Painter &p) override;
    std::string accessibleName() const override { return _tooltip; }

protected:
    void paintChrome(gfx::Painter &p);
    void paintGlyph(gfx::Painter &p, gfx::Icon icon);
    // The glyph turned clockwise about the centre (the footer's task cog).
    void paintGlyphTurned(gfx::Painter &p, gfx::Icon icon, float degrees);

private:
    uint16_t _icon;
    bool     _plus;
};

// A flat icon button: a fixed tint (no hover recolour), an optional hover/press wash, a tooltip.
class GlyphButton : public ui::Clickable {
public:
    GlyphButton(gfx::Icon icon, float box, float iconPx, ui::C tint, std::string tooltip = {});
    void        setIcon(gfx::Icon icon);
    void        setTint(ui::C tint);
    ui::C       tint() const { return _tint; }
    gfx::Icon   icon() const { return gfx::Icon(_icon); }
    void        paint(gfx::Painter &p) override;
    std::string accessibleName() const override { return _tooltip; }

private:
    uint16_t _icon;
    float    _px;
    ui::C    _tint;
};

} // namespace shell
