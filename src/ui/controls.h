// Form controls and the modal dialog frame, for Settings and every other
// dialog. They share one form look (radio buttons, check boxes, spin boxes,
// buttons, line edits, dropdowns and the dialog card; sizes from
// control_metrics.h) and paint from the Form* tokens:
//
//   CheckBox     an 18-px square, accent-filled with a check when on
//   Radio        an 18-px circle, accent ring and dot when chosen;
//                RadioGroup keeps a column of them exclusive
//   Dropdown     one of a list, chosen from a Menu
//   SpinBox      a number with a suffix ("14 days") and up/down arrows
//   TextField    a one-line input, small or normal (optionally masked with a
//                show/hide eye, a leading icon, a length counter)
//   TextArea     a fixed-height multi-line input
//   Button::Form the form look of Button (widgets.h): primary / secondary /
//                danger / ghost, normal or small
//   SectionList  a vertical list of page names (a settings dialog's left side)
//   Dialog       an in-window modal: a dimmed backdrop over the whole window
//                with a centred card, either Settings' fixed panel or the
//                titled card (a title, a close button, a content column
//                and a button row)
//
// Like widgets.h, each paints itself from theme tokens; only the composite
// ones (RadioGroup, TextField, TextArea, Dialog) have child views.
#pragma once

#include "ui/scroll.h"
#include "ui/textedit.h"
#include "ui/widgets.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// Height of the compact form controls and of the normal ones.
constexpr float kFormSmallH = 30, kFormNormalH = 38;

// ── CheckBox / Radio ────────────────────────────────────────────────────────
// A box (or circle) and its label; a click anywhere on it, Space or Enter
// toggles. onChange fires on user changes only, not on setChecked().
class CheckBox : public Clickable {
public:
    explicit CheckBox(std::string label = {}, bool on = false);
    ~CheckBox() override;
    void setLabel(std::string label);
    // The label's face (default Font::Control in FormText).
    void setLabelFont(Font f, C color);

    std::function<void(bool on)> onChange;

    void        activate() override;
    SizeF       measureContent(float availW, float availH) override;
    void        paint(gfx::Painter &p) override;
    void        styleChanged() override;
    std::string accessibleName() const override { return _label; }

protected:
    virtual void paintIndicator(gfx::Painter &p, RectF r);
    bool         _radio = false;

private:
    const text::Layout           *labelFor(float width);
    std::string                   _label;
    std::unique_ptr<text::Layout> _l;
    float                         _builtW        = -1;
    Font                          _font          = Font::Control;
    C                             _color         = C::FormText;
    bool                          _lEnabledState = true;
};

// A single radio: activate() only ever turns it on (RadioGroup, or the
// owner, turns the others off).
class Radio : public CheckBox {
public:
    explicit Radio(std::string label = {}, bool on = false);
    void activate() override;

protected:
    void paintIndicator(gfx::Painter &p, RectF r) override;
};

// A column of Radios, `spacing` apart, exactly one chosen.
class RadioGroup : public View {
public:
    explicit RadioGroup(const std::vector<std::string> &options, int selected = 0);
    void                           setSelected(int index);
    int                            selected() const { return _selected; }
    Radio                         *radio(int index) const;
    std::function<void(int index)> onChange; // user choices only

private:
    void choose(int index);
    int  _selected;
};

// ── Choice / Dropdown ───────────────────────────────────────────────────────
// Option labels and the selected index. onChange fires on user choices only.
class Choice : public Clickable {
public:
    explicit Choice(std::vector<std::string> options, int selected = 0);
    ~Choice() override;
    void                            setSelected(int index);
    int                             selected() const { return _selected; }
    const std::vector<std::string> &options() const { return _options; }
    void                            setOptions(std::vector<std::string> options, int selected);
    std::function<void(int index)>  onChange;

    void        styleChanged() override;
    std::string accessibleName() const override;

protected:
    void                choose(int index); // select + notify when it changed
    const text::Layout *label(size_t i);   // option i, Font::Body

    std::vector<std::string>                   _options;
    std::vector<std::unique_ptr<text::Layout>> _labels;
    int                                        _selected;
};

