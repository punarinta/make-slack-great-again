#include "screens/shell/composer_popups.h"
#include "screens/shell/gif_search.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"
#include "ui/datetime.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/shortcuts.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

using namespace ui;
using gfx::Icon;
using i18n::tr;

namespace shell {

namespace {

// The list card: surface.raised, a divider.strong hairline, radius 6, no
// shadow.
void card(View *v, float radius) {
    v->setBackground(C::FormBg, radius);
    v->setBorder(C::FormDividerStrong);
}

// PickRow::paintEnterBadge: a bordered caption chip, 22 px tall.
View *enterBadge(View *row) {
    auto *b = row->add<View>();
    b->style().height(22).padding(9, 0).stack().items(Align::Center).noShrink();
    card(b, 4);
    b->add<Label>(tr("Enter"), Font::Small, C::FormTextMuted);
    b->setVisible(false);
    return b;
}

constexpr float kMentionRowH = 38, kChannelRowH = 38, kPlainRowH = 30, kCommandRowH = 56;
// Five command rows show before the list scrolls,
// eight of the others; a command row is 460 wide (+ the 4 px margins).
constexpr int   kCmdVisible = 5, kPlainVisible = 8;
constexpr float kCommandW = 460;

// A command row: the 36 px icon, a bold "/name" with the dimmed usage
// hint, then the bold source and the dimmed description. Hover paints the
// accent (link blue) with light text; keyboard selection (checked) the
// subtle gray with the "Enter" chip on the right.
class CommandRow final : public Clickable {
public:
    CommandRow(const PickList::Item &it, Avatars *avatars) : _it(it) {
        style().row().height(kCommandRowH).padding(14, 0, 14, 0).items(Align::Center).noShrink();
        setLook({C::None, C::None, C::None, C::None, 6});
        if (it.avatar.empty() && it.source == "Slack") {
            // Slack's built-in commands: the Slack mark, inset 1 px.
            auto *mark = add<IconView>(gfx::Icon::SlackMark, 34, C::Text); // its own colours
            mark->style().margins(1, 1, 1, 1);
            mark->setHitTransparent(true);
        } else {
            auto *av = add<Avatar>();
            av->style().size(36, 36).noShrink();
            av->setRadius(8);
            av->setPlaceholder(C::PresenceAway);
            av->setInitial(it.source.empty() ? std::string_view("A") : std::string_view(it.source));
            av->setHitTransparent(true);
            if (avatars && !it.avatar.empty())
                av->setBitmap(avatars->get(it.avatar, 72));
        }
        add<View>()->style().flex(1);
        _badge = enterBadge(this);
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave) {
            _hover = e.type == EventType::PointerEnter;
            stateChanged();
            update();
        }
        return Clickable::onEvent(e);
    }
    void stateChanged() override {
        _badge->setVisible(checked() && !_hover);
        _lines[0].reset();
    }
    void styleChanged() override {
        _lines[0].reset();
        Clickable::styleChanged();
    }
    void paint(gfx::Painter &p) override {
        if (_hover)
            p.fillRoundRect(bounds(), 6, color(C::Link));
        else if (checked())
            p.fillRoundRect(bounds(), 6, color(C::FormHighlight));
        const float left  = 14 + 36 + 12;
        const float right = _badge->visible() ? _badge->frame().x - 12 : width() - 14;
        const float w     = std::max(0.f, right - left);
        if (!_lines[0] || _builtW != w) {
            _builtW                 = w;
            const float          k  = windowScale();
            const Color          hi = color(_hover ? C::TooltipText : C::FormText);
            const Color          lo = color(_hover ? C::OnDarkDim : C::FormTextFaint);
            text::AttributedText t;
            t.append(_it.title, pxFont(14, text::Weight::Bold, hi));
            if (!_it.usage.empty())
                t.append(str::concat({" ", _it.usage}), pxFont(14, text::Weight::Regular, lo));
            _lines[0] = oneLineLayout(t, w, k);
            text::AttributedText s2;
            if (!_it.source.empty())
                s2.append(
                    _it.source,
                    pxFont(
                        12, text::Weight::Bold, color(_hover ? C::TooltipText : C::FormTextMuted)
                    )
                );
            if (!_it.subtitle.empty())
                s2.append(
                    _it.source.empty() ? _it.subtitle : str::concat({"  \xC2\xB7  ", _it.subtitle}),
                    pxFont(12, text::Weight::Regular, lo)
                );
            _lines[1] = oneLineLayout(s2, w, k);
        }
        // Centred vertically in the 20 px title box at y 10, the 18 px one at 30.
        _lines[0]->paint(p, snapPx({left, std::floor(20 - _lines[0]->height() / 2)}));
        _lines[1]->paint(p, snapPx({left, std::floor(39 - _lines[1]->height() / 2)}));
    }

private:
    PickList::Item                _it;
    View                         *_badge = nullptr;
    std::unique_ptr<text::Layout> _lines[2];
    float                         _builtW = -1;
    bool                          _hover  = false;
};

} // namespace

