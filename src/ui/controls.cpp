#include "ui/controls.h"

#include <cstdlib>

#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

constexpr float kBox         = 18; // checkbox / radio indicator (16 + the 1-px border)
constexpr float kBoxGap      = 6;  // indicator → label
constexpr float kChevron     = 16;
constexpr float kFieldPad    = 12; // text inset of dropdowns and text fields
constexpr float kSpinArrowW  = 16;
constexpr float kRowH        = 39; // SectionList rows (9 + text + 9) …
constexpr float kRowPitch    = 41; // … 1 px margin above and below
constexpr float kRowInsetX   = 6;
constexpr float kScrimMargin = 24;
// The backdrop: the same dim in both themes, rgba(0,0,0,150).
constexpr Color kScrim       = 0x96000000;

using text::layoutPlain;

std::unique_ptr<text::Layout>
layoutText(std::string_view s, Font f, C c, float scale, float maxWidth = kInf) {
    return layoutPlain(s, font(f, c), scale, maxWidth);
}

// Choice's and SectionList's row labels, built once per set of strings.
void ensureLabels(
    std::vector<std::unique_ptr<text::Layout>> &labels,
    const std::vector<std::string>             &texts,
    const View                                 &v
) {
    if (labels.size() == texts.size())
        return;
    labels.clear();
    for (const std::string &t : texts)
        labels.push_back(layoutText(t, Font::Body, C::FormText, v.windowScale()));
}

// The frame of dropdowns and one-line text fields: 1 px, 2 px when active.
void inputFrame(gfx::Painter &p, RectF r, bool active, bool enabled) {
    fieldFrame(
        p,
        r,
        metric(M::RadiusM),
        enabled ? C::FormBg : C::FormSunken,
        active ? C::FieldBorderFocus : C::FieldBorder,
        active ? 2.f : 1.f
    );
}

// A one-pixel stroke inside `r` (strokeRoundRect strokes inside the outline).
void innerStroke(gfx::Painter &p, RectF r, float radius, Color c) {
    p.strokeRoundRect(r, radius, 1, c);
}

} // namespace

// ── CheckBox / Radio ────────────────────────────────────────────────────────

CheckBox::CheckBox(std::string label, bool on) : _label(std::move(label)) {
    setRole(Role::CheckBox);
    setLook({C::None, C::None, C::None, C::None, 0});
    setFlag(UserFlag0, on);
}

CheckBox::~CheckBox() = default;

void CheckBox::setLabel(std::string label) {
    _label  = std::move(label);
    _builtW = -1;
    invalidateLayout();
    update();
}

void CheckBox::setLabelFont(Font f, C color) {
    _font   = f;
    _color  = color;
    _builtW = -1;
    invalidateLayout();
    update();
}

void CheckBox::activate() {
    setChecked(!checked());
    if (onChange) {
        auto cb = onChange;
        cb(checked());
    }
}

void CheckBox::styleChanged() {
    _builtW = -1;
    update();
}

const text::Layout *CheckBox::labelFor(float w) {
    const float tw = std::max(1.f, w - kBox - kBoxGap);
    // Wrapped text that fits lays out the same at any width between its own
    // and the one it was built for (wider too, as a single line).
    const bool  same =
        _l && _builtW >= 0 &&
        (tw == _builtW ||
         (_l->width() + 0.01f < tw && (tw <= _builtW || _l->lineCount() <= 1) && !_l->truncated()));
    if (!same) {
        _builtW = tw;
        _l = layoutText(_label, _font, enabled() ? _color : C::FormTextFaint, windowScale(), tw);
    }
    return _l.get();
}

SizeF CheckBox::measureContent(float aw, float) {
    if (_label.empty())
        return {kBox, kBox};
    const text::Layout *l = labelFor(aw >= kInf ? kInf : aw);
    return {kBox + kBoxGap + std::ceil(l->width()), std::max(kBox, std::ceil(l->height()))};
}

