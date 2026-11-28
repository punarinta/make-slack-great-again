#include "gfx/icons_generated.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float kMargin         = 8;  // popups keep this far from the window edge
constexpr float kGap            = 4;  // between the anchor and the popup
constexpr float kShadow         = 12; // blur, also the paint outset
constexpr float kIcon           = 16;
constexpr int   kSubmenuDelayMs = 220; // hover pause before a submenu opens or closes
} // namespace

// ── Popup ───────────────────────────────────────────────────────────────────

Popup::Popup() {
    setRole(Role::Group);
    setPaintOutset(uint8_t(kShadow + 4));
}

Popup::~Popup() = default;

void Popup::setAnchor(RectF r, Place p) {
    _anchor = r;
    _place  = p;
    invalidateLayout();
}

void Popup::close() {
    if (window())
        window()->closePopup(this);
}

RectF Popup::placeIn(SizeF win) {
    if (_place == Place::Fill)
        return {0, 0, win.w, win.h};
    SizeF s       = measure(win.w - 2 * kMargin, win.h - 2 * kMargin);
    s.w           = std::min(s.w, win.w - 2 * kMargin);
    s.h           = std::min(s.h, win.h - 2 * kMargin);
    const RectF a = _anchor;
    float       x = a.x, y = a.y;
    switch (_place) {
    case Place::Below:
    case Place::Above: {
        const float below = a.y + a.h + kGap, above = a.y - kGap - s.h;
        const bool  fitsBelow = below + s.h <= win.h - kMargin, fitsAbove = above >= kMargin;
        y = (_place == Place::Below ? (fitsBelow || !fitsAbove) : (!fitsAbove && fitsBelow))
                ? below
                : above;
        break;
    }
    case Place::Right: {
        const float right = a.x + a.w + kGap, left = a.x - kGap - s.w;
        const bool  fitsRight = right + s.w <= win.w - kMargin, fitsLeft = left >= kMargin;
        x = fitsRight || !fitsLeft ? right : left;
        break;
    }
    case Place::Cursor:
        // Below-right of the anchor; the other side where that leaves the window.
        x = a.x + a.w;
        y = a.y + a.h;
        if (x + s.w > win.w - kMargin && a.x - s.w >= kMargin)
            x = a.x - s.w;
        if (y + s.h > win.h - kMargin && a.y - s.h >= kMargin)
            y = a.y - s.h;
        break;
    case Place::Tip:
        x = a.x + a.w / 2 - s.w / 2;
        y = a.y - 4 - s.h; // a 4-px gap: arrow tip to target
        if (y < kMargin)
            y = a.y + a.h + 4;
        break;
    case Place::Over:
    case Place::Fill:
        break;
    }
    x = std::clamp(x, kMargin, std::max(kMargin, win.w - kMargin - s.w));
    y = std::clamp(y, kMargin, std::max(kMargin, win.h - kMargin - s.h));
    return {std::round(x), std::round(y), s.w, s.h};
}

void Popup::paint(gfx::Painter &p) {
    if (_card) {
        const float r = metric(M::RadiusL);
        p.dropShadow(bounds(), r, kShadow, color(C::Shadow));
        p.fillRoundRect(bounds(), r, color(C::PopupBg));
        p.strokeRoundRect(bounds(), r, 1, color(C::PopupBorder));
    }
    View::paint(p);
}

bool Popup::onEvent(Event &e) {
    if (e.type == EventType::KeyDown && e.key == plat::Key::Escape) {
        close();
        return true;
    }
    // Presses on the card itself must not fall through to what is below.
    return e.type == EventType::PointerDown;
}

// ── Menu ────────────────────────────────────────────────────────────────────
// Metrics, colours and drawing: 36-px rows, 26-px section
// headers, 9-px separators, 12-px side padding, a 16-px check column when
// any row is checked, an 8-px radius card with a soft halo, a faint full-width
// hover wash, and its own palette in both themes.

