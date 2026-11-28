#include "screens/shell/update_bar.h"

#include "base/i18n.h"
#include "ui/controls.h"

using namespace ui;
using i18n::tr;

namespace shell {

UpdateBar::UpdateBar() {
    // 32 px; spacing.lg / spacing.md margins, the caption font at 600.
    style().row().height(32).padding(12, 0, 8, 0).spacing(12).items(Align::Center).noShrink();
    _label = add<Label>("", Font::SmallSemibold, C::UpdateBannerText);
    _label->setMaxLines(1);
    _label->style().flex(1);
    _btn          = add<Button>("", Button::Kind::Danger, Button::Form::Small);
    _btn->onClick = [this] {
        if (onRestart)
            onRestart();
    };
    setVisible(false);
}

void UpdateBar::showUpdateReady() {
#ifdef __APPLE__
    _label->setText(tr("A new version of msga is ready to install."));
    _btn->setLabel(tr("Open installer"));
#else
    _label->setText(tr("A new version of msga has been downloaded. Restart to apply."));
    _btn->setLabel(tr("Restart now"));
#endif
    setVisible(true);
}

void UpdateBar::paint(gfx::Painter &p) {
    const RectF b = bounds();
    p.fillRect(b, color(C::UpdateBannerBg));
    p.fillRect({0, b.h - 1, b.w, 1}, color(C::UpdateBannerBorder)); // border-bottom
    View::paint(p);
}

} // namespace shell