void CheckBox::paint(gfx::Painter &p) {
    const Style &s = currentStyle();
    if (_label.empty()) {
        paintIndicator(p, {snapPx(s.pad.l), snapPx(s.pad.t), kBox, kBox});
        return;
    }
    // The label's colour follows enabled() (the layout caches it).
    if (_lEnabledState != enabled())
        _builtW = -1;
    _lEnabledState           = enabled();
    const text::Layout *l    = labelFor(width() - s.pad.l - s.pad.r);
    // The indicator sits on the first line.
    const float         line = std::min(l->height(), std::ceil(font(Font::Control).size * 1.4f));
    paintIndicator(
        p,
        {snapPx(s.pad.l),
         snapPx(s.pad.t + std::max(0.f, std::floor((line - kBox) / 2))),
         kBox,
         kBox}
    );
    l->paint(
        p,
        {snapPx(s.pad.l + kBox + kBoxGap),
         snapPx(s.pad.t + std::max(0.f, std::floor((kBox - line) / 2)))}
    );
}

void CheckBox::paintIndicator(gfx::Painter &p, RectF r) {
    const bool on = checked(), en = enabled();
    if (on) {
        p.fillRoundRect(r, 4, color(en ? C::Accent : C::FormTextFaint));
        gfx::drawIcon(
            p, gfx::Icon::CheckOnAccent, {r.x + 1, r.y + 1, 16, 16}, color(C::AccentText)
        );
    } else {
        p.fillRoundRect(r, 4, color(C::FieldWell));
        innerStroke(p, r, 4, color(hovered() && en ? C::FormTextFaint : C::FormDividerStrong));
    }
    paintFocusRing(*this, p, {r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 6);
}

Radio::Radio(std::string label, bool on) : CheckBox(std::move(label), on) {
    _radio = true;
}

void Radio::activate() {
    if (checked())
        return;
    CheckBox::activate();
}

void Radio::paintIndicator(gfx::Painter &p, RectF r) {
    const bool   on = checked(), en = enabled();
    const PointF c{r.x + r.w / 2, r.y + r.h / 2};
    p.fillCircle(c, r.w / 2, color(C::FieldWell));
    const C ring = on                ? (en ? C::Accent : C::FormTextFaint)
                   : hovered() && en ? C::FormTextFaint
                                     : C::FormDividerStrong;
    p.strokeCircle(c, r.w / 2 - 0.5f, 1, color(ring));
    if (on)
        p.fillCircle(c, 4.5f, color(en ? C::Accent : C::FormTextFaint));
    paintFocusRing(*this, p, {r.x - 2, r.y - 2, r.w + 4, r.h + 4}, r.w / 2 + 2);
}

RadioGroup::RadioGroup(const std::vector<std::string> &options, int selected)
    : _selected(selected) {
    setRole(Role::Group);
    style().spacing(8);
    for (size_t i = 0; i < options.size(); ++i) {
        auto *r     = add<Radio>(options[i], int(i) == selected);
        r->onChange = [this, i](bool) { choose(int(i)); };
        r->style().alignSelf(Align::Start);
    }
}

Radio *RadioGroup::radio(int i) const {
    return i >= 0 && size_t(i) < childCount() ? static_cast<Radio *>(child(size_t(i))) : nullptr;
}

void RadioGroup::setSelected(int i) {
    _selected = i;
    for (size_t k = 0; k < childCount(); ++k)
        static_cast<Radio *>(child(k))->setChecked(int(k) == i);
}

void RadioGroup::choose(int i) {
    const bool changed = i != _selected;
    setSelected(i);
    if (changed && onChange) {
        auto cb = onChange;
        cb(i);
    }
}

// ── Choice / Dropdown ───────────────────────────────────────────────────────

Choice::Choice(std::vector<std::string> options, int selected)
    : _options(std::move(options)), _selected(selected) {
    setLook({C::None, C::None, C::None, C::None, metric(M::RadiusM)});
    style().h = kFormSmallH;
    style().noShrink();
}

Choice::~Choice() = default;

void Choice::setSelected(int i) {
    if (i < 0 || i >= int(_options.size()) || i == _selected)
        return;
    _selected = i;
    update();
}

void Choice::setOptions(std::vector<std::string> options, int selected) {
    _options  = std::move(options);
    _selected = std::clamp(selected, 0, std::max(0, int(_options.size()) - 1));
    _labels.clear();
    invalidateLayout();
    update();
}

void Choice::choose(int i) {
    if (i < 0 || i >= int(_options.size()) || i == _selected)
        return;
    setSelected(i);
    if (onChange) {
        auto cb = onChange;
        cb(i);
    }
}

void Choice::styleChanged() {
    _labels.clear();
    update();
}

const text::Layout *Choice::label(size_t i) {
    ensureLabels(_labels, _options, *this);
    return _labels[i].get();
}

std::string Choice::accessibleName() const {
    return _selected >= 0 && _selected < int(_options.size()) ? _options[size_t(_selected)]
                                                              : std::string();
}

Dropdown::Dropdown(std::vector<std::string> options, int selected)
    : Choice(std::move(options), selected) {}

SizeF Dropdown::measureContent(float, float) {
    float w = 0;
    for (size_t i = 0; i < _options.size(); ++i)
        w = std::max(w, std::ceil(label(i)->width()));
    return {kFieldPad + w + 8 + kChevron + kFieldPad, kFormSmallH};
}

void Dropdown::paint(gfx::Painter &p) {
    const bool en = enabled();
    inputFrame(p, bounds(), en && (hovered() || _menu), en);
    if (_selected >= 0 && _selected < int(_options.size())) {
        const text::Layout *l = label(size_t(_selected));
        p.save();
        p.clipRect({0, 0, width() - kFieldPad - kChevron - 4, height()});
        if (!en)
            p.setOpacity(0.5f);
        l->paint(p, snapPx({kFieldPad, std::floor((height() - l->height()) / 2)}));
        p.restore();
    }
    gfx::drawIcon(
        p,
        gfx::Icon::ChevronDown,
        {snapPx(width() - kFieldPad - kChevron),
         snapPx(std::floor((height() - kChevron) / 2)),
         kChevron,
         kChevron},
        color(en ? C::FormTextMuted : C::FormTextFaint)
    );
}

void Dropdown::paintOver(gfx::Painter &p) {
    paintFocusRing(*this, p, bounds(), metric(M::RadiusM));
}

void Dropdown::activate() {
    if (!window() || _menu)
        return;
    std::vector<MenuItem> items;
    for (size_t i = 0; i < _options.size(); ++i) {
        items.push_back({int(i), _options[i], "", Button::kNoIcon, true, int(i) == _selected});
        if (int(i) == _separatorAfter)
            items.push_back(MenuItem::separatorItem()); // a trailing one is dropped by Menu
    }
    const RectF r = windowRect();
    _menu = Menu::show(*window(), {r.x, r.y + 2, r.w, r.h}, std::move(items), [this](int id) {
        choose(id);
    });
    _menu->setCurrent(_selected + (_separatorAfter >= 0 && _selected > _separatorAfter));
    _menu->onClosed = [this] {
        _menu = nullptr;
        update();
    };
    update();
}

// ── SpinBox ─────────────────────────────────────────────────────────────────

SpinBox::SpinBox(int value, int min, int max, std::string suffix)
    : _value(std::clamp(value, min, max)), _min(min), _max(max), _suffix(std::move(suffix)) {
    setFocusable(true);
    setHoverRepaint(true);
    setRole(Role::TextInput);
    setCursor(plat::Cursor::IBeam);
    style().h = kFormSmallH;
    style().noShrink();
}

SpinBox::~SpinBox() = default;

void SpinBox::setValue(int v) {
    v = std::clamp(v, _min, _max);
    if (v == _value && !_typing)
        return;
    _value  = v;
    _typing = false;
    _num.reset();
    update();
}

void SpinBox::styleChanged() {
    _num.reset();
    _suf.reset();
    update();
}

std::string SpinBox::accessibleName() const {
    return str::number(_value) + _suffix;
}

SizeF SpinBox::measureContent(float, float) {
    return {90, kFormSmallH};
}

uint8_t SpinBox::cursorAt(PointF pt) const {
    return arrowAt(pt) ? uint8_t(plat::Cursor::Hand) : View::cursorAt(pt);
}

int SpinBox::arrowAt(PointF pt) const {
    if (pt.x < width() - kSpinArrowW - 1 || pt.x >= width())
        return 0;
    return pt.y < height() / 2 ? 1 : -1;
}

void SpinBox::step(int n) {
    const int before = _value;
    commit();
    setValue(_value + n);
    if (_value != before && onChange) {
        auto cb = onChange;
        cb(_value);
    }
}

void SpinBox::commit() {
    if (!_typing)
        return;
    const int before = _value;
    const int typed  = _typed.empty() ? _value : std::atoi(_typed.c_str());
    _typing          = false;
    _typed.clear();
    _value = std::clamp(typed, _min, _max);
    _num.reset();
    update();
    if (_value != before && onChange) {
        auto cb = onChange;
        cb(_value);
    }
}

void SpinBox::paint(gfx::Painter &p) {
    const bool  en = enabled();
    const RectF b  = bounds();
    fieldFrame(p, b, 4, C::FieldWell, focused() ? C::FormLink : C::FormDividerStrong);
    // Up/down halves.
    const float ax = width() - kSpinArrowW - 1;
    if (_hoverArrow && hovered() && en)
        p.fillRect(
            {ax,
             _hoverArrow > 0 ? 1.f : std::floor(height() / 2),
             kSpinArrowW,
             std::floor(height() / 2) - 1},
            color(C::FormHighlight)
        );
    const C tint = en ? C::FormTextMuted : C::FormTextFaint;
    gfx::drawIcon(
        p, gfx::Icon::SpinUp, {snapPx(ax + 3), snapPx(height() / 2 - 11), 10, 10}, color(tint)
    );
    gfx::drawIcon(
        p, gfx::Icon::SpinDown, {snapPx(ax + 3), snapPx(height() / 2 + 1), 10, 10}, color(tint)
    );
    // "14" (selected right after focusing, like a spin box) + " days".
    const float scale = windowScale();
    const C     tc    = en ? C::FormText : C::FormTextFaint;
    // Focus and enabled only recolour the shaped text; typing reshapes.
    if (!_num || _typing)
        _num = layoutText(_typing ? _typed : str::number(_value), Font::Control, tc, scale);
    _num->setColor(color(_fresh && focused() ? C::AccentText : tc));
    if (!_suf)
        _suf = layoutText(_suffix, Font::Control, tc, scale);
    _suf->setColor(color(tc));
    const float x = 9, y = snapPx(std::floor((height() - _num->height()) / 2));
    if (_fresh && focused())
        p.fillRect({x, y + 1, std::ceil(_num->width()), _num->height() - 2}, color(C::Accent));
    _num->paint(p, {x, y});
    _suf->paint(p, {snapPx(x + _num->width()), y});
    if (focused() && _typing)
        p.fillRect({snapPx(x + _num->width()), y + 2, 1, _num->height() - 4}, color(C::Caret));
}

bool SpinBox::onEvent(Event &e) {
    switch (e.type) {
    case EventType::FocusIn:
        _fresh = true;
        if (window()) { // typed digits arrive as TextInput only while this is on
            const PointF o = mapToWindow({0, 0});
            window()->setTextInput(true, {o.x, o.y, width(), height()});
        }
        update();
        return false;
    case EventType::FocusOut:
        _fresh = false;
        commit();
        if (window())
            window()->setTextInput(false, {});
        update();
        return false;
    case EventType::PointerMove: {
        const int a = arrowAt(e.pos);
        if (a != _hoverArrow) {
            _hoverArrow = a;
            update();
        }
        return false;
    }
    case EventType::PointerLeave:
        _hoverArrow = 0; // the hover repaint (setHoverRepaint) clears it
        return false;
    case EventType::PointerDown:
        if (e.button != plat::Button::Left)
            return false;
        focus();
        if (const int a = arrowAt(e.pos))
            step(a);
        return true;
    case EventType::PointerUp:
        return true;
    case EventType::KeyDown:
        if (const int n = stepForKey(e.key)) {
            step(n);
            return true;
        }
        switch (e.key) {
        case plat::Key::Enter:
        case plat::Key::KpEnter:
            commit();
            return true;
        case plat::Key::Backspace:
            if (!_typing) {
                _typing = true;
                _typed  = _fresh ? std::string() : str::number(_value);
            }
            if (!_typed.empty())
                _typed.pop_back();
            _fresh = false;
            update();
            return true;
        default:
            return false;
        }
    case EventType::TextInput:
        if (!e.raw)
            return false;
        for (char ch : e.raw->text)
            if (ch >= '0' && ch <= '9') {
                if (!_typing || _fresh) {
                    _typing = true;
                    _typed.clear();
                }
                _fresh = false;
                if (_typed.size() < 6)
                    _typed += ch;
            }
        update();
        return true;
    default:
        return false;
    }
}

// ── TextField ───────────────────────────────────────────────────────────────

namespace {

// Repaints the field around it when focus comes or goes (focus events do not
// bubble; the border is the parent's).
class FieldEdit final : public TextEdit {
public:
    bool onEvent(Event &e) override {
        if ((e.type == EventType::FocusIn || e.type == EventType::FocusOut) && parent())
            parent()->update();
        return TextEdit::onEvent(e);
    }
};

} // namespace

TextField::TextField(std::string placeholder, bool multiLine) : _multi(multiLine) {
    setRole(Role::TextInput);
    style().row().items(multiLine ? Align::Stretch : Align::Center).spacing(8);
    _edit = add<FieldEdit>();
    _edit->style().flex(1).padding(0);
    _edit->setPlaceholder(std::move(placeholder));
    if (multiLine) {
        // The glossary: caption-sized text, fixed height (two controls and
        // an extra-small one tall), scrolling inside.
        style().h = 2 * kFormNormalH + 22;
        style().padding(9, 8, 9, 6);
        _edit->setFont(Font::Control);
        _edit->setMinLines(4);
        _edit->setMaxLines(4);
    } else {
        style().h = kFormSmallH;
        style().padding(kFieldPad + 3, 0, kFieldPad - 4, 0);
        _edit->setFont(Font::Field);
        _edit->setMinLines(1);
        _edit->setMaxLines(1);
        _edit->onSubmit = [this] {
            if (onReturn) {
                auto cb = onReturn;
                cb();
            }
            return true; // never a newline
        };
    }
    style().noShrink();
}

TextField::TextField(std::string placeholder, Size size, uint16_t icon)
    : TextField(std::move(placeholder)) {
    if (size == Size::Small && icon == 0xffff)
        return;
    _icon     = icon;
    style().h = size == Size::Normal ? kFormNormalH : kFormSmallH;
    // StyledLineEdit: the icon 12 in, 8 before the text.
    if (icon != 0xffff)
        style().padding(kFieldPad + 16 + 8 + 3, 0, kFieldPad - 4, 0);
}

TextField::~TextField() = default;

void TextField::setPrefix(std::string text) {
    // text.tertiary at fonts.base, before the edit.
    auto *l = adopt(std::make_unique<Label>(std::move(text), Font::Field, C::FormTextFaint), 0);
    l->style().noShrink();
}

void TextField::setQuiet() {
    _quiet = true;
    // The icon 12 in, 8 before the edit; the edit's padding is TextEdit's.
    style().padding(kFieldPad + (_icon != 0xffff ? 16 + 8 : 0), 0, kFieldPad, 0);
    _edit->style().padding(8, 6);
    update();
}

void TextField::setMaxLength(int n) {
    _max = n;
    if (!_counter) {
        _counter = add<Label>();
        _counter->style().noShrink();
    }
    _edit->setMaxLength(n);
    _edit->onChange = [this] { updateCounter(); };
    updateCounter();
}

void TextField::updateCounter() {
    const int            n = int(utf8::countCodePoints(_edit->text()));
    text::AttributedText t; // fonts.sm, text.tertiary
    t.append(str::number(_max - n), pxFont(11, text::Weight::Regular, themed(C::FormTextFaint)));
    _counter->setRichText(std::move(t));
}

bool TextField::onEvent(Event &e) {
    if (e.type == EventType::PointerDown && e.button == plat::Button::Left) {
        _edit->focus(); // a press anywhere on the frame
        return true;
    }
    return View::onEvent(e);
}

bool TextField::masked() const {
    return _edit->masked();
}

void TextField::setMasked(bool on, bool reveal) {
    _edit->setMasked(on);
    if (reveal && !_eye) {
        _eye = add<IconButton>(gfx::Icon::Eye, i18n::tr("Show"));
        _eye->style().size(26, 26);
        _eye->setIconSize(16);
        _eye->setTextColor(C::FormTextMuted);
        _eye->setLook({C::None, C::FormHighlight, C::FormHighlight, C::None, 13});
        _eye->setFocusable(false);
        _eye->onClick = [this] {
            const bool hide = !_edit->masked();
            _edit->setMasked(hide);
            _eye->setIcon(hide ? gfx::Icon::Eye : gfx::Icon::EyeOff);
            _eye->setTooltip(hide ? i18n::tr("Show") : i18n::tr("Hide"));
        };
    }
}

void TextField::paint(gfx::Painter &p) {
    const bool active = _edit->focused();
    if (_multi) {
        fieldFrame(
            p,
            bounds(),
            metric(M::RadiusM),
            C::FormBg,
            active ? C::FieldBorderFocus : C::FieldBorder
        );
    } else {
        inputFrame(p, bounds(), active && !_quiet, enabled());
        if (_icon != 0xffff)
            gfx::drawIcon(
                p,
                gfx::Icon(_icon),
                {kFieldPad, (height() - 16) / 2, 16, 16},
                color(_quiet ? C::FormTextFaint : C::FormIcon)
            );
    }
}

// ── Button: the form look ───────────────────────────────────────────────────
// Button's Form style (widgets.h), kept with the other form controls.

void Button::applyForm() {
    const float r = metric(M::RadiusM);
    switch (_kind) {
    case Kind::Primary:
        _look = {C::Accent, C::AccentHover, C::AccentPressed, C::None, r};
        break;
    case Kind::Secondary:
        _look = {C::FormBg, C::FormSunken, C::FormHighlightStrong, C::None, r};
        break;
    case Kind::Danger:
        _look = {C::DangerFill, C::DangerFillHover, C::DangerFillHover, C::None, r};
        break;
    default: // Ghost
        _look = {C::FormHighlight, C::FormHighlightStrong, C::FormHighlightStrong, C::None, r};
        break;
    }
    const bool small  = _form == Form::Small;
    const bool filled = _kind == Kind::Primary || _kind == Kind::Danger;
    _text             = filled ? C::AccentText : C::FormText;
    style().padding(small ? 12 : 18, 0).noShrink();
    style().h = small ? kFormSmallH : kFormNormalH;
}

void Button::paintForm(gfx::Painter &p) {
    if (!enabled() && (_kind == Kind::Primary || _kind == Kind::Danger))
        p.fillRoundRect(bounds(), _look.radius, color(C::FormHighlightStrong));
    else
        Clickable::paint(p);
    if (_kind == Kind::Secondary)
        innerStroke(p, bounds(), _look.radius, color(C::FormDividerStrong));
    const text::Layout *l = labelLayout(); // recoloured for the enabled state
    if (!l)
        return;
    // Centre the capitals, not the line box: the font's ascent leaves more
    // room above the caps than its descent does below the baseline, so a
    // box-centred label sits visibly low.
    const float cap = text::metrics(font(labelFont()), windowScale()).capHeight;
    p.save();
    p.clipRect(bounds());
    l->paint(
        p, snapPx({std::floor((width() - l->width()) / 2), (height() + cap) / 2 - l->baseline(0)})
    );
    p.restore();
}

// ── SectionList ─────────────────────────────────────────────────────────────

SectionList::SectionList(std::vector<std::string> items) : _items(std::move(items)) {
    setRole(Role::List);
    setFocusable(true); // a click focuses it, so Up/Down go on from there
    setHoverRepaint(true);
}

SectionList::~SectionList() = default;

void SectionList::setSelected(int i) {
    if (i < 0 || i >= int(_items.size()) || i == _selected)
        return;
    _selected = i;
    update();
}

void SectionList::choose(int i) {
    if (i < 0 || i >= int(_items.size()) || i == _selected)
        return;
    setSelected(i);
    if (onChange) {
        auto cb = onChange;
        cb(i);
    }
}

void SectionList::styleChanged() {
    _labels.clear();
    update();
}

uint8_t SectionList::cursorAt(PointF pt) const {
    return rowAt(pt.y) >= 0 ? uint8_t(plat::Cursor::Hand) : View::cursorAt(pt);
}

int SectionList::rowAt(float y) const {
    const float r = y - currentStyle().pad.t;
    const int   i = int(std::floor(r / kRowPitch));
    return r >= 0 && i < int(_items.size()) ? i : -1;
}

SizeF SectionList::measureContent(float, float) {
    return {120, kRowPitch * float(_items.size())};
}

void SectionList::paint(gfx::Painter &p) {
    // Sunken, rounded only where it meets the dialog's bottom-left corner.
    const float rad = metric(M::RadiusL) - 1;
    const Color bg  = color(C::FormSunken);
    p.fillRoundRect(bounds(), rad, bg);
    p.fillRect({0, 0, width(), height() - rad}, bg);
    p.fillRect({rad, 0, width() - rad, height()}, bg);
    p.fillRect({width() - 1, 0, 1, height()}, color(C::FormDivider));

    const Style &s = currentStyle();
    ensureLabels(_labels, _items, *this);
    const float w = width() - 1 - 2 * kRowInsetX;
    for (size_t i = 0; i < _items.size(); ++i) {
        const RectF row{kRowInsetX, s.pad.t + kRowPitch * float(i) + 1, w, kRowH};
        if (int(i) == _selected)
            p.fillRoundRect(row, 4, color(C::FormHighlightStrong));
        else if (int(i) == _hover && hovered())
            p.fillRoundRect(row, 4, color(C::FormHighlight));
        const text::Layout *l = _labels[i].get();
        l->paint(p, snapPx({row.x + 18, row.y + std::floor((kRowH - l->height()) / 2)}));
        if (int(i) == _selected)
            paintFocusRing(*this, p, row, 4);
    }
}

bool SectionList::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerMove: {
        const int h = rowAt(e.pos.y);
        if (h != _hover) {
            // Only the two rows whose hover fill changes.
            const float top = currentStyle().pad.t;
            for (int r : {_hover, h})
                if (r >= 0)
                    update({0, top + kRowPitch * float(r), width(), kRowPitch});
            _hover = h;
        }
        return false;
    }
    case EventType::PointerDown:
        if (e.button != plat::Button::Left)
            return false;
        choose(rowAt(e.pos.y));
        return true;
    case EventType::PointerUp:
        return true;
    case EventType::KeyDown:
        switch (e.key) {
        case plat::Key::Up:
            choose(_selected - 1);
            return true;
        case plat::Key::Down:
            choose(_selected + 1);
            return true;
        case plat::Key::Home:
            choose(0);
            return true;
        case plat::Key::End:
            choose(int(_items.size()) - 1);
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

// ── Dialog ──────────────────────────────────────────────────────────────────

namespace {

constexpr float kCardRadius = 12, kCardPadH = 28, kCardPadV = 24, kHeaderGap = 16;
constexpr Color kCardScrim = 0x8c000000; // rgba(0,0,0,140), both themes

// A dialog's card: surface.raised, radius 12, the children clipped to its
// rounded corners (content may run to the edges); presses on it stay inside.
class Card final : public View {
public:
    Card() {
        setBackground(C::FormBg, kCardRadius);
        setClipChildren(true);
    }
    bool onEvent(Event &e) override { return e.type == EventType::PointerDown; }
};

// IconButton(":/ui/x.svg", 32, 14): flat, a round hover wash.
class CloseButton final : public Clickable {
public:
    CloseButton() {
        style().size(32, 32).noShrink();
        setFocusable(false);
    }
    void paint(gfx::Painter &p) override {
        if (hovered())
            p.fillCircle({16, 16}, 16, color(C::FormHighlight));
        gfx::drawIcon(p, gfx::Icon::X, {9, 9, 14, 14}, color(C::FormIcon));
    }
};

} // namespace

Label *styledLabel(View *parent, std::string text, const text::Style &st, int maxLines) {
    auto                *l = parent->add<Label>();
    text::AttributedText t;
    t.append(text, st);
    l->setRichText(std::move(t));
    if (maxLines)
        l->setMaxLines(maxLines);
    return l;
}

Dialog::Dialog(std::string title, float cardWidth, Scroll scroll) : _w(cardWidth), _h(0) {
    setCard(false);
    setAnchor({}, Place::Fill);
    setPaintOutset(0);
    style().dir = Dir::None;
    // Not a layout boundary: the card is as tall as its content, so content
    // that appears or wraps anew must resize it. No padding of its own: the
    // header and the body carry it, so a bare card's content (and a scroll
    // bar) can reach its edges.
    _panel      = add<Card>();
    _panel->style().spacing(0);
    const bool titled = !title.empty();
    if (titled) {
        // The cross lines up with the body's right edge: its button's hover
        // wash reaches into the padding.
        auto *head = _panel->add<View>();
        head->style()
            .row()
            .spacing(12)
            .items(Align::Center)
            .padding(kCardPadH, kCardPadV, kCardPadH - kCloseInset, kHeaderGap)
            .noShrink();
        const text::Style ts = pxFont(15 * 1.45f, text::Weight::Bold, color(C::FormText));
        styledLabel(head, std::move(title), ts, 2)->style().flex(1);
        auto *close    = head->add<CloseButton>();
        close->onClick = [this] { reject(); };
        close->style().alignSelf(Align::Start);
    }
    if (scroll == Scroll::Enabled) {
        // The header stays; the rest scrolls once the card is clamped to the
        // window. Not a boundary either: the card follows its content.
        _scroll = _panel->add<ScrollView>();
        _scroll->setLayoutBoundary(false);
        _content = _scroll->content();
    } else {
        _content = _panel->add<View>();
    }
    _content->style().spacing(12);
    if (titled)
        _content->style().padding(kCardPadH, 0, kCardPadH, kCardPadV);
}

Button *Dialog::makeButton(std::string label, Button::Kind k) {
    return new Button(std::move(label), k, Button::Form::Normal); // adopted by addButtonRow
}

std::unique_ptr<Dialog> Dialog::confirm(
    std::string                               title,
    std::string                               text,
    std::string                               confirmLabel,
    Button::Kind                              kind,
    Color                                     textColor,
    const std::function<void(View *content)> &extra
) {
    auto d = std::make_unique<Dialog>(std::move(title));
    styledLabel(d->content(), std::move(text), pxFont(15, text::Weight::Regular, textColor));
    if (extra)
        extra(d->content());
    auto *ok = makeButton(std::move(confirmLabel), kind);
    d->addButtonRow(ok, makeButton(i18n::tr("Cancel"), Button::Kind::Secondary));
    Dialog *raw = d.get();
    ok->onClick = [raw] { raw->accept(); };
    return d;
}

View *Dialog::addButtonRow(Button *primary, Button *secondary, View *leading) {
    auto *row = _content->add<View>();
    row->style().row().spacing(8).items(Align::Center);
    if (leading)
        row->adopt(std::unique_ptr<View>(leading));
    row->add<View>()->style().flex(1);
    if (secondary) {
        row->adopt(std::unique_ptr<View>(secondary));
        secondary->onClick = [this] { reject(); };
    }
    if (primary)
        row->adopt(std::unique_ptr<View>(primary));
    return row;
}

void Dialog::accept() {
    if (_done)
        return;
    _done   = true;
    auto cb = std::move(onAccepted);
    close(); // deferred destruction: safe to run the callback after
    if (cb)
        cb();
}

void Dialog::reject() {
    if (_done)
        return;
    _done   = true;
    auto cb = std::move(onRejected);
    close();
    if (cb)
        cb();
}

SizeF Dialog::measureContent(float aw, float ah) {
    return _h > 0 ? Popup::measureContent(aw, ah) : SizeF{0, 0};
}

Dialog::Dialog(float w, float h) : _w(w), _h(h) {
    setCard(false);
    setAnchor({}, Place::Fill);
    setPaintOutset(0);
    style().dir = Dir::None;
    _panel      = add<View>();
    _panel->setBackground(C::FormBg, metric(M::RadiusL));
    _panel->setBorder(C::FormDividerStrong);
    _panel->setLayoutBoundary(true);
    _panel->style().padding(1); // children sit inside the 1-px border
}

Dialog::~Dialog() = default;

void Dialog::layout() {
    if (_h <= 0) { // the titled card: as tall as its content
        const float W = width(), H = height(), avail = std::max(0.f, W - 80);
        const float w =
            _w > 0 ? std::min(_w, avail) : std::clamp(avail, std::min(480.f, avail), 560.f);
        const float need = _panel->measure(w, kInf).h;
        const float h    = std::min(need, std::max(std::min(200.f, H), H - 80));
        const RectF f{
            std::round((W - w) / 2), std::round((H - h) / 2), std::round(w), std::round(h)
        };
        if (!sameRect(f, _panel->frame()))
            update(); // the backdrop and the shadow cover the whole window
        _panel->setFrame(f);
        return;
    }
    const float w = std::min(_w, std::max(0.f, width() - 2 * kScrimMargin));
    const float h = std::min(_h, std::max(0.f, height() - 2 * kScrimMargin));
    _panel->setFrame(
        {std::round((width() - w) / 2),
         std::round((height() - h) / 2),
         std::round(w),
         std::round(h)}
    );
}

void Dialog::paint(gfx::Painter &p) {
    if (_h <= 0) {
        p.fillRect(bounds(), kCardScrim);
        p.dropShadow(_panel->frame(), kCardRadius, 40, {0, 6}, 0x46000000U);
        return;
    }
    p.fillRect(bounds(), kScrim);
    p.dropShadow(_panel->frame(), metric(M::RadiusL), 16, color(C::Shadow));
}

bool Dialog::onEvent(Event &e) {
    // A press on the backdrop (not on the card) dismisses, like Escape.
    if (e.type == EventType::PointerDown && !_panel->frame().contains(e.pos)) {
        reject();
        return true;
    }
    if (e.type == EventType::KeyDown && e.key == plat::Key::Escape) {
        reject();
        return true;
    }
    return Popup::onEvent(e);
}

} // namespace ui