// ── PickList ────────────────────────────────────────────────────────────────

PickList::PickList(Avatars *avatars, std::vector<Item> items, bool wide, Pick onPick)
    : _avatars(avatars), _items(std::move(items)), _onPick(std::move(onPick)) {
    setModal(false); // the editor keeps the keyboard
    setCard(false);
    card(this, 6);
    style().padding(4);
    applyWidth(wide);
    _scroll = add<ScrollView>();
    _scroll->content()->style().spacing(1);
    build();
}

void PickList::applyWidth(bool wide) {
    _wide               = wide;
    const bool commands = !_items.empty() && _items[0].kind == Item::Kind::Command;
    // The command list is sized to its rows (460) and the scroll bar's room.
    style().width(
        commands ? kCommandW + 8 + (_items.size() > size_t(kCmdVisible) ? 10 : 0)
        : wide   ? 560
                 : 360
    );
}

void PickList::update(std::vector<Item> items, bool wide, PointF anchor) {
    if (items != _items || wide != _wide) {
        _items = std::move(items);
        applyWidth(wide);
        build();
    } else {
        select(0);
    }
    setAnchor({anchor.x, anchor.y, 0, 0}, Place::Above);
}

PickList *PickList::show(
    Window &w, Avatars *avatars, PointF anchor, std::vector<Item> items, bool wide, Pick onPick
) {
    auto *p = new PickList(avatars, std::move(items), wide, std::move(onPick));
    p->setAnchor({anchor.x, anchor.y, 0, 0}, Place::Above);
    w.showPopup(std::unique_ptr<Popup>(p));
    return p;
}

void PickList::build() {
    ++_builds;
    View *c = _scroll->content();
    c->clearChildren();
    _rows.clear();
    _badges.clear();
    float      h        = 0;
    const bool commands = !_items.empty() && _items[0].kind == Item::Kind::Command;
    const int  shown    = std::min<int>(int(_items.size()), commands ? kCmdVisible : kPlainVisible);
    for (size_t i = 0; i < _items.size() && i < 50; ++i) {
        const Item &it = _items[i];
        if (it.kind == Item::Kind::Command) {
            auto     *row = c->add<CommandRow>(it, _avatars);
            const int idx = int(i);
            row->onClick  = [this, idx] {
                _sel = idx;
                confirm();
            };
            _rows.push_back(row);
            _badges.push_back(nullptr); // the row shows its own (not while hovered)
            if (int(i) < shown)
                h += kCommandRowH + (i ? 1 : 0);
            continue;
        }
        auto       *row   = c->add<Clickable>();
        const bool  plain = it.kind == Item::Kind::Plain;
        const float rowH  = plain                            ? kPlainRowH
                            : it.kind == Item::Kind::Channel ? kChannelRowH
                                                             : kMentionRowH;
        row->setLook(
            {C::None,
             plain ? C::FormHighlight : C::None,
             C::None,
             plain ? C::AccentSubtle : C::FormHighlight,
             plain ? 4.f : 6.f}
        );
        row->style().row().height(rowH).items(Align::Center).noShrink();
        const int idx = int(i);
        row->onClick  = [this, idx] {
            _sel = idx;
            confirm();
        };
        if (plain) {
            row->style().padding(10, 0);
            row->add<Label>(it.display, Font::Control, C::FormText)->setMaxLines(1);
            _badges.push_back(nullptr);
        } else if (it.kind == Item::Kind::Channel) {
            row->style().padding(12, 0).spacing(12);
            row->add<IconView>(it.privateChannel ? Icon::Lock : Icon::Hash, 22, C::FormText);
            auto *n = row->add<Label>(it.title, Font::BodyBold, C::FormText);
            n->setMaxLines(1);
            n->style().flex(1);
            _badges.push_back(enterBadge(row));
        } else {
            row->style().padding(8, 0).spacing(8);
            if (it.kind == Item::Kind::Alias) {
                auto *box = row->add<View>();
                box->style().size(26, 26).stack().items(Align::Center).noShrink();
                box->add<IconView>(Icon::Megaphone, 20, C::FormTextMuted);
            } else {
                auto *av = row->add<Avatar>();
                av->style().size(26, 26).noShrink();
                av->setRadius(6);
                av->setPlaceholder(C::PresenceAway);
                if (_avatars)
                    av->setBitmap(_avatars->get(it.avatar, 52));
            }
            auto *n = row->add<Label>(it.title, Font::BodyBold, C::FormText);
            n->setMaxLines(1);
            n->style().noShrink();
            if (it.bot) {
                auto *app = row->add<View>();
                app->style().height(15).padding(5, 0).stack().items(Align::Center).noShrink();
                app->setBackground(C::FormHighlightStrong, 3);
                app->add<Label>(tr("APP"), Font::Tiny, C::FormTextMuted);
            }
            if (it.presence) {
                // A 9 px dot right after the name: green, a DND bar, or a ring.
                class Dot final : public View {
                public:
                    explicit Dot(int p) : kind(p) { style().size(9, 9).noShrink(); }
                    void paint(gfx::Painter &p) override {
                        const PointF c{4.5f, 4.5f};
                        if (kind == 1)
                            p.fillCircle(c, 4.5f, color(C::Online));
                        else if (kind == 3) {
                            p.fillCircle(c, 4.5f, color(C::FormDivider));
                            p.drawLine(
                                {c.x - 2, c.y}, {c.x + 2, c.y}, 1.5f, color(C::PresenceAway)
                            );
                        } else
                            p.strokeCircle(c, 3.75f, 1.5f, color(C::FormTextFaint));
                    }
                    int kind;
                };
                row->add<Dot>(it.presence);
            }
            if (!it.subtitle.empty()) {
                auto *s = row->add<Label>(
                    it.subtitle,
                    Font::Field,
                    it.kind == Item::Kind::Alias ? C::FormTextMuted : C::FormTextFaint
                );
                s->setMaxLines(1);
                s->style().shrink = 1;
            }
            row->add<View>()->style().flex(1);
            if (!it.status.empty())
                row->add<Label>(it.status, Font::Small, C::FormTextMuted)->style().noShrink();
            _badges.push_back(enterBadge(row));
        }
        _rows.push_back(row);
        if (int(i) < shown)
            h += rowH + (i ? 1 : 0);
    }
    _scroll->style().height(h);
    select(0);
}