// A field showing the selected option and a chevron; opens a Menu of all
// options under itself. Sized to its widest option unless given a width.
class Dropdown : public Choice {
public:
    explicit Dropdown(std::vector<std::string> options, int selected = 0);
    SizeF measureContent(float availW, float availH) override;
    void  paint(gfx::Painter &p) override;
    void  paintOver(gfx::Painter &p) override;
    void  activate() override; // opens the menu
    Menu *menu() const { return _menu; }
    // A divider in the open menu after option `index` (-1 = none), between
    // groups of options.
    void  setSeparatorAfter(int index) { _separatorAfter = index; }

private:
    Menu *_menu           = nullptr;
    int   _separatorAfter = -1;
};

// ── SpinBox ─────────────────────────────────────────────────────────────────
// An integer in [min, max] shown with a suffix. The arrows and Up/Down/
// PageUp/PageDown step it; digits typed while focused replace it (committed
// on Enter or when focus leaves). onChange fires on user changes only.
class SpinBox : public View {
public:
    SpinBox(int value, int min, int max, std::string suffix);
    ~SpinBox() override;
    void                     setValue(int v);
    int                      value() const { return _value; }
    std::function<void(int)> onChange;

    SizeF       measureContent(float availW, float availH) override;
    void        paint(gfx::Painter &p) override;
    bool        onEvent(Event &e) override;
    uint8_t     cursorAt(PointF local) const override; // hand over the arrows
    void        styleChanged() override;
    std::string accessibleName() const override;

private:
    void                          step(int n);
    void                          commit();
    int                           arrowAt(PointF p) const; // +1 up, -1 down, 0 neither
    int                           _value, _min, _max;
    int                           _hoverArrow = 0;
    bool                          _typing = false; // digits typed since focus, not yet committed
    bool                          _fresh  = false; // focused, the number shown selected
    std::string                   _suffix, _typed;
    std::unique_ptr<text::Layout> _num, _suf;
};

// ── TextField / TextArea ────────────────────────────────────────────────────
// A bordered input around a TextEdit (the border thickens and darkens while
// the edit has focus).
class TextField : public View {
public:
    // StyledLineEdit's two heights: Small (30, the Settings forms) and
    // Normal (38, the dialogs).
    enum class Size : uint8_t { Small, Normal };
    explicit TextField(std::string placeholder = {}, bool multiLine = false);
    TextField(std::string placeholder, Size size, uint16_t leadingIcon = 0xffff);
    ~TextField() override;
    // A counter of the characters left (StyledLineEdit::setMaxLength);
    // input past `n` code points is cut. Takes edit().onChange.
    void                  setMaxLength(int n);
    TextEdit             &edit() const { return *_edit; }
    const std::string    &text() const { return _edit->text(); }
    void                  setText(std::string_view s) { _edit->setText(s); }
    // Masked input (API keys); reveal adds the eye button that toggles it.
    void                  setMasked(bool on, bool reveal = false);
    // StyledLineEdit::setPrefix: a muted label inside the box before the text.
    void                  setPrefix(std::string text);
    // The pickers' search look (the emoji and GIF pickers, the members
    // popup, the link popup): the border stays 1 px with focus, the icon is
    // faint, the text keeps the edit's own padding inside a 12-px inset.
    void                  setQuiet();
    bool                  masked() const;
    IconButton           *eye() const { return _eye; }
    std::function<void()> onReturn; // Enter in a one-line field

    void        paint(gfx::Painter &p) override;
    bool        onEvent(Event &e) override;
    std::string accessibleName() const override { return _edit->accessibleName(); }

private:
    void        updateCounter();
    TextEdit   *_edit;
    IconButton *_eye     = nullptr;
    Label      *_counter = nullptr;
    int         _max     = 0;
    uint16_t    _icon    = 0xffff;
    bool        _multi;
    bool        _quiet = false;
};

// ── SectionList ─────────────────────────────────────────────────────────────
// A column of page names with one selected. Up/Down/Home/End move the
// selection when focused. Paints its own sunken background (rounded at the
// bottom-left, where it meets the dialog's corner) and right-hand rule.
class SectionList : public View {
public:
    explicit SectionList(std::vector<std::string> items);
    ~SectionList() override;
    void                           setSelected(int index);
    int                            selected() const { return _selected; }
    std::function<void(int index)> onChange; // user choices only