namespace {
constexpr float kItemH    = 36;
constexpr float kHeaderH  = 26;
constexpr float kMenuSepH = 9;
constexpr float kPadH     = 12;
constexpr float kPadV     = 6;
constexpr float kIconGap  = 8;
constexpr float kCheckW   = 16;
constexpr float kRadius   = 8;
constexpr float kHalo     = 8; // shadow halo width
constexpr float kSlack    = 4; // so the widest label never elides
constexpr float kGapRight = 24;

// The menu's colours (+ the accent and the strong icon colour).
enum MenuColor : uint8_t { Bg, Text, Dim, Danger, Accent, Icon, Hover, Sep, NColors };
constexpr C kMenuColors[NColors] = {
    C::MenuBg,
    C::MenuText,
    C::FormIcon,
    C::MenuDanger,
    C::Accent,
    C::FormIconStrong,
    C::MenuHover,
    C::MenuSeparator,
};

Color menuColor(MenuColor c) {
    return color(kMenuColors[c]);
}

text::Style menuFont(Font f, MenuColor c) {
    text::Style s = font(f);
    s.color       = menuColor(c);
    return s;
}

float rowH(const MenuItem &it) {
    return it.separator ? kMenuSepH : it.header ? kHeaderH : kItemH;
}

bool selectable(const MenuItem &it) {
    return !it.separator && !it.header && it.enabled;
}

// "E", "Del", "Ctrl+C" → the key (and whether Ctrl/Cmd is part of it).
plat::Key hintKey(std::string_view h, bool *primary) {
    *primary = false;
    if (h.size() > 5 && (h.substr(0, 5) == "Ctrl+" || h.substr(0, 5) == "Cmd+")) {
        *primary = true;
        h        = h.substr(h[0] == 'C' && h[1] == 'm' ? 4 : 5);
    }
    if (h == "Del")
        return plat::Key::Delete;
    if (h.size() == 1 && h[0] >= 'A' && h[0] <= 'Z')
        return plat::Key(int(plat::Key::A) + (h[0] - 'A'));
    return plat::Key::Unknown;
}
} // namespace

MenuItem MenuItem::separatorItem() {
    MenuItem m;
    m.separator = true;
    return m;
}

MenuItem MenuItem::headerItem(std::string text) {
    MenuItem m;
    m.label  = std::move(text);
    m.header = true;
    return m;
}

Menu::Menu(std::vector<MenuItem> items, std::function<void(int)> onSelect)
    : _items(std::move(items)), _onSelect(std::move(onSelect)) {
    setRole(Role::Menu);
    setFocusable(true);
    setCard(false); // painted here, with its own halo
    setPaintOutset(uint8_t(kHalo + 2));
    style().padding(0, kPadV);
    // Separators collapse: no doubled, leading or trailing dividers.
    std::vector<MenuItem> kept;
    for (MenuItem &it : _items)
        if (!it.separator || (!kept.empty() && !kept.back().separator))
            kept.push_back(std::move(it));
    while (!kept.empty() && kept.back().separator)
        kept.pop_back();
    _items = std::move(kept);
}

Menu::~Menu() {
    if (_hoverTimer)
        app()->cancelTimer(_hoverTimer);
}

Menu *Menu::show(
    Window &w, RectF anchor, std::vector<MenuItem> items, std::function<void(int)> onSelect, Place p
) {
    auto m = std::make_unique<Menu>(std::move(items), std::move(onSelect));
    m->setAnchor(anchor, p);
    return static_cast<Menu *>(w.showPopup(std::move(m)));
}

Menu *Menu::popupAt(
    Window &w, PointF at, std::vector<MenuItem> items, std::function<void(int)> onSelect
) {
    // The menu carries its halo as a transparent margin, so the card
    // sits that far from the click; flipped, it ends that far before it.
    return show(
        w,
        {at.x - kHalo, at.y - kHalo, 2 * kHalo, 2 * kHalo},
        std::move(items),
        std::move(onSelect),
        Place::Cursor
    );
}

void Menu::setMinWidth(float w) {
    _minW = w;
    invalidateLayout();
}

void Menu::willClose() {
    if (_hoverTimer) {
        app()->cancelTimer(_hoverTimer);
        _hoverTimer = 0;
    }
    if (Menu *c = _child) // the submenu first: focus unwinds child → parent → origin
        c->close();
    if (_parent) {
        _parent->_child    = nullptr;
        _parent->_childFor = -1;
        _parent            = nullptr;
    }
}

void Menu::styleChanged() {
    _labels.clear();
    _hints.clear();
    update();
}

bool Menu::anyChecked() const {
    for (const MenuItem &it : _items)
        if (it.checked)
            return true;
    return false;
}

SizeF Menu::measureContent(float, float) {
    const float scale = windowScale();
    if (_labels.size() != _items.size()) {
        _labels.clear();
        _hints.clear();
        for (const MenuItem &it : _items) {
            text::AttributedText t, h;
            if (it.header) {
                t.append(it.label, menuFont(Font::Small, Dim));
            } else if (!it.separator) {
                const MenuColor c  = !it.enabled  ? Dim
                                     : it.danger  ? Danger
                                     : it.checked ? Accent
                                                  : Text;
                text::Style     st = menuFont(Font::Body, c);
                if (it.bold)
                    st.weight = text::Weight::Bold;
                t.append(it.label, st);
                const std::string_view right = !it.hint.empty()  ? std::string_view(it.hint)
                                               : !it.sub.empty() ? std::string_view("\xE2\x80\xBA")
                                                                 : std::string_view();
                h.append(right, menuFont(Font::Small, Dim));
            }
            _labels.push_back(text::Layout::build(std::move(t), {}, scale));
            _hints.push_back(
                h.text.empty() ? nullptr : text::Layout::build(std::move(h), {}, scale)
            );
        }
    }
    const float check = anyChecked() ? kCheckW : 0;
    float       w = _minW, h = 0;
    for (size_t i = 0; i < _items.size(); ++i) {
        const MenuItem &it = _items[i];
        h += rowH(it);
        if (it.separator || it.header)
            continue;
        float rw = kPadH + check + (it.icon != Button::kNoIcon ? kIcon + kIconGap : 0) +
                   std::ceil(_labels[i]->width()) + kSlack + kPadH;
        if (_hints[i])
            rw += kGapRight + std::ceil(_hints[i]->width()) + kPadH;
        w = std::max(w, rw);
    }
    return {std::ceil(w), h};
}

