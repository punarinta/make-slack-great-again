// Scrolling: ScrollArea (input, kinetic, scrollbar, blit-scrolling shared by
// both), ScrollView (one content view) and VirtualList (millions of rows,
// only the visible ones exist as views).
#pragma once

#include "ui/view.h"

#include <functional>
#include <memory>
#include <vector>

namespace ui {

// ── ScrollArea ──────────────────────────────────────────────────────────────
// Vertical scrolling input and chrome, shared by ScrollView and VirtualList:
//   wheel notches  → a smooth ~100 ms glide per notch (instant with reduced motion)
//   precise deltas → applied 1:1; with ScrollPhase (Wayland, macOS) a lift
//                    (End) starts our own kinetic fling on Linux/Windows —
//                    macOS sends Momentum events instead and we just apply them
//   scrollbar      → thin overlay thumb, wider on hover, draggable; a press on
//                    the track pages
//   keys           → PageUp/PageDown/Home/End/Up/Down when they bubble here
// Moving the content blits the pixels already on the canvas (Window::
// scrollBlit) and repaints only the strip that scrolled in — when the area
// paints an opaque background (setBackground(C::Surface) etc.); a transparent
// area repaints its viewport, since its backdrop does not scroll.
class ScrollArea : public View {
public:
    ScrollArea();

    float         scrollOffset() const { return offset(); } // content px above the viewport
    virtual float contentExtent() const = 0; // total content height (may be estimated)
    bool          canScroll() const { return contentExtent() > height() + 0.5f; }
    bool          atEnd() const { return offset() >= contentExtent() - height() - 0.5f; }

    // Relative / absolute scrolling; animated honours reduced motion.
    void         scrollBy(float dy, bool animated = false);
    virtual void scrollTo(float y, bool animated = false);
    void         stopScrolling(); // cancels glide/fling

    // After every offset change (load older history near the top, etc.).
    std::function<void()> onScroll;

    // An overlay thumb for lists instead of the default bar: a fixed
    // 4 px pill in one colour (no hover widening or darkening), full height,
    // 2 px from the edge; only the thumb itself grabs (a press beside it
    // goes to the row under it) and shows a resize cursor.
    void setThinThumb(C color);

    bool    onEvent(Event &e) override;
    void    paintOver(gfx::Painter &p) override;
    bool    tick(double nowMs) override;
    void    interruptAnimation() override { stopScrolling(); }
    View   *hitTest(PointF local) override;
    uint8_t cursorAt(PointF local) const override;

protected:
    // Subclasses move their content by dy (+ = towards the end) and return
    // the distance actually moved (0 at an edge).
    virtual float scrollPixels(float dy) = 0;
    virtual void  setOffset(float y)     = 0;
    virtual float offset() const         = 0;
    // Call after the content moved from oldOffset to newOffset by scrolling
    // alone: blits (when `blit`) and notifies.
    void          moved(float oldOffset, float newOffset, bool blit = true);
    RectF         barZone() const; // the strip the scrollbar lives in (local)
    // Custom animation hook for subclasses (VirtualList's jump-to-item).
    virtual bool  tickCustom(double dtMs) { return false; }
    void          startCustomAnimation();

private:
    RectF thumbRect() const;
    float barMargin() const; // above/below the thumb's track
    bool  inBarZone(PointF p) const;
    void  startGlide(float dy);

    enum class Anim : uint8_t { None, Glide, Fling, Custom };
    double _lastTick = 0, _lastPrecise = 0;
    float  _glideLeft = 0, _velocity = 0, _dragGrab = 0;
    Anim   _anim     = Anim::None;
    bool   _barHover = false, _dragging = false;
    C      _thinColor = C::None; // setThinThumb; None = the default bar
};

// ── ScrollView ──────────────────────────────────────────────────────────────
// Scrolls one content view (a Column by default) measured at the viewport
// width. A layout boundary by default; setLayoutBoundary(false) lets a
// parent size it to its content (up to maxH — popups, pickers).
class ScrollView : public ScrollArea {
public:
    ScrollView();
    View *content() const { return _content; }
    View *setContent(std::unique_ptr<View> v);
    // Scroll the least amount that shows `descendant` (plus margin).
    void  ensureVisible(const View *descendant, float margin = 0, bool animated = false);
    void  revealFocus(const View *descendant) override { ensureVisible(descendant, 8); }

