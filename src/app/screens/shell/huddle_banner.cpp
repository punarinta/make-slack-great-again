#include "screens/shell/huddle_banner.h"

#include "base/i18n.h"
#include "gfx/icons_generated.h"

using namespace ui;
using i18n::tr;

namespace shell {

namespace {

// The white pill: presence.online text on text.onDark, surface.raised while
// hovered or pressed; 22 px, 0/12 padding, radius 3, the caption at 600.
class JoinButton final : public Clickable {
public:
    JoinButton() {
        setLook({C::AccentText, C::FormBg, C::FormBg, C::None, 3});
        setRole(Role::Button);
        style().row().height(22).padding(12, 0, 12, 0).items(Align::Center).noShrink();
        add<Label>(tr("Join"), Font::SmallSemibold, C::Online)->setHitTransparent(true);
    }
    std::string tooltip() const override { return tr("Opens the huddle in Slack for web"); }
};

} // namespace

HuddleBanner::HuddleBanner() {
    // spacing.xl / spacing.md margins, spacing.md between.
    style().row().height(34).padding(16, 0, 12, 0).spacing(12).items(Align::Center).noShrink();
    setBackground(C::Online);
    add<IconView>(gfx::Icon::Headphones, 16, C::AccentText);
    auto *label = add<Label>(tr("A huddle is happening"), Font::SmallSemibold, C::AccentText);
    label->setMaxLines(1);
    label->style().flex(1);
    add<JoinButton>()->onClick = [this] {
        if (onJoin)
            onJoin();
    };
    setVisible(false);
}

} // namespace shell