float PickList::scrollOffset() const {
    return _scroll->scrollOffset();
}

void PickList::select(int i) {
    if (_rows.empty())
        return;
    _sel = std::clamp(i, 0, int(_rows.size()) - 1);
    for (size_t k = 0; k < _rows.size(); ++k) {
        _rows[k]->setChecked(int(k) == _sel);
        if (_badges[k])
            _badges[k]->setVisible(int(k) == _sel);
    }
    _scroll->ensureVisible(_rows[size_t(_sel)], 0);
}

bool PickList::handleKey(const Event &e) {
    if (e.type != EventType::KeyDown || _rows.empty())
        return false;
    const int n = int(_rows.size());
    switch (e.key) {
    case plat::Key::Escape:
        close();
        return true;
    case plat::Key::Up:
        select((_sel - 1 + n) % n);
        return true;
    case plat::Key::Down:
        select((_sel + 1) % n);
        return true;
    case plat::Key::Tab:
    case plat::Key::Enter:
    case plat::Key::KpEnter:
        if (e.mods & (plat::ModShift | plat::ModCtrl | plat::ModAlt))
            return false;
        confirm();
        return true;
    default:
        return false;
    }
}

void PickList::confirm() {
    if (_sel < 0 || _sel >= int(_items.size())) {
        close();
        return;
    }
    const Item it   = _items[size_t(_sel)];
    Pick       pick = _onPick;
    close();
    if (pick)
        pick(it);
}

// ── Link popup ──────────────────────────────────────────────────────────────

namespace {

// A StyledLineEdit: a bordered one-line field.
TextEdit *field(View *parent, const char *placeholder, float minW, bool small = false) {
    auto *box = parent->add<TextField>(
        placeholder, small ? TextField::Size::Small : TextField::Size::Normal
    );
    box->setQuiet();
    box->style().minW = minW;
    return &box->edit();
}

Popup *framedPopup() {
    auto *p = new Popup();
    p->setCard(false);
    card(p, 8);
    p->style().padding(12).spacing(8);
    return p;
}

} // namespace

Popup *showLinkPopup(
    Window                                                       &w,
    RectF                                                         anchor,
    std::string                                                   selected,
    std::function<void(const std::string &, const std::string &)> done
) {
    Popup *p = framedPopup();
    p->style().width(12 + 280 + 12); // the fields' 280 px minimum, the margins
    p->add<Label>(tr("URL"), Font::Small, C::FormTextMuted);
    TextEdit *url = field(p, "https://", 280);
    p->add<Label>(tr("Display text"), Font::Small, C::FormTextMuted);
    TextEdit *text = field(p, "", 280);
    text->setText(selected);
    auto *row = p->add<View>();
    row->style().row().spacing(8).justifyContent(Justify::End);
    auto *cancel = row->add<Button>(tr("Cancel"), Button::Kind::Secondary, Button::Form::Small);
    auto *insert = row->add<Button>(tr("Insert"), Button::Kind::Primary, Button::Form::Small);
    auto  apply  = [p, url, text, done] {
        const std::string u(str::trim(url->text()));
        if (u.empty()) {
            url->focus();
            return;
        }
        const std::string l(str::trim(text->text()));
        p->close();
        if (done)
            done(u, l);
    };
    cancel->onClick = [p] { p->close(); };
    insert->onClick = apply;
    url->onSubmit   = [text] { return text->focus(), true; };
    text->onSubmit  = [apply] { return apply(), true; };
    p->setAnchor(anchor, Popup::Place::Below);
    w.showPopup(std::unique_ptr<Popup>(p));
    url->focus();
    return p;
}

// ── Schedule popup ──────────────────────────────────────────────────────────

