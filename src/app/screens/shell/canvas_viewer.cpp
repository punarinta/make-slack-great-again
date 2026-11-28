#include "screens/shell/canvas_viewer.h"

#include "base/i18n.h"
#include "gfx/icons_generated.h"
#include "screens/shell/canvas_page.h"

#include <algorithm>
#include <cmath>

using namespace ui;
using i18n::tr;

namespace shell {

namespace {

constexpr float kMargin    = 40;   // backdrop visible around the panel
constexpr float kMaxPanelW = 1120; // the page's column and its side padding
constexpr float kRadius    = 8;

class CanvasViewer final : public Popup {
public:
    CanvasViewer(screens::Context &ctx, model::ConvRef conv, const model::File &f)
        : _ctx(ctx), _permalink(f.permalink) {
        setCard(false);
        setAnchor({0, 0, 0, 0}, Place::Fill);
        style().dir = Dir::None;
        _panel      = add<View>();
        _panel->setBackground(C::Surface, kRadius);
        // The bottom margin keeps the page's square corners inside the panel.
        _panel->style().column().padding(0, 0, 0, kRadius);
        _panel->setClipChildren(true);
        View *bar = _panel->add<View>();
        bar->style().row().padding(metric(M::SpaceL), metric(M::SpaceS), metric(M::SpaceS), 0);
        bar->style().spacing(metric(M::SpaceXS)).items(Align::Center);
        auto *heading =
            styledLabel(bar, tr("Canvas"), pxFont(12, text::Weight::Bold, themed(C::TextMuted)), 1);
        heading->style().flex(1);
        auto *open = bar->add<IconButton>(gfx::Icon::ExternalLink, tr("Open in browser"));
        open->style().size(32, 32);
        open->setIconSize(16);
        open->setVisible(!_permalink.empty());
        open->onClick = [this] {
            if (_ctx.openUrl && !_permalink.empty())
                _ctx.openUrl(_permalink);
        };
        auto *close = bar->add<IconButton>(gfx::Icon::X, tr("Close"));
        close->style().size(32, 32);
        close->setIconSize(16);
        close->onClick = [this] { dismiss(); };
        _page          = _panel->add<CanvasPage>(ctx);
        _page->style().flex(1);
        // Deleted from its own ⋮ menu: nothing left to show.
        _page->onDeleted = [this] { Popup::close(); };
        _page->openFile(conv, f.id, f.title.empty() ? f.name : f.title);
    }
    CanvasPage &page() { return *_page; }
    void        dismiss() {
        _page->flushPendingSave();
        Popup::close();
    }
    void layout() override {
        const float w = std::min(kMaxPanelW, std::max(200.f, width() - 2 * kMargin));
        const float h = std::max(200.f, height() - 2 * kMargin);
        _panel->setFrame({std::round((width() - w) / 2), std::round((height() - h) / 2), w, h});
    }
    void paint(gfx::Painter &p) override { p.fillRect(bounds(), color(C::ViewerBackdrop)); }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerDown) {
            if (e.button == plat::Button::Left && !_panel->frame().contains(e.pos))
                dismiss();
            return true;
        }
        if (e.type == EventType::KeyDown && e.key == plat::Key::Escape) {
            dismiss();
            return true;
        }
        return Popup::onEvent(e);
    }

private:
    screens::Context &_ctx;
    std::string       _permalink;
    View             *_panel = nullptr;
    CanvasPage       *_page  = nullptr;
};

} // namespace

ui::Popup *showCanvasViewer(
    screens::Context                             &ctx,
    ui::Window                                   &w,
    model::ConvRef                                conv,
    const model::File                            &canvas,
    std::function<void(const std::string &error)> onError
) {
    auto v            = std::make_unique<CanvasViewer>(ctx, conv, canvas);
    v->page().onError = std::move(onError);
    return w.showPopup(std::move(v));
}

} // namespace shell
