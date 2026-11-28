#include "screens/shell/overview_page.h"

#include "screens/shell/shell_text.h"

using namespace ui;

namespace shell {

namespace {

// The card geometry: the message list's avatar, the chat rows'
// rhythm (spacing.sm around a row, spacing.xs under the name line).
constexpr float kAvatar = 36, kAvatarRadius = 4, kAvatarGap = 10;
constexpr float kRowPadV = 4, kHdrGap = 2;

} // namespace

// ── TextLink ────────────────────────────────────────────────────────────────

TextLink::TextLink(std::string text, Font f, C c, bool underline)
    : _text(std::move(text)), _font(f), _color(c), _underline(underline) {
    setLook({C::None, C::None, C::None, C::None, 0});
    setRole(Role::Button);
    setCursor(plat::Cursor::Hand);
    style().row().alignSelf(Align::Start);
    _label = add<Label>();
    _label->setMaxLines(1);
    refreshLook();
}

bool TextLink::onEvent(Event &e) {
    if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave)
        refreshLook();
    return Clickable::onEvent(e);
}

void TextLink::refreshLook() {
    text::Style st = font(_font);
    // The link button turns accent.hover when hovered.
    st.color       = themed(_underline && hovered() ? C::AccentHover : _color);
    st.underline   = _underline || hovered();
    setStyledText(_label, _text, st);
}

// ── The page ────────────────────────────────────────────────────────────────

OverviewPage::OverviewPage(screens::Context &ctx, Avatars &avatars, std::string title)
    : _ctx(ctx), _avatars(avatars) {
    style().column().flex(1);
    // The thread panel's header: 48 px, the title bold; a hairline under it.
    auto *header = add<View>();
    header->style().row().height(48).padding(16, 0, 8, 0).items(Align::Center).noShrink();
    header->add<Label>(std::move(title), Font::Title)->style().flex(1);
    _rule   = add<Separator>(false, C::FormDivider);
    _scroll = add<ScrollView>();
    _scroll->style().flex(1);
    View *content = _scroll->content();
    content->style().padding(24).spacing(24);
    _status = content->add<Label>("", Font::Body, C::TextMuted);
    _status->setAlign(text::LayoutOptions::Align::Center);
    _status->style().padding(24);
    _status->setVisible(false);
    _list = content->add<View>();
    _list->style().spacing(24);
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        onChange(ch);
    });
}

OverviewPage::~OverviewPage() {
    _ctx.store.unobserve(_observer);
}

void OverviewPage::paint(gfx::Painter &p) {
    p.fillRect(bounds(), color(C::FormSunken));
    View::paint(p);
}

void OverviewPage::rebuildSoon() {
    if (_rebuildQueued)
        return;
    _rebuildQueued           = true;
    std::weak_ptr<int> alive = _alive;
    _ctx.app.platform().post([this, alive] {
        if (alive.expired())
            return;
        _rebuildQueued = false;
        if (visible())
            rebuild();
    });
}

void OverviewPage::setStatus(std::string text) {
    _statusText = std::move(text);
    _status->setText(_statusText);
    _status->setVisible(!_statusText.empty());
}

// ── Card parts ──────────────────────────────────────────────────────────────

View *cardHeader(
    View *parent, const model::Store &st, model::ConvRef conv, std::function<void()> onOpen
) {
    auto *row = parent->add<View>();
    row->style().row().spacing(4).items(Align::Center);
    row->add<IconView>(convIcon(st.conversation(conv)), 15, C::Text);
    row->add<TextLink>(st.displayName(conv), Font::BodyBold, C::Text, false)->onClick =
        std::move(onOpen);
    return row;
}

View *cardBody(View *card) {
    auto *body = card->add<View>();
    body->setBackground(C::Surface, 8);
    body->setBorder(C::Border);
    body->style().padding(24, 16, 24, 16).spacing(4);
    return body;
}

View *fillMessageRow(
    View              *row,
    Avatars           &avatars,
    const std::string &who,
    const std::string &picture,
    const std::string &time
) {
    row->style().row().spacing(kAvatarGap).padding(0, kRowPadV, 0, kRowPadV).items(Align::Start);
    auto *av = row->add<Avatar>();
    av->style().size(kAvatar, kAvatar).noShrink();
    av->setRadius(kAvatarRadius);
    av->setPlaceholder(C::PresenceAway);
    av->setInitial(who);
    if (!picture.empty())
        av->setBitmap(avatars.get(picture, int(kAvatar * 2)));
    auto *col = row->add<View>();
    col->style().flex(1).spacing(kHdrGap);
    col->style().shrink = 1;
    if (time.empty()) { // the name alone
        col->add<Label>(who, Font::BodySemibold)->setMaxLines(1);
        return col;
    }
    auto *hdr = col->add<View>();
    hdr->style().row().spacing(8).items(Align::Center);
    auto *n = hdr->add<Label>(who, Font::BodySemibold);
    n->setMaxLines(1);
    n->style().shrink = 1;
    hdr->add<Label>(time, Font::Small, C::TextMuted)->style().noShrink();
    return col;
}

} // namespace shell
