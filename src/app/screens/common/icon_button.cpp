#include "app/screens/common/icon_button.h"

namespace screens {

IconButton::IconButton(gfx::Icon icon, float glyph, ui::C ink)
    : _icon(icon), _glyph(glyph), _ink(ink) {}

void IconButton::setIcon(gfx::Icon icon) {
    _icon = icon;
    update();
}

void IconButton::setInk(ui::C ink) {
    if (_ink == ink)
        return;
    _ink = ink;
    update();
}

void IconButton::paint(gfx::Painter &p) {
    Clickable::paint(p);
    paintIcon(p);
}

void IconButton::paintIcon(gfx::Painter &p) const {
    gfx::drawIcon(
        p,
        _icon,
        {snapPx((width() - _glyph) / 2), snapPx((height() - _glyph) / 2), _glyph, _glyph},
        ui::color(_ink)
    );
}

} // namespace screens