    float contentExtent() const override { return _contentH; }
    SizeF measureContent(float availW, float availH) override;
    void  layout() override;

protected:
    float scrollPixels(float dy) override;
    void  setOffset(float y) override;
    float offset() const override { return _offset; }

private:
    View *_content = nullptr;
    float _offset = 0, _contentH = 0;
};

// ── VirtualList ─────────────────────────────────────────────────────────────
// A list of `count()` rows of variable, lazily measured height. Only rows in
// (or just outside) the viewport exist as views; they are recycled per kind,
// and with setKeep the last few scrolled out stay built for scrolling back.
//
// Scroll position is an anchor (an item index and the pixel offset of the
// viewport top into it) rather than a pixel offset, so:
//   - rows above the viewport growing/shrinking/being measured never move
//     what you see;
//   - itemsInserted() above the anchor (older history prepended) keeps the
//     viewport on the same message;
//   - with stickToBottom, a list scrolled to the end stays pinned there as
//     rows are appended or grow (new messages, images loading).
// Unmeasured rows count as the adapter's estimate (or the running average)
// in the scrollbar only.
class VirtualList : public ScrollArea {
public:
    class Adapter {
    public:
        virtual ~Adapter()                          = default;
        virtual int                   count() const = 0;
        // Rows of different kinds are recycled separately (message, day divider…).
        virtual int                   kind(int index) const { return 0; }
        virtual std::unique_ptr<View> create(int kind)           = 0;
        // Fill `row` for `index`. Called when a row scrolls in and after
        // itemsChanged(); the row measures itself (at the list width).
        virtual void                  bind(View &row, int index) = 0;
        // Before a row goes back to the pool (drop image requests, …).
        virtual void                  unbind(View &row, int index) {}
        // Height guess for a row never measured; 0 = the running average.
        virtual float                 estimateHeight(int index) const { return 0; }
        // What item `index` is (non-zero), for keeping its row as built
        // (setKeep): a row scrolled out stays bound, hidden, and comes back
        // for the same key without bind() — until itemsChanged(), reset() or
        // removal says the item changed. 0: rebound every time.
        virtual uint64_t              key(int index) const { return 0; }
        // A kept row coming back instead of bind(): refresh what may have
        // moved on without a model change; false = bind() it after all.
        virtual bool                  reuse(View &row, int index) { return true; }
    };

    enum class ItemAlign : uint8_t { Start, Center, End, Nearest };

    explicit VirtualList(Adapter *adapter); // not owned
    ~VirtualList() override;

    // Keep the end in view when appending while scrolled to the end (chat).
    void setStickToBottom(bool on);
    // A list shorter than the viewport sits at the bottom (chat) or top.
    void setBottomAligned(bool on);
    void setOverscan(float px) { _overscan = px; }
    void setGap(float px); // vertical space between rows
    // Rows scrolled out kept bound (Adapter::key), at most `rows`, the least
    // recently shown dropped first; 0 (the default) keeps none.
    void setKeep(size_t rows) { _keep = rows; }

    // ── Model changes ───────────────────────────────────────────────────────
    void itemsInserted(int index, int n);
    void itemsRemoved(int index, int n);
    void itemsChanged(int index, int n); // rebind + re-measure
    void reset();                        // everything changed

    // ── Navigation ──────────────────────────────────────────────────────────
    // Animated jumps glide the last ~viewport of distance even from far away
    // (a far jump lands near the target first), honouring reduced motion.
    void scrollToItem(int index, ItemAlign a = ItemAlign::Start, bool animated = true);
    void scrollToBottom(bool animated = false);
    bool pinned() const { return _pinned; } // stuck to the bottom