    SizeF   measureContent(float availW, float availH) override;
    void    paint(gfx::Painter &p) override;
    bool    onEvent(Event &e) override;
    uint8_t cursorAt(PointF local) const override; // hand over a row
    void    styleChanged() override;

private:
    int  rowAt(float y) const;
    void choose(int index);

    std::vector<std::string>                   _items;
    std::vector<std::unique_ptr<text::Layout>> _labels;
    int                                        _selected = 0, _hover = -1;
};

// ── Dialog ──────────────────────────────────────────────────────────────────
// A modal Popup that covers the window: a dimmed backdrop and a centred
// card, panel(), holding the content (shrunk to fit small windows). Escape,
// the close button or a press on the backdrop reject it. Never a top-level
// OS window: those misbehave on Wayland (compositor-chosen positions, no
// reliable modality). Two frames:
//   Dialog(w, h)          the Settings window: a fixed w × h panel with a
//                         1-px border and an 8-px radius over rgba(0,0,0,150);
//   Dialog(title, cardW)  a dialog: a 12-px card as tall as its content
//                         (cardW 0: clamp(window − 80, min(480, …), 560)),
//                         a bold ×1.45 title and a round close button over
//                         rgba(0,0,0,140), a soft 40-px shadow; content() is
//                         a column, 12 apart. The header and content() carry
//                         the 28/24 padding, not the card, and the cross
//                         lines up with content()'s right edge. An empty
//                         title: a bare card, no header and no padding, for
//                         dialogs with their own chrome edge to edge (the
//                         finders); children are clipped to its corners.
class Dialog : public Popup {
public:
    // The titled card's content: wrapped in a
    // scroll view (the default), so a card taller than the window scrolls —
    // its buttons included — instead of clipping. A dialog that scrolls its
    // own body, sized around it, passes Disabled (no nested scrolling).
    enum class Scroll : uint8_t { Enabled, Disabled };
    // The cross's inset in its 32-px close button: a row ending in one pads
    // its right side this much less, so the cross meets the content edge.
    static constexpr float kCloseInset = 9;
    // height <= 0: as tall as the panel's content at that width.
    Dialog(float width, float height);
    explicit Dialog(std::string title, float cardWidth = 0, Scroll scroll = Scroll::Enabled);
    ~Dialog() override;
    View       *panel() const { return _panel; }
    View       *content() const { return _content; } // the titled form only
    ScrollView *scroller() const { return _scroll; } // null: Scroll::Disabled or untitled

    // [leading] stretch [secondary] [primary]; secondary rejects.
    View          *addButtonRow(Button *primary, Button *secondary, View *leading = nullptr);
    // A Normal-size button for addButtonRow (which adopts it).
    static Button *makeButton(std::string label, Button::Kind k);
    // A confirmation dialog: `title`, `text` as a 15-px paragraph in
    // `textColor` (a themed() sentinel or a colour), whatever `extra` adds
    // below it, then [Cancel] [confirmLabel]. Confirming accepts (onAccepted
    // runs); Cancel and × reject. Not shown yet.
    static std::unique_ptr<Dialog> confirm(
        std::string                               title,
        std::string                               text,
        std::string                               confirmLabel,
        Button::Kind                              kind,
        Color                                     textColor,
        const std::function<void(View *content)> &extra = {}
    );

    void                  accept(); // closes, then onAccepted
    void                  reject(); // closes, then onRejected
    std::function<void()> onAccepted, onRejected;

    SizeF measureContent(float availW, float availH) override;
    void  layout() override;
    void  paint(gfx::Painter &p) override;
    bool  onEvent(Event &e) override;

private:
    View       *_panel, *_content = nullptr;
    ScrollView *_scroll = nullptr;
    float       _w, _h; // _h 0: the titled card, as tall as its content
    bool        _done = false;
};

// A label in one text::Style (pxFont sizes).
Label *styledLabel(View *parent, std::string text, const text::Style &st, int maxLines = 0);

} // namespace ui
