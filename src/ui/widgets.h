// Widgets v1: exactly what msga's screens need. Each is a View subclass that
// paints itself (no child views inside a Button or a Menu), keeps token ids
// instead of colours, and allocates nothing for features it does not use.
#pragma once

#include "text/text.h"
#include "ui/view.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

// The keyboard focus ring (2 px, C::FocusRing) around r, drawn only while v
// has focus and its window shows focus (keyboard navigation).
void paintFocusRing(const View &v, gfx::Painter &p, RectF r, float radius);

// A form field's box: filled, with a `borderW`-px stroke inside its edge.
// Each control passes its own tokens (text fields, dropdowns, spin boxes and
// date fields differ).
void fieldFrame(gfx::Painter &p, RectF r, float radius, C fill, C border, float borderW = 1);

// What a step key does to a number field: Up/Down ±1, PageUp/PageDown ±10,
// anything else 0.
int stepForKey(plat::Key k);

// Type-to-select in lists and menus: the lower-case letter (or, with
// digits, the digit) key k types, 0 for any other key …
char typeAheadChar(plat::Key k, bool digits);
// … and the first of n items after `current` (wrapping; -1 starts at the
// top) whose label(ctx, i) starts with it, ignoring ASCII case; -1 when
// none. An empty label never matches (items that cannot be chosen).
using TypeAheadLabel = std::string_view (*)(const void *ctx, int index);
int typeAheadMatch(int current, int n, char c, const void *ctx, TypeAheadLabel label);

// ── Label ───────────────────────────────────────────────────────────────────
// Plain or rich text. Wraps at the width it is given; with maxLines > 0 the
// last line ends in "…" when cut. Rich text may use themed() sentinel colours
// and linkId spans (onLink fires on click, the cursor turns into a hand).
class Label : public View {
public:
    explicit Label(std::string text = {}, Font f = Font::Body, C color = C::Text);
    ~Label() override;

    void               setText(std::string text);
    const std::string &text() const { return _rich ? _rich->text : _text; }
    // Replaces the plain text (held once, in the rich text); sentinel colours
    // in the spans are resolved per build.
    void               setRichText(text::AttributedText t);
    void               setFont(Font f);
    void               setColor(C c);
    void               setMaxLines(int n); // 0 = unlimited; >0 also ellipsizes
    void               setAlign(text::LayoutOptions::Align a);
    void               setLineHeight(float multiple);

    // The rich text as set (null for plain labels).
    const text::AttributedText *richText() const { return _rich.get(); }
    // Spans of this link paint underlined (a hovered link); 0 = none. Only
    // the layout is rebuilt: the text and its size stay.
    void                        setUnderlinedLink(uint32_t linkId);

    std::function<void(uint32_t linkId)> onLink;

    // The layout for the current width (built on demand; null before layout).
    const text::Layout *textLayout() const { return _layout.get(); }
    // Where the layout is painted (padding, vertical centring), local coords.
    PointF              textOrigin() const;
    // The linkId of the span under `local` (0 = none): context menus on links.
    uint32_t            linkAt(PointF local) const;
    // A highlighted range of the text (byte offsets; from == to: none),
    // painted in the system highlight with the text white on it — a
    // selection the owner drives (the message list selects across many
    // labels).
    void                setSelection(uint32_t from, uint32_t to);
    uint32_t            selectionFrom() const;
    uint32_t            selectionTo() const;
    // The text offset nearest to `local` (selection drags; 0 before layout).
    uint32_t            offsetAt(PointF local) const;

    SizeF       measureContent(float availW, float availH) override;
    void        paint(gfx::Painter &p) override;
    bool        onEvent(Event &e) override;
    uint8_t     cursorAt(PointF local) const override;
    void        styleChanged() override;
    std::string accessibleName() const override { return text(); }

private:
    const text::Layout           *layoutFor(float width);
    std::unique_ptr<text::Layout> buildLayout(float width, float scale) const;
    void                          dropLayout();
    void                          dropAlt();
    void                          updateInk();

    std::string                           _text; // plain labels; empty for rich ones
    std::unique_ptr<text::AttributedText> _rich; // only for rich labels
    // Borrows the text above (Layout::buildBorrowed): dropped before the
    // text changes, and declared after it so it is destroyed first.
    std::unique_ptr<text::Layout>         _layout;
    float                                 _layoutW = -1, _layoutScale = 0, _lineHeight = 1.4f;
    uint32_t                              _pressedLink = 0;
    struct Extra;
    std::unique_ptr<Extra>     _x; // a selection, a second layout; null while neither
    Font                       _font;
    C                          _color;
    uint8_t                    _maxLines       = 0;
    text::LayoutOptions::Align _align          = text::LayoutOptions::Align::Left;
    uint32_t                   _underlinedLink = 0; // in the tail padding
};