Popup *showSchedulePopup(Window &w, RectF anchor, std::function<void(int64_t)> done) {
    Popup *p = framedPopup();
    p->style().width(12 + 240 + 12);
    p->add<Label>(tr("Send at"), Font::Small, C::FormTextMuted);
    // A date-time field: calendar drop-down, the language's order and
    // the 12/24-hour clock; an hour from now, never under a minute out.
    auto *when = p->add<DateTimeField>(DateTimeField::Kind::DateTime);
    when->setMinimumValue(base::nowSecs() + 60);
    when->setValue(base::nowSecs() + 3600);
    auto *row = p->add<View>();
    row->style().row().spacing(8).justifyContent(Justify::End);
    auto *cancel   = row->add<Button>(tr("Cancel"), Button::Kind::Secondary, Button::Form::Small);
    auto *schedule = row->add<Button>(tr("Schedule"), Button::Kind::Primary, Button::Form::Small);
    auto  apply    = [p, when, done] {
        // Left open past its minimum: the time is behind us now. Refuse it
        // visibly, moved up to the new earliest time, instead of posting
        // something Slack would reject.
        const int64_t min = base::nowSecs() + 60;
        if (when->value() < min) {
            when->setMinimumValue(min);
            when->setInvalid(true);
            when->focus();
            return;
        }
        const int64_t ts = when->value();
        p->close();
        if (done)
            done(ts);
    };
    cancel->onClick   = [p] { p->close(); };
    schedule->onClick = apply;
    p->setAnchor(anchor, Popup::Place::Above);
    w.showPopup(std::unique_ptr<Popup>(p));
    when->focus();
    return p;
}

// ── GIF picker ──────────────────────────────────────────────────────────────

namespace {

// The GIF grid: two masonry columns of rounded, cover-fit animated
// previews on a sunken placeholder, the hovered or selected one ringed in
// the accent.
class GifGrid final : public View {
public:
    explicit GifGrid(screens::Context &ctx) : _ctx(ctx) { style().dir = Dir::None; }
    void setGifs(std::vector<model::Backend::Gif> gifs) {
        clearChildren();
        _gifs = std::move(gifs);
        _sel  = -1;
#ifdef MSGA_HAVE_MESSAGES
        for (const auto &g : _gifs) {
            auto *img = add<screens::CachedImage>(
                _ctx.images, g.preview, screens::ImageCache::Shape::Rounded, 6
            );
            img->setAnimated(true);
            img->setHitTransparent(true);
        }
#endif
        invalidateLayout();
    }
    const std::vector<model::Backend::Gif> &gifs() const { return _gifs; }
    int                                     selected() const { return _sel; }
    void                                    setSelected(int i) {
        _sel = i;
        update();
    }
    std::function<void(int)> onPick;