float Menu::itemTop(int index) const {
    float y = currentStyle().pad.t;
    for (int i = 0; i < index && i < int(_items.size()); ++i)
        y += rowH(_items[size_t(i)]);
    return y;
}

RectF Menu::itemRect(std::string_view label) const {
    for (size_t i = 0; i < _items.size(); ++i)
        if (!_items[i].separator && _items[i].label.rfind(label, 0) == 0)
            return {0, itemTop(int(i)), width(), rowH(_items[i])};
    return {};
}

int Menu::itemAt(float y) const {
    float top = currentStyle().pad.t;
    for (size_t i = 0; i < _items.size(); ++i) {
        const float h = rowH(_items[i]);
        if (y >= top && y < top + h)
            return _items[i].separator || _items[i].header ? -1 : int(i);
        top += h;
    }
    return -1;
}

void Menu::setCurrent(int index) {
    if (index == _current)
        return;
    if (_current >= 0)
        update({0, itemTop(_current), width(), kItemH});
    _current = index;
    if (_current >= 0)
        update({0, itemTop(_current), width(), kItemH});
}

void Menu::step(int dir) {
    const int n = int(_items.size());
    int       i = _current;
    for (int k = 0; k < n; ++k) {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : ((i + dir) % n + n) % n;
        if (selectable(_items[size_t(i)])) {
            setCurrent(i);
            return;
        }
    }
}

void Menu::openSubmenu(int index, bool selectFirst) {
    if (index < 0 || index >= int(_items.size()) || !window())
        return;
    const MenuItem &it = _items[size_t(index)];
    if (it.sub.empty() || !it.enabled)
        return;
    if (_child && _childFor == index) {
        if (selectFirst) {
            window()->setFocus(_child);
            if (_child->_current < 0)
                _child->step(1);
        }
        return;
    }
    if (_child)
        _child->close();
    setCurrent(index);
    // Beside the row, with its first item level with the row.
    const RectF r = windowRect();
    const float y = r.y + itemTop(index) - currentStyle().pad.t;
    auto        m = std::make_unique<Menu>(it.sub, nullptr);
    m->setAnchor({r.x + 2, y, r.w - 4, kItemH}, Place::Right);
    m->_parent = this;
    Menu *raw  = m.get();
    _child     = raw;
    _childFor  = index;
    window()->showPopup(std::move(m));
    if (selectFirst)
        raw->step(1);
    else
        window()->setFocus(this); // opened by hovering: the keys stay here
}

void Menu::syncSubmenuLater() {
    if (_hoverTimer)
        app()->cancelTimer(_hoverTimer);
    _hoverTimerFor = _current;
    _hoverTimer    = app()->addTimer(kSubmenuDelayMs, false, [this] {
        _hoverTimer = 0;
        if (_current == _childFor)
            return;
        if (_current >= 0 && !_items[size_t(_current)].sub.empty())
            openSubmenu(_current, false);
        else if (_child && !_child->hovered())
            _child->close();
        if (window())
            window()->requestFrame();
    });
}

void Menu::choose(int index) {
    if (index < 0 || index >= int(_items.size()))
        return;
    const MenuItem &it = _items[size_t(index)];
    if (!selectable(it))
        return;
    if (!it.sub.empty()) {
        openSubmenu(index, true);
        return;
    }
    const int id   = it.id;
    Menu     *root = this;
    while (root->_parent)
        root = root->_parent;
    auto cb = std::move(root->_onSelect);
    root->close(); // closes the submenus too; destruction is deferred, so this is safe
    if (cb)
        cb(id);
}