    int   firstVisible() const; // -1 when empty
    int   lastVisible() const;
    View *viewFor(int index) const;       // the live row view, or null if not materialised
    int   indexOf(const View *row) const; // -1 if not a live row
    struct Anchor {
        int   index  = 0;
        float offset = 0; // viewport top, px below the item's top
    };
    Anchor anchor() const { return {_anchorIdx, _anchorOff}; }
    // Jumps (no animation) so the viewport top is a.offset px below item
    // a.index's top (negative: above it) — a saved position, or an item
    // placed a third down the viewport. Clamped at both ends as usual;
    // resolved by the next layout, so it may come before the first one.
    void   scrollToAnchor(Anchor a);
    int    liveCount() const { return int(_live.size()); }
    int    measuredCount() const { return _measured; }

    float contentExtent() const override;
    SizeF measureContent(float availW, float availH) override { return {0, 0}; }
    void  layout() override;

protected:
    float scrollPixels(float dy) override;
    void  setOffset(float y) override;
    float offset() const override;
    bool  tickCustom(double dtMs) override;

public:
    void scrollTo(float y, bool animated = false) override;

private:
    void placeAt(int index, ItemAlign a);

private:
    struct Live {
        int      index;
        int      kind;
        View    *view;
        bool     dirty;   // needs bind()
        bool     wasLive; // layout(): live at its start, at prevY/prevH
        uint64_t key;     // Adapter::key when bound
        float    prevY = 0, prevH = 0;
    };
    struct Pooled {
        int      kind;
        View    *view;
        uint64_t key; // still bound to this item (0: a spare)
    };
    struct Placed {
        int   index;
        float y, h;
    };
    struct KindCount {
        int    kind;
        size_t n;
    };

    float  heightOf(int i) const;
    float  measureItem(int i);
    float  offsetOf(int i) const;  // content y of item i's top (estimates above)
    int    indexAt(float y) const; // the item whose span contains content y
    float  knownHeight(int i, int *unknown) const;
    void   fenwickAdd(int i, float dh, int du);
    void   fenwickAppend(); // _h grew at its end
    void   rebuildFenwick() const;
    float  averageHeight() const;
    View  *acquire(int index, bool *fresh);
    void   release(size_t liveIdx);
    void   trimPool();
    void   dropKept(int index, int k);
    void   unkeepAll();
    size_t findLive(int index) const;
    bool   normalize();   // keep the anchor inside its item; returns false at the top edge
    bool   clampBottom(); // true when the content end reached the viewport bottom
    void   anchorFromBottom();
    void   setAnchor(int index, float off, bool pinned);

    Adapter           *_adapter;
    std::vector<float> _h; // >0 measured, <0 stale (|h| is a guess), 0 unknown
    // Offsets are prefix sums over two Fenwick trees: heights we know (measured,
    // stale, or the adapter's estimate) and a count of rows we know nothing
    // about, which count at the running average. A new measurement is an
    // O(log n) update and the average moving invalidates nothing, so the
    // scrollbar costs the same at 100 or 1,000,000 rows.
    struct Fen {
        float h = 0; // known heights
        int   u = 0; // rows known nothing about
    };
    mutable std::vector<Fen> _fen;
    mutable bool             _fenDirty = true;
    std::vector<Live>        _live; // sorted by index
    std::vector<Pooled>      _pool; // oldest first
    // layout()'s and trimPool()'s working memory, kept between calls.
    std::vector<Placed>      _placed;
    std::vector<RectF>       _fresh;
    std::vector<KindCount>   _spares;
    size_t                   _keep      = 0;
    int                      _anchorIdx = 0, _measured = 0;
    float     _anchorOff = 0, _measuredSum = 0, _width = -1, _overscan = 120, _gap = 0;
    float     _subpixel  = 0; // scroll remainder below one physical pixel
    int       _jumpIdx   = -1;
    ItemAlign _jumpAlign = ItemAlign::Start;
    bool      _pinned = false, _stick = false, _bottomAligned = false;
    bool      _contentDirty = true; // next layout damages the whole viewport
};

} // namespace ui