    SizeF measureContent(float availW, float) override {
        place(availW);
        return {availW, _h};
    }
    void layout() override {
        place(width());
        for (size_t i = 0; i < childCount() && i < _rects.size(); ++i)
            child(i)->setFrame(_rects[i]);
    }
    void paint(gfx::Painter &p) override {
        for (const RectF &r : _rects) // the placeholder under every cell
            p.fillRoundRect(r, 6, color(C::FormSunken));
    }
    void paintOver(gfx::Painter &p) override {
        for (int i : {_sel, _hover})
            if (i >= 0 && size_t(i) < _rects.size()) {
                const RectF r = _rects[size_t(i)];
                p.strokeRoundRect({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 6, 2, color(C::Accent));
            }
    }
    uint8_t cursorAt(PointF p) const override {
        return at(p) >= 0 ? uint8_t(plat::Cursor::Hand) : View::cursorAt(p);
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerMove || e.type == EventType::PointerLeave) {
            const int h = e.type == EventType::PointerLeave ? -1 : at(e.pos);
            if (h != _hover) {
                _hover = h;
                update();
            }
            return false;
        }
        if (e.type == EventType::PointerDown && e.button == plat::Button::Left) {
            const int i = at(e.pos);
            if (i >= 0 && onPick)
                onPick(i);
            return i >= 0;
        }
        return false;
    }

private:
    static constexpr float kGap = 6, kMargin = 6, kMinH = 60, kMaxH = 200;
    void                   place(float w) {
        _rects.clear();
        const float colW    = std::max(40.f, std::floor((w - kMargin * 2 - kGap) / 2));
        float       colY[2] = {kMargin, kMargin};
        for (const auto &g : _gifs) {
            const int col = colY[1] < colY[0] ? 1 : 0; // the shorter column
            float     h = g.width > 0 ? std::round(colW * float(g.height) / float(g.width)) : colW;
            h           = std::clamp(h, kMinH, kMaxH);
            _rects.push_back({kMargin + col * (colW + kGap), colY[col], colW, h});
            colY[col] += h + kGap;
        }
        _h = _gifs.empty() ? 0 : std::max(colY[0], colY[1]) - kGap + kMargin;
    }
    int at(PointF p) const {
        for (size_t i = 0; i < _rects.size(); ++i) {
            const RectF r = _rects[i];
            if (p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h)
                return int(i);
        }
        return -1;
    }
    screens::Context                &_ctx;
    std::vector<model::Backend::Gif> _gifs;
    std::vector<RectF>               _rects;
    float                            _h   = 0;
    int                              _sel = -1, _hover = -1;
};

class GifPicker final : public Popup {
public:
    GifPicker(screens::Context &ctx, GifHooks h) : _ctx(ctx), _h(std::move(h)) {
        setCard(false);
        card(this, 8);
        style().width(354).padding(8).spacing(8);
        build();
    }
    ~GifPicker() override {
        if (_debounce)
            _ctx.app.platform().cancelTimer(_debounce);
    }
    TextEdit *search() const { return _search; }

private:
    bool service() const { return _ctx.backend.gifSearchAvailable(); }
    void build() {
        clearChildren();
        _grid             = nullptr;
        const bool hasKey = service() || (_h.key && !_h.key().empty());
        if (!hasKey || _needsKey) { // the setup form is the whole panel
            style().h  = kAuto;
            auto *page = add<View>();
            page->style().padding(12, 8, 12, 12).spacing(8);
            page->add<Label>(
                tr("Searching GIFs needs a GIPHY API key.\n\nCreate a free one \xE2\x80\x94 it "
                   "takes a minute \xE2\x80\x94 then paste it below. You can change it later in "
                   "Settings \xE2\x86\x92 System."),
                Font::Control,
                C::FormTextMuted
            );
            auto *link = page->add<Clickable>();
            link->setLook({C::None, C::None, C::None, C::None, 0});
            link->style().alignSelf(Align::Start);
            link->add<Label>(tr("Get a free GIPHY key\xE2\x80\xA6"), Font::Control, C::FormLink);
            link->onClick = [this] {
                if (_h.openUrl)
                    _h.openUrl("https://developers.giphy.com/dashboard/");
            };
            _key = field(page, tr("Paste your GIPHY API key"), 0, true);
            _key->setMasked(true);
            // A key GIPHY refused comes back here with the reason.
            _error = page->add<Label>(_keyError, Font::Small, C::FormError);
            _error->setVisible(!_keyError.empty());
            auto *row = page->add<View>();
            row->style().row().items(Align::End);
            auto *save = row->add<Button>(tr("Save"), Button::Kind::Primary, Button::Form::Small);
            save->onClick  = [this] { saveKey(); };
            _key->onSubmit = [this] { return saveKey(), true; };
            row->add<View>()->style().flex(1);
            row->add<Label>("Powered by GIPHY", Font::Tiny, C::FormTextFaint)
                ->style()
                .margins(0, 16, 0, 0);
            _search = nullptr;
            return;
        }
        style().h = 460;
        auto *box =
            add<TextField>(tr("Search GIFs"), TextField::Size::Normal, uint16_t(Icon::Search));
        box->setQuiet();
        _search           = &box->edit();
        _search->onChange = [this] { schedule(); };
        _search->onSubmit = [this] {
            // Enter with a selection sends it; otherwise search now.
            if (_grid && _grid->selected() >= 0)
                pick(_grid->selected());
            else
                query();
            return true;
        };
        auto *body = add<View>();
        body->style().stack().flex(1);
        auto *scroll  = body->add<ScrollView>();
        _grid         = scroll->content()->add<GifGrid>(_ctx);
        _grid->onPick = [this](int i) { pick(i); };
        _scroll       = scroll;
        _msg          = body->add<Label>("", Font::Control, C::FormTextMuted);
        _msg->setAlign(text::LayoutOptions::Align::Center);
        _msg->style().alignSelf(Align::Center);
        auto *attr = add<Label>("Powered by GIPHY", Font::Tiny, C::FormTextFaint);
        attr->setAlign(text::LayoutOptions::Align::Right);
        query();
    }
    void state(const char *message) { // nullptr: the results
        _scroll->setVisible(!message);
        _msg->setVisible(message);
        if (message)
            _msg->setText(message);
    }
    void schedule() { // a 450 ms debounce, "Searching…" at once
        state(tr("Searching\xE2\x80\xA6"));
        auto &pa = _ctx.app.platform();
        if (_debounce)
            pa.cancelTimer(_debounce);
        std::weak_ptr<int> alive = _alive;
        _debounce                = pa.addTimer(450, false, [this, alive] {
            if (alive.expired())
                return;
            _debounce = 0;
            query();
        });
    }
    void query() {
        state(tr("Searching\xE2\x80\xA6"));
        const std::string  q(str::trim(_search->text()));
        std::weak_ptr<int> alive = _alive;
        _pending                 = q;
        if (!service()) { // GIPHY itself, with the key from Settings
            if (!_giphy)
                _giphy = std::make_unique<GifSearch>(_ctx.app.platform());
            _giphy->search(
                q, _h.key ? _h.key() : std::string(), [this, alive, q](GifSearch::Result r) {
                    if (alive.expired() || q != _pending)
                        return;
                    if (r.keyRejected) { // back to the setup form, the reason under the field
                        _needsKey = true;
                        _keyError = std::move(r.error);
                        build();
                        return;
                    }
                    if (!r.error.empty()) {
                        _message = std::move(r.error);
                        return state(_message.c_str());
                    }
                    if (r.gifs.empty())
                        return state(tr("No GIFs found."));
                    _grid->setGifs(std::move(r.gifs));
                    state(nullptr);
                }
            );
            return;
        }
        _ctx.backend.searchGifs(q, [this, alive, q](std::vector<model::Backend::Gif> gifs) {
            if (alive.expired() || q != _pending)
                return;
            if (gifs.empty())
                return state(tr("No GIFs found."));
            _grid->setGifs(std::move(gifs));
            state(nullptr);
        });
    }
    void pick(int i) {
        if (!_grid || i < 0 || size_t(i) >= _grid->gifs().size())
            return;
        const auto g  = _grid->gifs()[size_t(i)];
        auto       cb = _h.picked;
        close();
        if (cb)
            cb(g.url, g.title);
    }
    void saveKey() {
        const std::string k(str::trim(_key->text()));
        if (k.empty()) {
            _error->setText(tr("Paste a key first."));
            _error->setVisible(true);
            return;
        }
        if (_h.setKey)
            _h.setKey(k);
        _needsKey = false;
        _keyError.clear();
        build();
        if (_search)
            _search->focus();
    }
    screens::Context          &_ctx;
    GifHooks                   _h;
    TextEdit                  *_key = nullptr, *_search = nullptr;
    Label                     *_error = nullptr, *_msg = nullptr;
    GifGrid                   *_grid   = nullptr;
    ScrollView                *_scroll = nullptr;
    std::string                _pending, _keyError, _message;
    std::unique_ptr<GifSearch> _giphy;
    bool                       _needsKey = false;
    plat::TimerId              _debounce = 0;
    std::shared_ptr<int>       _alive    = std::make_shared<int>(0);
};

} // namespace