// ── Clickable ───────────────────────────────────────────────────────────────
// Anything pressable: hover/pressed/checked backgrounds from tokens, onClick
// on release inside, Enter/Space when focused, a tooltip, the hand cursor.
// Sidebar rows and list items are Clickables with Label/Badge/Image children.
class Clickable : public View {
public:
    struct Look {
        C     bg = C::None, hover = C::Hover, pressed = C::Pressed, checked = C::None;
        float radius = 0;
    };
    Clickable();

    std::function<void()> onClick;

    void        setLook(const Look &l);
    void        setChecked(bool on);
    bool        checked() const { return flag(UserFlag0); }
    void        setTooltip(std::string t) { _tooltip = std::move(t); }
    std::string tooltip() const override { return _tooltip; }

    void         paint(gfx::Painter &p) override;
    bool         onEvent(Event &e) override;
    // Fires onClick; override to act before/instead.
    virtual void activate();

protected:
    // The background token for the current state (C::None = transparent).
    C            stateColor() const;
    virtual void stateChanged() {} // checked changed

    Look        _look;
    std::string _tooltip;
};

// ── Button ──────────────────────────────────────────────────────────────────
// A label and/or an icon. Kinds pick token defaults:
//   Primary  accent fill        Secondary  bordered        Ghost  wash on hover only
//   Danger   red fill
//   Tab      underline when checked (header tabs)
//   Icon     square ghost button with an icon (toolbar, composer, header)
// Form styles it as a dialog / settings button (the form look, controls.h):
// kFormSmallH or kFormNormalH tall in the Control or Field font (bold when
// filled), the label centred on its capitals, a faint label when disabled
// and a grey fill for a disabled Primary or Danger. Its kinds: Primary,
// Secondary (field fill, inner stroke), Danger and Ghost (a highlight wash).
class Button : public Clickable {
public:
    enum class Kind : uint8_t { Primary, Secondary, Danger, Ghost, Tab, Icon };
    enum class Form : uint8_t { None, Small, Normal };
    static constexpr uint16_t kNoIcon = 0xffff;

    explicit Button(std::string label, Kind k = Kind::Secondary, Form f = Form::None);
    Button(gfx::Icon icon, std::string tooltip, Kind k = Kind::Icon);
    ~Button() override;

    void               setLabel(std::string s);
    const std::string &label() const { return _label; }
    void               setIcon(gfx::Icon icon);
    void               setIconSize(float px);
    void               setTextColor(C c); // default depends on kind
    Kind               kind() const { return _kind; }

    SizeF       measureContent(float availW, float availH) override;
    void        paint(gfx::Painter &p) override;
    void        paintOver(gfx::Painter &p) override; // focus ring, tab underline
    void        styleChanged() override;
    std::string accessibleName() const override { return _label.empty() ? _tooltip : _label; }

protected:
    void stateChanged() override;

private:
    void                          applyKind();
    void                          applyForm();                // controls.cpp
    void                          paintForm(gfx::Painter &p); // controls.cpp
    Font                          labelFont() const;
    const text::Layout           *labelLayout();
    std::string                   _label;
    std::unique_ptr<text::Layout> _layout;
    float                         _iconSize = 18;
    uint16_t                      _icon     = kNoIcon;
    Kind                          _kind;
    Form                          _form       = Form::None;
    C                             _text       = C::Text;
    bool                          _layoutBold = false; // a tab's layout: built checked
};

// Square icon-only button: IconButton(gfx::Icon::Bold, "Bold").
class IconButton : public Button {
public:
    IconButton(gfx::Icon icon, std::string tooltip) : Button(icon, std::move(tooltip)) {}
};

// ── Badge ───────────────────────────────────────────────────────────────────
// Unread count pill (count > 0), or a small dot (setDot). Hidden when 0.
class Badge : public View {
public:
    explicit Badge(int count = 0, C bg = C::Badge, C fg = C::BadgeText);
    ~Badge() override;
    void setCount(int n);
    int  count() const { return _count; }
    void setDot(bool on);