void Menu::paint(gfx::Painter &p) {
    if (_labels.size() != _items.size())
        measureContent(0, 0);
    const RectF b = bounds();
    p.dropShadow(b, kRadius, kHalo, color(C::Shadow));
    p.fillRoundRect(b, kRadius, menuColor(Bg));
    const float check = anyChecked() ? kCheckW : 0;
    float       y     = currentStyle().pad.t;
    for (size_t i = 0; i < _items.size(); ++i) {
        const MenuItem &it = _items[i];
        const float     h  = rowH(it);
        if (it.separator) {
            p.fillRect({kPadH / 2, y + std::floor(h / 2), width() - kPadH, 1}, menuColor(Sep));
            y += h;
            continue;
        }
        const text::Layout *l = _labels[i].get();
        if (it.header) { // bottom-aligned, 2 px up
            l->paint(p, snapPx({kPadH + check, y + h - 2 - l->height()}));
            y += h;
            continue;
        }
        if (int(i) == _current && it.enabled)
            p.fillRect({0, y, width(), h}, menuColor(Hover));
        if (it.checked) {
            const float cx = kPadH / 2 + check / 2 - 1, cy = y + std::floor(h / 2);
            const Color a = menuColor(Accent);
            p.drawLine({cx - 4, cy + 1}, {cx - 1, cy + 4}, 1.6f, a);
            p.drawLine({cx - 1, cy + 4}, {cx + 5, cy - 3}, 1.6f, a);
        }
        float x = kPadH + check;
        if (it.icon != Button::kNoIcon) {
            const RectF ir{x, y + std::floor((h - kIcon) / 2), kIcon, kIcon};
            gfx::drawIcon(p, gfx::Icon(it.icon), ir, menuColor(it.checked ? Accent : Icon));
            x += kIcon + kIconGap;
        }
        l->paint(p, snapPx({x, y + std::floor((h - l->height()) / 2)}));
        if (const text::Layout *r = _hints[i].get())
            r->paint(
                p, snapPx({width() - kPadH - r->width(), y + std::floor((h - r->height()) / 2)})
            );
        y += h;
    }
}

bool Menu::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerMove: {
        const int i = bounds().contains(e.pos) ? itemAt(e.pos.y) : -1;
        if (i < 0 || !_items[size_t(i)].enabled) {
            setCurrent(-1); // nothing hovered off the rows
            _hoverTimerFor = -1;
            return true;
        }
        const bool moved = i != _current;
        if (moved)
            setFlag(UserFlag0, true); // armed: a release now chooses
        setCurrent(i);
        if (i != _childFor) {
            if (moved || i != _hoverTimerFor) // once per row, not on every move
                syncSubmenuLater();
        } else if (_hoverTimer) { // back on the open submenu's row: keep it
            app()->cancelTimer(_hoverTimer);
            _hoverTimer    = 0;
            _hoverTimerFor = -1;
        }
        return true;
    }
    case EventType::PointerLeave:
        if (!_child)
            setCurrent(-1);
        return false;
    case EventType::PointerDown:
        setFlag(UserFlag0, true);
        return true; // choose on release (press-drag-release works)
    case EventType::PointerUp:
        // Not armed = the release of the click that opened the menu (a
        // context menu opens under the pointer): ignore it.
        if (flag(UserFlag0) && bounds().contains(e.pos))
            choose(itemAt(e.pos.y));
        return true;
    case EventType::KeyDown: {
        // An item's shortcut hint ("E", "Del", "Ctrl+C") chooses it.
        const bool primary = (e.mods & (plat::ModCtrl | plat::ModSuper)) != 0;
        for (size_t i = 0; i < _items.size(); ++i) {
            bool            needPrimary = false;
            const MenuItem &it          = _items[i];
            if (!selectable(it) || it.hint.empty())
                continue;
            if (hintKey(it.hint, &needPrimary) == e.key && needPrimary == primary) {
                choose(int(i));
                return true;
            }
        }
        switch (e.key) {
        case plat::Key::Up:
            step(-1);
            return true;
        case plat::Key::Down:
            step(1);
            return true;
        case plat::Key::Home:
            _current = -1;
            step(1);
            return true;
        case plat::Key::End:
            _current = -1;
            step(-1);
            return true;
        case plat::Key::Right:
            openSubmenu(_current, true);
            return true;
        case plat::Key::Left:
            if (_parent)
                close(); // back to the parent menu
            return true;
        case plat::Key::Enter:
        case plat::Key::KpEnter:
        case plat::Key::Space:
            choose(_current);
            return true;
        default:
            break;
        }
        // A letter jumps to the next enabled item starting with it.
        if (const char c = typeAheadChar(e.key, false);
            c && !(e.mods & kModMask & ~plat::ModShift)) {
            const int i = typeAheadMatch(
                _current, int(_items.size()), c, &_items, [](const void *items, int k) {
                    const MenuItem &it =
                        (*static_cast<const std::vector<MenuItem> *>(items))[size_t(k)];
                    return selectable(it) ? std::string_view(it.label) : std::string_view();
                }
            );
            if (i >= 0)
                setCurrent(i);
            return true;
        }
        return Popup::onEvent(e);
    }
    default:
        return Popup::onEvent(e);
    }
}

} // namespace ui