Popup *showGifPicker(Window &w, RectF anchor, screens::Context &ctx, GifHooks hooks) {
    auto *p = new GifPicker(ctx, std::move(hooks));
    p->setAnchor(anchor, Popup::Place::Above);
    w.showPopup(std::unique_ptr<Popup>(p));
    if (TextEdit *s = p->search())
        s->focus();
    return p;
}

// ── Prompt history search ───────────────────────────────────────────────────
// Ctrl+R's search of earlier prompts.

namespace {

constexpr float  kHistMargins = 4, kHistListMaxH = 320, kHistPadX = 10, kHistPadY = 6;
constexpr int    kHistPageRows = 5;
// A match further into a long prompt than this would be out of sight in two
// lines: the row starts shortly before it instead.
constexpr size_t kLateMatch = 100, kLeadIn = 30;
constexpr double kShadeFadeMs = 350; // as Ctrl+F's shade
constexpr int    kShadeAlpha  = 70;  // surface.overlay

bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::vector<std::string> queryWords(std::string_view q) {
    std::vector<std::string> out;
    size_t                   i = 0;
    while (i < q.size()) {
        while (i < q.size() && isWs(q[i]))
            ++i;
        size_t j = i;
        while (j < q.size() && !isWs(q[j]))
            ++j;
        if (j > i)
            out.emplace_back(q.substr(i, j - i));
        i = j;
    }
    return out;
}

// The shade over the message area while the panel is open; a click on it
// closes the panel like Esc.
class HistoryShade final : public Popup {
public:
    explicit HistoryShade(std::function<void()> onClick) : _onClick(std::move(onClick)) {
        setModal(false);
        setCard(false);
        startTicking();
    }
    bool tick(double nowMs) override {
        if (_start < 0)
            _start = nowMs;
        const double t = std::min(1.0, (nowMs - _start) / kShadeFadeMs);
        _t             = float(1 - (1 - t) * (1 - t) * (1 - t)); // OutCubic
        update();
        return t < 1;
    }
    void paint(gfx::Painter &p) override {
        p.fillRect(bounds(), gfx::Color(uint32_t(std::lround(kShadeAlpha * _t)) << 24));
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerDown) {
            if (_onClick) {
                auto cb = _onClick;
                cb();
            }
            return true;
        }
        return false;
    }

private:
    std::function<void()> _onClick;
    double                _start = -1;
    float                 _t     = 0;
};

// A match row: the prompt in two lines at most, the query's words semibold
// on accent.subtleBg; surface.highlightStrong when selected,
// surface.highlight under the pointer.
class HistoryRow final : public Clickable {
public:
    HistoryRow() {
        setLook({C::None, C::None, C::None, C::None, 6});
        style().padding(kHistPadX, kHistPadY);
        label = add<Label>();
        label->setMaxLines(2);
        label->setHitTransparent(true);
    }
    void paint(gfx::Painter &p) override {
        const RectF r{0, 1, width(), std::max(0.f, height() - 2)};
        if (checked())
            p.fillRoundRect(r, 6, color(C::FormHighlightStrong));
        else if (hovered())
            p.fillRoundRect(r, 6, color(C::FormHighlight));
    }
    Label *label = nullptr;
};