    SizeF       measureContent(float availW, float availH) override;
    void        paint(gfx::Painter &p) override;
    void        styleChanged() override;
    std::string accessibleName() const override;

private:
    std::unique_ptr<text::Layout> _layout;
    int                           _count;
    C                             _bg, _fg;
    bool                          _dot = false;
};

// ── IconView ────────────────────────────────────────────────────────────────
// A static, tinted icon (section headers, channel glyphs). Not clickable:
// use IconButton for that.
class IconView : public View {
public:
    explicit IconView(gfx::Icon icon, float size = 16, C tint = C::TextMuted);
    void setIcon(gfx::Icon icon);
    void setTint(C c);
    void paint(gfx::Painter &p) override;

private:
    uint16_t _icon;
    C        _tint;
};

// ── Separator ───────────────────────────────────────────────────────────────
// A 1-px hairline across the parent's cross axis (horizontal by default).
class Separator : public View {
public:
    explicit Separator(bool vertical = false, C c = C::Border);
    void paint(gfx::Painter &p) override;

private:
    C _c;
};

// ── Image ───────────────────────────────────────────────────────────────────
// A shared bitmap (or animation) scaled into the frame. Bitmaps are shared
// (std::shared_ptr) so one decoded avatar serves every row that shows it.
class Image : public View {
public:
    enum class Fit : uint8_t { Fill /* stretch */, Contain, Cover };
    using Frames = std::vector<gfx::AnimFrame>;

    Image();
    ~Image() override;
    void               setBitmap(std::shared_ptr<const gfx::Bitmap> b);
    // Animated: frames advance on timers while the image is in a window.
    void               setFrames(std::shared_ptr<const Frames> f);
    void               setFit(Fit f);
    void               setRadius(float r);  // rounded corners (message images)
    void               setCircle(bool on);  // avatars
    void               setPlaceholder(C c); // fill while empty
    const gfx::Bitmap *bitmap() const;
    int                frameIndex() const { return _frame; }

    SizeF measureContent(float availW, float availH) override;
    void  paint(gfx::Painter &p) override;
    void  windowChanged() override;
    bool  showsBitmap(const gfx::Bitmap *b) const override;

    // Smooth shrinks made since the bitmap / frames were set (tests).
    int  shrinkCount() const { return _shrinks; }
    // Whether the next animation frame is due (tests).
    bool frameScheduled() const { return _timer != 0; }

private:
    void scheduleFrame();

    // A shown bitmap shrunk once to the pixels it was painted at (painting
    // then blits it 1:1): one for a still, one per frame of an animation.
    // Keyed by the source's pixels, so a bitmap filled in place (an avatar
    // that downloaded) is shrunk again.
    struct Shrunk {
        const uint32_t *src = nullptr;
        int             sw = 0, sh = 0, dw = 0, dh = 0;
        gfx::Bitmap     bmp;
    };
    std::vector<Shrunk>                _shrunk;
    int                                _shrinks = 0;
    std::shared_ptr<const gfx::Bitmap> _bitmap;
    std::shared_ptr<const Frames>      _frames;
    plat::TimerId                      _timer       = 0;
    float                              _radius      = 0;
    int                                _frame       = 0;
    Fit                                _fit         = Fit::Cover;
    C                                  _placeholder = C::None;
    bool                               _circle      = false;
};

// ── Popup ───────────────────────────────────────────────────────────────────
// An in-window overlay card (menus, tooltips, emoji picker, autocomplete).
// Show with Window::showPopup(); its content is its children (flex layout as
// usual). Placement is relative to an anchor rect in window coordinates,
// flipped to the other side when it does not fit, and clamped into the window.
// A modal popup (default) takes keyboard focus, closes on Escape and on a
// press outside it (that press is swallowed, like a native menu).
class Popup : public View {
public:
    // Right: beside the anchor, on its left when there is no room (submenus).
    // Fill covers the whole window, anchor ignored (Dialog's backdrop).
    // Cursor: a context menu at a point (zero-size anchor) — below-right of
    // it, flipped left/up where it would leave the window.
    enum class Place : uint8_t { Below, Above, Right, Over, Fill, Cursor, Tip };
    // Tip: centred above the anchor, below it when there is no room.
    Popup();
    ~Popup() override;

    void  setAnchor(RectF windowRect, Place p = Place::Below);
    RectF anchor() const { return _anchor; }
    Place place() const { return _place; }
    void  setModal(bool on) { _modal = on; }
    bool  modal() const { return _modal; }
    void  setCard(bool on) { _card = on; } // background, border, shadow (default on)
    void  close();

    std::function<void()> onClosed;

