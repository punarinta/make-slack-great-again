// ui core: geometry, the flex Style, View (the retained tree node), Event,
// Window and App. See ui.h for the overview; widgets live in widgets.h,
// scroll.h and textedit.h.
#pragma once

#include "gfx/gfx.h"
#include "plat/plat.h"
#include "ui/theme.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

using gfx::Color;
using gfx::PointF;
using gfx::RectF;

class View;
class Window;
class App;
class Popup;

struct SizeF {
    float w = 0, h = 0;
};
struct Edges {
    float l = 0, t = 0, r = 0, b = 0;
};

// "No fixed size" for Style::w/h, and the "unbounded" available size.
constexpr float kAuto = -1;
constexpr float kInf  = 1e9f;

RectF       intersect(RectF a, RectF b);
RectF       unite(RectF a, RectF b);
inline bool empty(RectF r) {
    return r.w <= 0 || r.h <= 0;
}
inline bool overlaps(RectF a, RectF b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
inline bool sameRect(RectF a, RectF b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// ── Layout style ────────────────────────────────────────────────────────────
// A compact flexbox: children laid out along `dir`, `gap` between them,
// `pad` inside the view, `margin` outside each child. Sizes are logical px.
//
// Main-axis sizing per child: a fixed `w`/`h` wins; otherwise a child with
// grow > 0 starts from 0 and takes its share of the free space (CSS
// `flex: <grow> 1 0`), and every other child starts from its measured size and
// gives space back in proportion to `shrink` when the line overflows (never
// below minW/minH). Cross axis: `align` (per child `self`) — Stretch fills.
// Stack overlays all children in the content box, aligned by `align` on both
// axes. None leaves child frames to an overriding layout().
enum class Dir : uint8_t { Column, Row, Stack, None };
enum class Align : uint8_t { Auto, Start, Center, End, Stretch };
enum class Justify : uint8_t { Start, Center, End, SpaceBetween };

struct Style {
    float   w = kAuto, h = kAuto;
    float   minW = 0, minH = 0, maxW = kInf, maxH = kInf;
    Edges   pad, margin;
    float   gap = 0, grow = 0, shrink = 1;
    Dir     dir     = Dir::Column;
    Align   align   = Align::Stretch; // children, cross axis
    Align   self    = Align::Auto;    // this view inside its parent
    Justify justify = Justify::Start;

    // Fluent setters for building trees tersely: v->style().row().gap(8).padding(8, 4).
    Style &row() {
        dir = Dir::Row;
        return *this;
    }
    Style &column() {
        dir = Dir::Column;
        return *this;
    }
    Style &stack() {
        dir = Dir::Stack;
        return *this;
    }
    Style &size(float ww, float hh) {
        w = ww;
        h = hh;
        return *this;
    }
    Style &width(float v) {
        w = v;
        return *this;
    }
    Style &height(float v) {
        h = v;
        return *this;
    }
    Style &padding(float v) {
        pad = {v, v, v, v};
        return *this;
    }
    Style &padding(float x, float y) {
        pad = {x, y, x, y};
        return *this;
    }
    Style &padding(float l, float t, float r, float b) {
        pad = {l, t, r, b};
        return *this;
    }
    Style &margins(float l, float t, float r, float b) {
        margin = {l, t, r, b};
        return *this;
    }
    Style &spacing(float v) {
        gap = v;
        return *this;
    }
    Style &flex(float g) {
        grow = g;
        return *this;
    }
    Style &noShrink() {
        shrink = 0;
        return *this;
    }
    Style &items(Align a) {
        align = a;
        return *this;
    }
    Style &alignSelf(Align a) {
        self = a;
        return *this;
    }
    Style &justifyContent(Justify j) {
        justify = j;
        return *this;
    }
};

// ── Events ──────────────────────────────────────────────────────────────────
// The modifier bits keys and shortcuts compare (locks and buttons left out).
constexpr uint32_t kModMask = plat::ModShift | plat::ModCtrl | plat::ModAlt | plat::ModSuper;

// What a View sees. Pointer positions are local to the receiving view (they
// are re-mapped while an unhandled event bubbles to the parent). Rarely used
// payloads (text, preedit cursor, drop items) stay in `raw`.
enum class EventType : uint8_t {
    PointerEnter, // the pointer entered this view or a descendant (hover chain)
    PointerLeave,
    PointerMove, // to the hovered view, or to the capturing view while a button is held
    PointerDown, // the handler (first view returning true) captures the pointer until Up
    PointerUp,
    Scroll,
    KeyDown, // to the focused view, bubbling; unhandled → shortcuts → Tab traversal
    KeyUp,
    TextInput,   // committed text (raw->text), to the focused view only
    TextPreedit, // IME composition (raw->text, raw->preeditCursorBegin/End)
    FocusIn,     // keyboard focus arrived (also when the window is re-activated)
    FocusOut,
    ContextMenu, // right click, long press, Menu key or Shift+F10; pos = where to open the menu
    DropEnter,   // DnD target hooks: set `dropAction` and return true to accept
    DropMove,
    DropLeave,
    Drop, // raw->items / raw->uris / raw->text carry the data
    // The press the capturing view got is over without a release (a long
    // press turned into a context menu): drop pressed state, don't click.
    PointerCancel,
};

struct Event {
    EventType          type;
    PointF             pos;       // local
    PointF             windowPos; // window coordinates
    plat::Button       button = plat::Button::Left;
    int                clicks = 0; // PointerDown: 1, 2 (double), 3 …
    uint32_t           mods   = 0; // plat::Mod bits
    plat::Key          key    = plat::Key::Unknown;
    bool               repeat = false;
    float              dx = 0, dy = 0; // Scroll: see ScrollArea for units
    bool               precise    = false;
    plat::ScrollPhase  phase      = plat::ScrollPhase::None;
    plat::DropAction   dropAction = plat::DropAction::None; // Drop*: the handler's answer
    const plat::Event *raw        = nullptr;
};

// Accessibility role, kept on every view so a later AT-SPI/UIA/NSAccessibility
// bridge can walk the tree (name comes from View::accessibleName()).
enum class Role : uint8_t {
    None,
    Group,
    Text,
    Button,
    CheckBox,
    Tab,
    Image,
    List,
    ListItem,
    TextInput,
    Menu,
    MenuItem,
    ScrollArea,
    Separator,
    Tooltip,
    Badge,
};

// ── View ────────────────────────────────────────────────────────────────────
// A node of the retained tree: owns its children, has a frame (relative to
// the parent, logical px) set by the parent's layout, and virtual hooks for
// measuring, laying out, painting and events. Not copyable; single-threaded.
//
// Invalidation:
//   update()            repaint this view (its rect is added to the window damage)
//   invalidateLayout()  re-measure this view and re-layout up to the nearest
//                       layout boundary (a view whose size cannot depend on its
//                       content: fixed w and h, a ScrollArea, the window root)
// Both are cheap to call often; the work happens once per frame.
class View {
public:
    View();
    virtual ~View();
    View(const View &)            = delete;
    View &operator=(const View &) = delete;

    // ── Tree ────────────────────────────────────────────────────────────────
    View   *parent() const { return _parent; }
    Window *window() const { return _window; }
    // The window's device scale; 1 while not in one.
    float   windowScale() const;
    size_t  childCount() const { return _children.size(); }
    View   *child(size_t i) const { return _children[i].get(); }
    // Takes ownership; index < 0 appends. Returns the raw pointer.
    View   *adopt(std::unique_ptr<View> v, int index = -1);
    template <class T, class... A>
    T *add(A &&...a) {
        return static_cast<T *>(adopt(std::unique_ptr<View>(new T(static_cast<A &&>(a)...))));
    }
    std::unique_ptr<View> remove(View *child); // returns ownership (null if not a child)
    void                  clearChildren();
    bool                  isAncestorOf(const View *v) const; // true for v == this too

    // ── Style & geometry ────────────────────────────────────────────────────
    // Mutable access marks the layout dirty (it is assumed you change it).
    Style       &style();
    const Style &style() const { return _style; }
    // A logical offset inside this view moved onto the physical pixel grid.
    // Layout puts view edges on the grid; content placed at offsets within a
    // view (text origins, centred icons) must be too, or at fractional
    // scales it sits on a half pixel, where float noise flips the rounding
    // of glyph rows between two paints of the same thing (text one pixel off
    // after a scroll blit).
    float        snapPx(float logical) const;
    PointF       snapPx(PointF p) const { return {snapPx(p.x), snapPx(p.y)}; }

    // Read-only access that never marks the layout dirty (from non-const code).
    const Style &currentStyle() const { return _style; }
    const RectF &frame() const { return _frame; }
    float        width() const { return _frame.w; }
    float        height() const { return _frame.h; }
    RectF        bounds() const { return {0, 0, _frame.w, _frame.h}; }
    // Layouts call this. Damages the old and new rect when it moves.
    void         setFrame(RectF f);
    PointF       mapToWindow(PointF local) const;
    PointF       mapFromWindow(PointF win) const;
    // This view's rect in window coordinates, clipped by clipping ancestors.
    RectF        windowRect() const;
    // windowRect() grown by the paint outset (what update() damages).
    RectF        damageRect() const;

    // Measured border-box size for the given available space; cached until
    // invalidateLayout(). Applies w/h/min/max and padding around measureContent().
    SizeF measure(float availW, float availH);

    // ── State ───────────────────────────────────────────────────────────────
    bool    visible() const { return _flags & Visible; }
    void    setVisible(bool on);
    // Disabled views get no pointer/keyboard events and paint dimmed where
    // they choose to; enabled() is false when any ancestor is disabled.
    bool    enabled() const;
    void    setEnabled(bool on);
    bool    hovered() const { return _flags & Hovered; } // pointer over this or a descendant
    bool    pressed() const { return _flags & Pressed; } // set by Clickable while held
    bool    focused() const { return _flags & Focused; }
    bool    focusable() const { return _flags & Focusable; }
    void    setFocusable(bool on, bool focusOnClick = true);
    // Focus by click only, never by Tab.
    void    setClickFocus(bool on) { setFlag(FocusOnClick, on); }
    void    focus(); // keyboard focus to this view (if attached)
    void    setClipChildren(bool on) { setFlag(ClipChildren, on); }
    // Pointer hit testing skips this view itself (its children are still hit).
    void    setHitTransparent(bool on) { setFlag(HitTransparent, on); }
    void    setLayoutBoundary(bool on) { setFlag(LayoutBoundary, on); }
    // Repaint when hover changes (for views whose look depends on hovered()).
    void    setHoverRepaint(bool on) { setFlag(HoverRepaint, on); }
    // Pointer cursor while over this view (children inherit unless they set one).
    void    setCursor(plat::Cursor c);
    // Extra paint area beyond the frame (drop shadows), for damage and culling.
    void    setPaintOutset(uint8_t px) { _outset = px; }
    uint8_t paintOutset() const { return _outset; }

    // Background fill and 1-px border from theme tokens (see theme.h), painted
    // by View::paint(). C::None = none.
    void setBackground(C token, float radius = 0);
    void setBorder(C token);
    // True when paint() covers the whole frame with an opaque colour (a
    // background token with full alpha and square corners). Scroll areas
    // blit only then: through a transparent one, the backdrop would move.
    bool opaqueBackground() const;

    Role                role() const { return _role; }
    void                setRole(Role r) { _role = r; }
    virtual std::string accessibleName() const { return {}; }
    // Tooltip text shown after the pointer rests on this view; empty = none.
    virtual std::string tooltip() const { return {}; }
    // What the tooltip points at, in window coordinates (default: the view).
    virtual RectF       tooltipAnchor() const;
    // Shown on hover without the delay (the toolbar and link tips).
    virtual bool        tooltipImmediate() const { return false; }

    // ── Invalidation ────────────────────────────────────────────────────────
    void update();
    void update(RectF local);
    void invalidateLayout();

    // ── Animation ───────────────────────────────────────────────────────────
    // Ask for tick() once per displayed frame (plat's Frame event paces it)
    // until tick() returns false.
    void startTicking();
    // Have hiddenByAncestor() called (a dictation or an offer that only means
    // something on screen). A list per window: costs nothing for the rest.
    void watchAncestorHide();

    // ── Hooks ───────────────────────────────────────────────────────────────
    // Content size inside the padding. Default: the flex algorithm over the
    // children (per style().dir). Leaves (text, images) override.
    virtual SizeF   measureContent(float availW, float availH);
    // Position children. Default: the flex algorithm.
    virtual void    layout();
    // Local coordinates, clipped to the damage; children paint afterwards.
    virtual void    paint(gfx::Painter &p);
    // After the children (scrollbars, focus rings, overlays within the view).
    virtual void    paintOver(gfx::Painter &p) {}
    // True while paintOver draws hover-only content -- scrolling can't just
    // shift the old pixels for that (nothing repaints it while state stays put).
    virtual bool    hasLiveOverlay() const { return false; }
    // Return true when handled (stops bubbling; for PointerDown also captures).
    virtual bool    onEvent(Event &e) { return false; }
    // Deepest view at `local` (default: children front to back, then this).
    virtual View   *hitTest(PointF local);
    // Cursor at a point; 0xff (kCursorInherit) = ask the parent.
    virtual uint8_t cursorAt(PointF local) const { return _cursor; }
    // Theme or display scale changed: drop cached text layouts and colours.
    // Default repaints. Called on the whole tree.
    virtual void    styleChanged() { update(); }
    // Joined or left a window (w == null). Views with timers start/stop them.
    virtual void    windowChanged() {}
    // setVisible changed it (not an ancestor's visibility).
    virtual void    visibilityChanged(bool) {}
    // An ancestor was hidden (setVisible(false) above this view). Only for views that asked:
    // watchAncestorHide().
    virtual void    hiddenByAncestor() {}
    virtual bool    tick(double nowMs) { return false; }
    // A press anywhere in the window: stop flings and smooth scrolls.
    virtual void    interruptAnimation() {}
    // Whether paint() draws `b` (Window::damageShowing): a view holding a
    // shared bitmap says so, and repaints alone when it is filled in place.
    virtual bool    showsBitmap(const gfx::Bitmap *b) const;
    // Tab moved the focus to `descendant`: a scroll container scrolls it into
    // view.
    virtual void    revealFocus(const View *descendant) {}

    static constexpr uint8_t kCursorInherit = 0xff;

    // Position without damage: for scroll containers, which move content and
    // blit/damage themselves.
    void setFrameQuiet(RectF f);

protected:
    // The flex algorithm, for overrides that want to extend it.
    SizeF measureFlex(float availW, float availH);
    void  layoutFlex();

    enum Flag : uint32_t {
        Visible        = 1u << 0,
        Disabled       = 1u << 1,
        Focusable      = 1u << 2,
        FocusOnClick   = 1u << 3,
        ClipChildren   = 1u << 4,
        HitTransparent = 1u << 5,
        LayoutBoundary = 1u << 6,
        NeedsLayout    = 1u << 7,
        SubtreeDirty   = 1u << 8,
        MeasureValid   = 1u << 9,
        Hovered        = 1u << 10,
        Pressed        = 1u << 11,
        Focused        = 1u << 12,
        HoverRepaint   = 1u << 13,
        Ticking        = 1u << 14,
        WatchesHide    = 1u << 15,
        UserFlag0      = 1u << 24, // free for subclasses
        UserFlag1      = 1u << 25,
        UserFlag2      = 1u << 26,
        UserFlag3      = 1u << 27,
    };
    bool flag(uint32_t f) const { return _flags & f; }
    void setFlag(uint32_t f, bool on) { _flags = on ? (_flags | f) : (_flags & ~f); }

private:
    friend class Window;
    void  setWindow(Window *w);
    void  unwatchHide();
    void  markParentsDirty();
    RectF clippedRect(float outset) const;

    View                              *_parent = nullptr;
    Window                            *_window = nullptr;
    std::vector<std::unique_ptr<View>> _children;
    RectF                              _frame;
    Style                              _style;
    float                              _mcW = -1, _mcH = -1;           // measure cache key
    SizeF                              _mc;                            // … and value
    float                              _mc2W = -1e30f, _mc2H = -1e30f; // the one before
    SizeF                              _mc2;
    uint32_t                           _flags  = Visible;
    uint8_t                            _cursor = kCursorInherit;
    uint8_t                            _outset = 0;
    uint8_t                            _bg = 0, _border = 0;
    float                              _radius = 0;
    Role                               _role   = Role::None;
};

// ── Window ──────────────────────────────────────────────────────────────────
// A plat window plus the view tree in it: root() for the screen, an overlay
// layer for popups/menus/tooltips (in-window, never top-level windows), input
// routing, focus, shortcuts and damage-driven painting.
class Window {
public:
    explicit Window(const plat::WindowDesc &desc);
    ~Window();
    Window(const Window &) = delete;

    plat::Window &native() { return *_native; }
    float         scale() const { return _scale; }
    SizeF         size() const;
    // The root is a Stack filling the window (background C::WindowBg): add one
    // screen view to it and it fills the window.
    View         &root() { return *_root; }

    // ── Focus ───────────────────────────────────────────────────────────────
    View *focusView() const { return _focus; }
    void  setFocus(View *v, bool visible = false); // null clears; visible: keep the focus ring
    // Next/previous focusable view in tree order (inside the top modal popup
    // if one is open). Returns false when there is none.
    bool  focusNext(bool backward);
    bool  isActive() const { return _active; }
    // True when focus last moved by keyboard (Tab): draw focus rings then only.
    bool  focusVisible() const { return _focusVisible; }

    // ── Shortcuts ───────────────────────────────────────────────────────────
    // Fires on an unhandled KeyDown with exactly these modifiers. kPrimary
    // means Cmd on macOS and Ctrl elsewhere. Returns an id for removeShortcut.
    static constexpr uint32_t kPrimary = 1u << 16;
    int                       addShortcut(plat::Key key, uint32_t mods, std::function<void()> fn);
    void                      removeShortcut(int id);
    // Sees every KeyDown before the focused view does (an app-wide key
    // filter): return true to consume it.
    std::function<bool(const Event &)>       keyFilter;
    // Sees every PointerDown/PointerUp, Scroll and touchpad gesture before
    // any view does (app-wide back/forward from side buttons and swipes):
    // return true to consume it.
    std::function<bool(const plat::Event &)> inputFilter;

    // ── Popups ──────────────────────────────────────────────────────────────
    // Shows p in the overlay layer, positioned by its anchor (Popup::setAnchor).
    Popup       *showPopup(std::unique_ptr<Popup> p);
    // Removes it (deferred to the end of the current event, so a popup can
    // close itself from its own handler). Focus returns where it was.
    void         closePopup(Popup *p);
    void         closeAllPopups();
    Popup       *topPopup() const;
    // The topmost popup `accept` says yes to (null when none).
    Popup       *topPopup(const std::function<bool(const Popup &)> &accept) const;
    // The tooltip currently shown (null when none).
    const Popup *tooltip() const { return _tooltip; }
    // The hovered view's tooltip changed (a different link under the
    // pointer): hide it and start the delay again.
    void         rearmTooltip();

    // ── Pointer ─────────────────────────────────────────────────────────────
    // The view a press at `windowPos` would reach (popups first), null when
    // none: a hit-test callback telling a title bar's controls from the
    // empty space that drags the window.
    View  *viewAt(PointF windowPos) { return hitAt(windowPos); }
    View  *capture() const { return _capture; }
    PointF pointerPos() const { return _pointer; }
    bool   startDrag(const plat::DragDesc &d);

    // ── IME ─────────────────────────────────────────────────────────────────
    // caret in window coordinates; TextEdit calls it while focused.
    void setTextInput(bool enabled, RectF caret);

    // ── Painting ────────────────────────────────────────────────────────────
    void damage(RectF windowRect);
    void damageAll();
    // Repaints only the shown views that draw one of `bitmaps`
    // (View::showsBitmap): shared pictures that were filled in place.
    void damageShowing(const std::vector<std::shared_ptr<const gfx::Bitmap>> &bitmaps);
    void requestFrame();
    // Shift the pixels already on the canvas inside `area`'s rect up by
    // dyPhys physical pixels (negative = down) at the start of the next frame
    // instead of repainting them — scroll areas use it and repaint only the
    // strip that scrolled in. Views painted above the area (later siblings of
    // it or its ancestors, popups) are repainted where they are and where
    // their pixels were carried to. The shifted rect is still reported to
    // the backend as changed (see onFrame), only its painting is saved.
    void scrollBlit(View *area, int dyPhys);

#ifdef MSGA_UI_VERIFY // test and demo builds only
    // Debug: after every frame, check that what a damage-only presenter
    // shows (a shadow copy updated only through the rects handed to
    // endPaint, as X11/Wayland/Win32 do) equals a full repaint of the tree.
    // Mismatches are counted in stats() and logged to stderr. Also on with
    // $UI_VERIFY=1; $UI_VERIFY_DUMP=<dir> writes the first mismatches as PPMs.
    void setVerify(bool on) { _verify = on; }
    // Debug: the whole window painted from scratch, written as a binary PPM
    // (to compare with a screenshot of what the compositor shows).
    bool dumpFullRepaint(const std::string &path);
#endif

    // Callbacks. onCloseRequested defaults to App::quit() when unset.
    std::function<void()>                    onCloseRequested;
    std::function<void(const plat::Event &)> onEvent; // StateChanged, Moved, FocusIn/Out, …

    // Diagnostics (tests, perf numbers).
    struct Stats {
        int                     frames = 0;
        std::vector<plat::Rect> lastDamage; // physical rects repainted in the last frame
        int                     viewsPainted = 0;
        double                  lastFrameMs = 0, lastLayoutMs = 0, lastPaintMs = 0;
        int                     verifiedFrames = 0, verifyMismatches = 0; // setVerify()
        int                     verifyNoiseFrames = 0; // only ±1 AA rounding differed
        plat::Rect              firstMismatch;
    };
    const Stats &stats() const { return _stats; }

    // Internal: plat event entry point (App routes by plat::Window::userData).
    void handle(const plat::Event &e);
    // Internal: v leaves the window, is hidden or is destroyed: drops the
    // focus, hover, capture and so on held inside it.
    void forget(View *v);
    void styleChangedAll();
    void refreshCursor(); // re-evaluate the pointer cursor (a view changed its own)
    // Content moved under a still pointer (scrolling): re-hit-test the hover
    // chain once the frame's layout is done.
    void refreshHover() { _hoverDirty = true; }

private:
    friend class View;
    struct Shortcut {
        int                   id;
        plat::Key             key;
        uint32_t              mods;
        std::function<void()> fn;
    };
    struct Blit {
        RectF rect;
        int   dy;
    };

    void  onFrame(bool requested);
    void  layoutPass(View *v);
    void  paintTree(View *v, gfx::Painter &p, RectF damage, float ox, float oy);
    View *hitAt(PointF winPos);
    void  updateHover(View *target);
    bool  dispatch(View *target, Event &e); // bubbles; returns the handler via _handled
    void  pointerDown(const plat::Event &e);
    void  pointerUp(const plat::Event &e);
    void  pointerMove(PointF pos, const plat::Event &e);
    void  key(const plat::Event &e);
    void  drop(const plat::Event &e);
    void  scroll(const plat::Event &e);
    void  longPress();
    void  armTooltip();
    void  showTooltip();
    void  hideTooltip();
    void  flushGraveyard();
#ifdef MSGA_UI_VERIFY
    void verifyFrame(const plat::Canvas &c, const std::vector<plat::Rect> &presented);
#endif
    void resized();

    std::unique_ptr<plat::Window>      _native;
    std::unique_ptr<View>              _root, _overlay;
    float                              _scale = 1;
    View                              *_focus = nullptr, *_capture = nullptr, *_dropView = nullptr;
    View                              *_scrollLatch = nullptr, *_handled = nullptr;
    std::vector<View *>                _hoverChain; // leaf first
    std::vector<View *>                _hoverNext;  // updateHover's scratch
    std::vector<View *>                _ticking, _tickScratch;
    std::vector<View *>                _hideWatchers; // watchAncestorHide()
    std::vector<View *>                _focusStack;   // focus to restore per popup
    std::vector<std::unique_ptr<View>> _graveyard;
    std::vector<Shortcut>              _shortcuts;
    std::vector<RectF>                 _damage, _movedScratch;
    std::vector<Blit>                  _blits;
    std::vector<plat::Rect>            _presented; // onFrame's scratch
    PointF                             _pointer;
    Popup                             *_tooltip      = nullptr;
    View                              *_tooltipFor   = nullptr;
    plat::TimerId                      _tooltipTimer = 0, _longPressTimer = 0;
    PointF                             _pressPos;
    int                                _nextShortcut = 1, _canvasW = 0, _canvasH = 0;
    uint8_t                            _cursor = 0xfe;
    // Optimistic: a new window is normally activated, and some setups (no WM)
    // never send FocusIn; FocusOut corrects it.
    bool                               _active = true, _frameRequested = false, _inFrame = false;
    bool                               _dying = false, _textInput = false, _buttonHeld = false;
    bool                               _focusVisible = false, _frameAgain = false;
    bool                               _hoverDirty = false, _pointerInside = false;
    gfx::PaintScratch                  _paintScratch; // the frame painters' working memory
#ifdef MSGA_UI_VERIFY
    bool                  _verify = false;
    std::vector<uint32_t> _shadow; // setVerify(): what a damage-only presenter shows
    int                   _shadowW = 0, _shadowH = 0;
    double                _verifyMs = 0;
#endif
    int   _depth = 0; // handle() nesting, for the graveyard
    RectF _textCaret;
    Stats _stats;
};

// ── App ─────────────────────────────────────────────────────────────────────
enum class ThemeMode : uint8_t { System, Light, Dark };

// The toolkit's process singleton: the plat app, the event loop, timers, the
// theme and system settings. Create exactly one before any Window.
class App {
public:
    // Creates the plat app ($PLAT_BACKEND honoured) and initialises text.
    static std::unique_ptr<App> create(std::string *error = nullptr);
    static App                 *instance() { return s_instance; }
    ~App();

    plat::App &platform() { return *_plat; }
    void       run() { _plat->run(); }
    void       quit() { _plat->quit(); }
    void       pump(int timeoutMs) { _plat->pump(timeoutMs); }

    plat::TimerId addTimer(int ms, bool repeat, std::function<void()> fn);
    void          cancelTimer(plat::TimerId id);
    double        nowMs() const; // monotonic

    ThemeMode                   themeMode() const { return _mode; }
    void                        setThemeMode(ThemeMode m); // live: restyles every window
    bool                        dark() const { return _dark; }
    const plat::SystemSettings &settings() const { return _settings; }
    bool                        reducedMotion() const { return _settings.reducedMotion; }
    // The app's own text size preference, multiplied into the OS one (Font
    // sizes in theme.h); live, like setThemeMode. 1 = as the OS says.
    float                       userTextScale() const { return _userTextScale; }
    void                        setUserTextScale(float s);
    // Theme tokens changed outside the mode (a palette pick): restyle every window.
    void                        restyle();

    // App-level plat events (tray, notifications, OpenUrls, …). Unset, a
    // QuitRequested quits.
    std::function<void(const plat::Event &)> onEvent;

    // Internal.
    void addWindow(Window *w);
    void removeWindow(Window *w);

private:
    App() = default;
    void handle(const plat::Event &e);
    void refreshTheme();

    static App                *s_instance;
    std::unique_ptr<plat::App> _plat;
    std::vector<Window *>      _windows;
    plat::SystemSettings       _settings;
    ThemeMode                  _mode          = ThemeMode::System;
    float                      _userTextScale = 1;
    bool                       _dark          = false;
};

inline App *app() {
    return App::instance();
}

} // namespace ui
