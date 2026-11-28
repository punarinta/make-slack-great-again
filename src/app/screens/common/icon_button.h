// A clickable icon: a glyph centred in its frame over the Clickable look
// (the message toolbar's buttons, the viewer's, the thread panel's, the
// emoji picker's category tabs). The frame and look are the caller's.
#pragma once

#include "gfx/gfx.h"
#include "ui/ui.h"

namespace screens {

class IconButton : public ui::Clickable {
public:
    // A `glyph`-px icon in `ink`.
    IconButton(gfx::Icon icon, float glyph, ui::C ink);

    void      setIcon(gfx::Icon icon); // repaints
    gfx::Icon icon() const { return _icon; }
    void      setInk(ui::C ink); // repaints
    ui::C     ink() const { return _ink; }

    void paint(gfx::Painter &p) override; // the look, then the icon

protected:
    // The icon alone, centred on whole device pixels.
    void paintIcon(gfx::Painter &p) const;

private:
    gfx::Icon _icon;
    float     _glyph;
    ui::C     _ink;
};

} // namespace screens