    void paint(gfx::Painter &p) override;
    bool onEvent(Event &e) override;

    // Internal: where the overlay places it for a window of `win` size.
    RectF        placeIn(SizeF win);
    // Internal: Window::closePopup calls it first (a menu closes its submenu).
    virtual void willClose() {}

private:
    RectF _anchor;
    Place _place = Place::Below;
    bool  _modal = true, _card = true;
};

// ── Menu ────────────────────────────────────────────────────────────────────
struct MenuItem {
    int                   id = 0; // passed to onSelect
    std::string           label;
    std::string           hint; // right-aligned shortcut text ("Ctrl+B")
    uint16_t              icon      = Button::kNoIcon;
    bool                  enabled   = true;
    bool                  checked   = false;
    bool                  separator = false; // a divider; other fields ignored
    bool                  danger    = false; // destructive action colour
    bool                  header    = false; // a dim section label ("Notify you about…")
    // Non-empty: a submenu (opens on hover, Right or Enter; its items report
    // to the root menu's onSelect).
    std::vector<MenuItem> sub;
    bool                  bold = false; // a spelling suggestion, in bold

    static MenuItem separatorItem();
    static MenuItem headerItem(std::string text);
};

// One row of a table-driven menu: its id, icon and label (untranslated,
// N_()), the label of the caller's variant where different (null: the same)
// and its shortcut hint (null: none).
struct MenuDef {
    uint8_t     id;
    uint16_t    icon;
    const char *label;
    const char *alt;
    const char *hint;
};
// Appends item `id` as `defs` describes it (label translated; `alt` picks
// the variant's label) and returns it for the caller to adjust.
MenuItem &addMenuItem(
    std::vector<MenuItem> &out, std::span<const MenuDef> defs, int id, bool alt, bool enabled
);
// Appends a separator, never a leading one and never two in a row.
void addMenuSeparator(std::vector<MenuItem> &out);

// A popup list of actions (a context menu). Keyboard:
// an item's hint is its shortcut ("E", "Del", "Ctrl+C" choose it), Up/Down/
// Home/End move, Enter/Space choose, Escape closes, other letters jump to the
// next item starting with them. Submenus open on a short hover, Right or
// Enter, and close with Left or Escape.
class Menu : public Popup {
public:
    Menu(std::vector<MenuItem> items, std::function<void(int id)> onSelect);
    ~Menu() override;
    // Convenience: create and show at `anchor` (a point = zero-size rect).
    static Menu *show(
        Window                     &w,
        RectF                       anchor,
        std::vector<MenuItem>       items,
        std::function<void(int id)> onSelect,
        Place                       p = Place::Below
    );

    // A context menu at a window point (Place::Cursor).
    // Rows are as wide as the widest needs; setMinWidth sets a floor
    // (140 for the workspace menus).
    static Menu *popupAt(
        Window &w, PointF at, std::vector<MenuItem> items, std::function<void(int id)> onSelect
    );

    void                         setMinWidth(float w);
    const std::vector<MenuItem> &items() const { return _items; }
    int                          current() const { return _current; }
    void                         setCurrent(int index);
    // The open submenu (null when none) and the menu this one hangs off.
    Menu                        *submenu() const { return _child; }
    Menu                        *parentMenu() const { return _parent; }
    // Opens item `index`'s submenu (selectFirst: keyboard opening).
    void                         openSubmenu(int index, bool selectFirst);
    // The row of the first item whose label starts with `label` (local
    // coordinates; empty when there is none) — the demo tour points at it.
    RectF                        itemRect(std::string_view label) const;

    SizeF measureContent(float availW, float availH) override;
    void  paint(gfx::Painter &p) override;
    bool  onEvent(Event &e) override;
    void  styleChanged() override;
    void  willClose() override;

private:
    int   itemAt(float y) const;
    float itemTop(int index) const;
    void  choose(int index);
    void  step(int dir);
    void  syncSubmenuLater(); // hover: open/close the submenu after a short pause
    bool  anyChecked() const;

    std::vector<MenuItem>                      _items;
    std::vector<std::unique_ptr<text::Layout>> _labels, _hints;
    std::function<void(int)>                   _onSelect;
    Menu                                      *_parent = nullptr, *_child = nullptr;
    plat::TimerId                              _hoverTimer = 0;
    float                                      _minW       = 0;
    int                                        _current = -1, _childFor = -1;
    int                                        _hoverTimerFor = -1; // the row it was armed on
};

} // namespace ui
