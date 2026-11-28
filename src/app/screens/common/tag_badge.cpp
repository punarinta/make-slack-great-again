#include "app/screens/common/tag_badge.h"

#include "base/i18n.h"

#include <cmath>
#include <memory>

namespace screens {

using i18n::tr;
using ui::C;

namespace {

// The tag badge: a 14-px pill, 4 px around the text, radius 2, the
// app font at 0.62 in bold.
class TagBadge final : public ui::View {
public:
    TagBadge(TagKind kind, bool sidebar) : _kind(kind), _sidebar(sidebar) { style().noShrink(); }
    void      styleChanged() override { _l.reset(), View::styleChanged(); }
    ui::SizeF measureContent(float, float) override {
        build();
        return {std::ceil(_l->width()) + 8, 14};
    }
    void paint(gfx::Painter &p) override {
        build();
        gfx::Color bg = 0;
        switch (_kind) {
        case TagKind::App:
            bg = ui::byTheme(0x1effffffU, 0x211d1c1dU);
            break;
        case TagKind::Ext:
            bg = !_sidebar     ? ui::byTheme(0x1ee6c98aU, 0x26c6920aU)
                 : lightRail() ? 0x26c6920aU
                               : 0x26e6c98aU;
            break;
        case TagKind::Vip:
            bg = !_sidebar     ? ui::byTheme(0x1eb595f8U, 0x267c3aedU)
                 : lightRail() ? 0x267c3aedU
                               : 0x26b595f8U;
            break;
        }
        p.fillRoundRect(bounds(), 2, bg);
        _l->paint(
            p,
            snapPx(
                {std::floor((width() - _l->width()) / 2), std::floor((height() - _l->height()) / 2)}
            )
        );
    }

private:
    // The chats list follows the rail: a light one gets the dark ink.
    static bool lightRail() {
        const gfx::Color c = ui::color(C::Sidebar);
        const uint32_t   r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
        return r * 299 + g * 587 + b * 114 > 128000;
    }
    void build() {
        if (_l)
            return;
        gfx::Color  fg = 0;
        std::string text;
        switch (_kind) {
        case TagKind::App:
            fg   = ui::byTheme(0xffa8a8a8U, 0xff616061U);
            text = tr("APP");
            break;
        case TagKind::Ext:
            fg   = !_sidebar     ? ui::byTheme(0xffd9b45cU, 0xff8a6508U)
                   : lightRail() ? 0xff8a6508U
                                 : 0xffe6c98aU;
            text = tr("EXT");
            break;
        case TagKind::Vip:
            fg   = !_sidebar     ? ui::byTheme(0xffb595f8U, 0xff7c3aedU)
                   : lightRail() ? 0xff7c3aedU
                                 : 0xffb595f8U;
            text = tr("VIP");
            break;
        }
        _l = text::layoutPlain(text, ui::pxFont(15 * 0.62f, text::Weight::Bold, fg), windowScale());
    }
    std::unique_ptr<text::Layout> _l;
    TagKind                       _kind;
    bool                          _sidebar;
};

} // namespace

ui::View *addTagBadge(ui::View *parent, TagKind kind, bool sidebar) {
    return parent->add<TagBadge>(kind, sidebar);
}

} // namespace screens