// historyFilter over entries already through foldCase, with the query's
// words folded too.
std::vector<size_t>
filterFolded(const std::vector<std::string> &folded, const std::vector<std::string> &words) {
    std::vector<size_t> out;
    for (size_t i = 0; i < folded.size(); ++i) {
        bool all = true;
        for (const std::string &w : words)
            all = all && utf8::containsPrefolded(folded[i], w);
        if (all)
            out.push_back(i);
    }
    return out;
}

std::vector<std::string> foldedWords(std::string_view query) {
    std::vector<std::string> words = queryWords(query);
    for (std::string &w : words)
        w = utf8::foldCase(w);
    return words;
}

// historyMatches with the query's words as folded code points.
std::vector<std::pair<size_t, size_t>>
matchesOf(std::string_view text, const std::vector<std::vector<uint32_t>> &needles) {
    // Folded code points with their byte offsets, so a match maps back to
    // the original bytes whatever folding did to their length.
    std::vector<uint32_t> cps;
    std::vector<size_t>   at;
    for (size_t i = 0; i < text.size();) {
        at.push_back(i);
        cps.push_back(utf8::foldCase(utf8::decode(text, i)));
    }
    at.push_back(text.size());
    std::vector<std::pair<size_t, size_t>> found;
    for (const std::vector<uint32_t> &needle : needles) {
        for (size_t s = 0; s + needle.size() <= cps.size();) {
            if (std::equal(needle.begin(), needle.end(), cps.begin() + long(s))) {
                found.push_back({at[s], at[s + needle.size()] - at[s]});
                s += needle.size();
            } else {
                ++s;
            }
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<std::pair<size_t, size_t>> merged;
    for (const auto &r : found) {
        if (!merged.empty() && r.first <= merged.back().first + merged.back().second) {
            auto &last  = merged.back();
            last.second = std::max(last.first + last.second, r.first + r.second) - last.first;
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

std::vector<std::vector<uint32_t>> queryNeedles(std::string_view query) {
    std::vector<std::vector<uint32_t>> out;
    for (const std::string &w : queryWords(query))
        out.push_back(utf8::codePoints(w, true));
    return out;
}

} // namespace

std::vector<size_t> historyFilter(const std::vector<std::string> &entries, std::string_view query) {
    // The query folded once; the entries are matched as they are.
    const std::vector<std::string> words = foldedWords(query);
    std::vector<size_t>            out;
    for (size_t i = 0; i < entries.size(); ++i) {
        bool all = true;
        for (const std::string &w : words)
            all = all && utf8::containsFoldedNeedle(entries[i], w);
        if (all)
            out.push_back(i);
    }
    return out;
}

std::vector<std::pair<size_t, size_t>>
historyMatches(std::string_view text, std::string_view query) {
    return matchesOf(text, queryNeedles(query));
}

HistorySearch::HistorySearch(std::vector<std::string> entries, RectF box) : _box(box) {
    setModal(false); // the search field takes the keyboard; a click elsewhere goes through
    setCard(false);
    card(this, 6);
    // Each prompt once, where it first appears.
    {
        std::unordered_set<std::string_view> seen;
        std::vector<bool>                    keep(entries.size());
        for (size_t i = 0; i < entries.size(); ++i)
            keep[i] = seen.insert(entries[i]).second;
        for (size_t i = 0; i < entries.size(); ++i)
            if (keep[i])
                _entries.push_back(std::move(entries[i]));
    }
    // Folded once here, not per keystroke.
    _folded.reserve(_entries.size());
    for (const std::string &e : _entries)
        _folded.push_back(utf8::foldCase(e));
    style().width(box.w).padding(kHistMargins).spacing(kHistMargins);
    _scroll = add<ScrollView>();
    _empty  = add<Label>(tr("No earlier prompt matches"), Font::Control, C::FormTextMuted);
    _empty->setAlign(text::LayoutOptions::Align::Center);
    _empty->style().padding(8);
    auto *search = add<TextField>(
        tr("Search earlier prompts"), TextField::Size::Normal, uint16_t(Icon::Search)
    );
    _field           = &search->edit();
    _field->onChange = [this] {
        if (!_closed)
            refilter();
    };
    _field->onKey = [this](const Event &e) { return e.type == EventType::KeyDown && key(e); };
    // The focus going anywhere else closes the panel (it stays where it went).
    _field->onFocusChange = [this](bool on) {
        if (!on)
            dismiss();
    };
}

HistorySearch::~HistorySearch() {
    if (_shade)
        _shade->close();
}

HistorySearch *HistorySearch::open(
    Window &w, std::vector<std::string> entries, std::string_view query, RectF box, RectF area
) {
    auto *p     = new HistorySearch(std::move(entries), box);
    // The shade: the area above the composer box, down to it, not over it.
    auto *shade = new HistoryShade([p] {
        auto cb = p->onCancelled;
        p->dismiss();
        if (cb)
            cb();
    });
    shade->style().size(area.w, std::max(0.f, box.y - area.y));
    shade->setAnchor({area.x, area.y, 0, 0}, Place::Over);
    shade->onClosed = [p] { p->_shade = nullptr; };
    p->_shade       = w.showPopup(std::unique_ptr<Popup>(shade));
    w.showPopup(std::unique_ptr<Popup>(p));
    p->_winSize = w.size();
    p->_field->setText(query); // refilters when it changes the text…
    p->_field->setSelection(uint32_t(query.size()), uint32_t(query.size()));
    p->refilter(); // …and when it doesn't
    p->_field->focus();
    return p;
}

const std::string &HistorySearch::query() const {
    return _field->text();
}

std::vector<std::string> HistorySearch::matches() const {
    std::vector<std::string> out;
    for (size_t i : _matches)
        out.push_back(_entries[i]);
    return out;
}

std::string HistorySearch::selectedEntry() const {
    return _sel >= 0 && size_t(_sel) < _matches.size() ? _entries[_matches[size_t(_sel)]]
                                                       : std::string();
}

void HistorySearch::refilter() {
    _matches = filterFolded(_folded, foldedWords(query()));
    _sel     = 0;
    View *c  = _scroll->content();
    c->clearChildren();
    _rows.assign(_matches.size(), nullptr);
    const std::vector<std::vector<uint32_t>> needles = queryNeedles(query());
    // Newest at the bottom, next to the search field.
    for (size_t r = _matches.size(); r-- > 0;) {
        std::string text = str::simplified(_entries[_matches[r]]);
        auto        m    = matchesOf(text, needles);
        if (!m.empty() && m.front().first > kLateMatch) {
            // Not inside a code point.
            const size_t from = utf8::truncateAt(text, m.front().first - kLeadIn);
            text              = str::concat({"\xE2\x80\xA6", std::string_view(text).substr(from)});
            m                 = matchesOf(text, needles);
        }
        text::AttributedText t;
        const text::Style    plain = pxFont(14, text::Weight::Regular, themed(C::FormText));
        text::Style          hit   = plain;
        hit.weight                 = text::Weight::Semibold;
        hit.background             = themed(C::AccentSubtle);
        size_t done                = 0;
        for (const auto &[start, len] : m) {
            t.append(std::string_view(text).substr(done, start - done), plain);
            t.append(std::string_view(text).substr(start, len), hit);
            done = start + len;
        }
        t.append(std::string_view(text).substr(done), plain);
        auto *row = c->add<HistoryRow>();
        row->label->setRichText(std::move(t));
        const int match = int(r);
        row->onClick    = [this, match] {
            select(match);
            pick();
        };
        _rows[r] = row;
    }
    const bool none = _matches.empty();
    _scroll->setVisible(!none);
    _empty->setVisible(none);
    if (!none) {
        const float inner = _box.w - 2 * kHistMargins - 2; // inside the border
        _scroll->style().height(std::min(c->measure(inner, kInf).h, kHistListMaxH));
    }
    // On the composer box: the panel's bottom edge on its top.
    const float h = measure(_box.w, kInf).h;
    setAnchor({_box.x, _box.y - h, 0, 0}, Place::Over);
    invalidateLayout();
    select(0);
}

void HistorySearch::select(int match) {
    if (_matches.empty())
        return;
    _sel = std::clamp(match, 0, int(_matches.size()) - 1);
    for (size_t i = 0; i < _rows.size(); ++i)
        _rows[i]->setChecked(int(i) == _sel);
    _scroll->ensureVisible(_rows[size_t(_sel)], 0);
}

void HistorySearch::pick() {
    if (_matches.empty())
        return;
    const std::string text = selectedEntry();
    auto              cb   = onPicked;
    dismiss();
    if (cb)
        cb(text);
}

void HistorySearch::dismiss() {
    if (_closed)
        return;
    _closed = true;
    if (_shade)
        _shade->close();
    close();
}

void HistorySearch::layout() {
    // The composer it sits on moves with the window: start again from there.
    if (window() && _winSize.w > 0 &&
        (window()->size().w != _winSize.w || window()->size().h != _winSize.h)) {
        dismiss();
        return;
    }
    Popup::layout();
}

bool HistorySearch::key(const Event &e) {
    // Older is up, as in the list; Ctrl+R again goes on to the next older one.
    if (shortcuts::matches(shortcuts::Id::SearchPromptHistory, e)) {
        select(_sel + 1);
        return true;
    }
    if (e.mods & ui::kModMask)
        return false;
    switch (e.key) {
    case plat::Key::Up:
        select(_sel + 1);
        return true;
    case plat::Key::Down:
        select(_sel - 1);
        return true;
    case plat::Key::PageUp:
        select(_sel + kHistPageRows);
        return true;
    case plat::Key::PageDown:
        select(_sel - kHistPageRows);
        return true;
    case plat::Key::Enter:
    case plat::Key::KpEnter:
        pick();
        return true;
    case plat::Key::Escape: {
        auto cb = onCancelled;
        dismiss();
        if (cb)
            cb();
        return true;
    }
    default:
        return false;
    }
}

} // namespace shell
