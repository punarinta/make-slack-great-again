// VirtualList: anchor-based virtual scrolling. The invariant that makes chat
// work is that the scroll position is (anchor item, offset into it), and the
// viewport is rebuilt from the anchor outwards every layout, so nothing above
// the anchor (unmeasured estimates, rows growing, history prepended) can move
// what is on screen. Pixel offsets exist only for the scrollbar.
#include "ui/scroll.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float  kDefaultRowH = 48;
constexpr size_t kPoolPerKind = 8;  // spare row views kept per kind
constexpr float  kJumpTau     = 60; // ms, jump-to-item glide
constexpr size_t kNone        = size_t(-1);
} // namespace

VirtualList::VirtualList(Adapter *a) : _adapter(a) {
    setRole(Role::List);
    _h.assign(size_t(std::max(0, a->count())), 0.f);
}

VirtualList::~VirtualList() = default;

void VirtualList::setStickToBottom(bool on) {
    _stick = on;
    if (on && _live.empty())
        _pinned = true; // a fresh chat opens at its newest message
    invalidateLayout();
}

void VirtualList::setBottomAligned(bool on) {
    _bottomAligned = on;
    _contentDirty  = true;
    invalidateLayout();
}

void VirtualList::setGap(float px) {
    _gap          = px;
    _fenDirty     = true;
    _contentDirty = true;
    invalidateLayout();
}

// ── Heights and offsets ─────────────────────────────────────────────────────

float VirtualList::averageHeight() const {
    return _measured ? _measuredSum / float(_measured) : kDefaultRowH;
}

// Height as far as we know it; *unknown = 1 when we know nothing at all.
float VirtualList::knownHeight(int i, int *unknown) const {
    const float h = _h[size_t(i)];
    *unknown      = 0;
    if (h != 0)
        return std::abs(h); // < 0: measured at another width or before a change
    const float est = _adapter->estimateHeight(i);
    if (est > 0)
        return est;
    *unknown = 1;
    return 0;
}

float VirtualList::heightOf(int i) const {
    int         u;
    const float k = knownHeight(i, &u);
    return u ? averageHeight() : k;
}

void VirtualList::rebuildFenwick() const {
    const size_t n = _h.size();
    _fen.assign(n + 1, Fen{});
    for (size_t i = 1; i <= n; ++i) {
        int u;
        _fen[i].h += knownHeight(int(i - 1), &u);
        _fen[i].u += u;
        const size_t j = i + (i & (~i + 1));
        if (j <= n) {
            _fen[j].h += _fen[i].h;
            _fen[j].u += _fen[i].u;
        }
    }
    _fenDirty = false;
}

// Rows appended: their nodes are filled in from the ones already there
// (each covers (i - lowbit(i), i]), O(log n) apiece instead of a rebuild.
void VirtualList::fenwickAppend() {
    if (_fenDirty)
        return;
    const size_t n0 = _fen.size() - 1, n = _h.size();
    for (size_t i = n0 + 1; i <= n; ++i) {
        int          u;
        float        h   = knownHeight(int(i - 1), &u);
        const size_t low = i & (~i + 1);
        for (size_t j = i - 1; j > i - low; j -= j & (~j + 1)) {
            h += _fen[j].h;
            u += _fen[j].u;
        }
        _fen.push_back({h, u});
    }
}

void VirtualList::fenwickAdd(int i, float dh, int du) {
    if (_fenDirty)
        return; // rebuilt on the next query anyway
    const size_t n = _h.size();
    for (size_t k = size_t(i) + 1; k <= n; k += k & (~k + 1)) {
        _fen[k].h += dh;
        _fen[k].u += du;
    }
}

float VirtualList::offsetOf(int i) const {
    const int n = int(_h.size());
    i           = std::clamp(i, 0, n);
    if (_fenDirty)
        rebuildFenwick();
    float sh = 0;
    int   su = 0;
    for (size_t k = size_t(i); k > 0; k -= k & (~k + 1)) {
        sh += _fen[k].h;
        su += _fen[k].u;
    }
    return sh + float(su) * averageHeight() + float(i) * _gap;
}

int VirtualList::indexAt(float y) const {
    const size_t n = _h.size();
    if (n == 0)
        return 0;
    if (_fenDirty)
        rebuildFenwick();
    // Fenwick descent: the largest i with offsetOf(i) <= y.
    const float avg = averageHeight();
    size_t      pos = 0, step = 1;
    float       accH = 0;
    int         accU = 0;
    while (step * 2 <= n)
        step *= 2;
    for (; step > 0; step /= 2) {
        const size_t nxt = pos + step;
        if (nxt > n)
            continue;
        const float off = accH + _fen[nxt].h + float(accU + _fen[nxt].u) * avg + float(nxt) * _gap;
        if (off <= y) {
            pos = nxt;
            accH += _fen[nxt].h;
            accU += _fen[nxt].u;
        }
    }
    return int(std::min(pos, n - 1));
}

float VirtualList::contentExtent() const {
    const int n = int(_h.size());
    return n ? offsetOf(n) - _gap : 0;
}

float VirtualList::offset() const {
    if (_h.empty())
        return 0;
    if (_pinned)
        return std::max(0.f, contentExtent() - height());
    return offsetOf(_anchorIdx) + _anchorOff;
}

// ── Live rows ───────────────────────────────────────────────────────────────

size_t VirtualList::findLive(int index) const {
    auto it = std::lower_bound(_live.begin(), _live.end(), index, [](const Live &l, int i) {
        return l.index < i;
    });
    return it != _live.end() && it->index == index ? size_t(it - _live.begin()) : kNone;
}

View *VirtualList::viewFor(int index) const {
    const size_t li = findLive(index);
    return li == kNone ? nullptr : _live[li].view;
}

int VirtualList::indexOf(const View *row) const {
    for (const Live &l : _live)
        if (l.view == row)
            return l.index;
    return -1;
}

View *VirtualList::acquire(int index, bool *fresh) {
    const int      kind = _adapter->kind(index);
    const uint64_t key  = _keep ? _adapter->key(index) : 0;
    // The row kept for this item, else a spare (the newest), else the
    // least recently kept row of the kind.
    size_t         pick = kNone;
    if (key)
        for (size_t i = 0; i < _pool.size() && pick == kNone; ++i)
            if (_pool[i].kind == kind && _pool[i].key == key)
                pick = i;
    const bool kept = pick != kNone;
    for (size_t i = _pool.size(); pick == kNone && i-- > 0;)
        if (_pool[i].kind == kind && !_pool[i].key)
            pick = i;
    for (size_t i = 0; pick == kNone && i < _pool.size(); ++i)
        if (_pool[i].kind == kind)
            pick = i;
    View *v = nullptr;
    if (pick != kNone) {
        v = _pool[pick].view;
        _pool.erase(_pool.begin() + ptrdiff_t(pick));
        v->setVisible(true);
    } else {
        v = adopt(_adapter->create(kind));
    }
    auto it = std::lower_bound(_live.begin(), _live.end(), index, [](const Live &l, int i) {
        return l.index < i;
    });
    _live.insert(it, Live{index, kind, v, false, false, key});
    if (!kept || !_adapter->reuse(*v, index))
        _adapter->bind(*v, index);
    *fresh = true;
    return v;
}

void VirtualList::release(size_t li) {
    const Live l = _live[li];
    _live.erase(_live.begin() + ptrdiff_t(li));
    _adapter->unbind(*l.view, l.index);
    l.view->setVisible(false);
    _pool.push_back({l.kind, l.view, l.dirty ? 0 : l.key});
    trimPool();
}

// Bounded memory after a tall viewport or a long scroll: at most _keep kept
// rows (the least recently shown stop being kept), and kPoolPerKind spares
// per kind besides (the oldest are destroyed).
void VirtualList::trimPool() {
    size_t kept = 0;
    for (const Pooled &p : _pool)
        kept += p.key != 0;
    for (size_t i = 0; kept > _keep && i < _pool.size(); ++i)
        if (_pool[i].key) {
            _pool[i].key = 0;
            --kept;
        }
    // Newest first, counting the spares of each kind seen so far (a list
    // has a few kinds): one pass, not a count of the newer ones per entry.
    _spares.clear();
    for (size_t i = _pool.size(); i-- > 0;) {
        if (_pool[i].key)
            continue;
        size_t *seen = nullptr;
        for (KindCount &k : _spares)
            if (k.kind == _pool[i].kind)
                seen = &k.n;
        if (!seen) {
            _spares.push_back({_pool[i].kind, 0});
            seen = &_spares.back().n;
        }
        if ((*seen)++ >= kPoolPerKind) {
            View *v = _pool[i].view;
            _pool.erase(_pool.begin() + ptrdiff_t(i));
            remove(v);
        }
    }
}

// Items [index, index + k) changed: their kept rows (if any) are stale.
void VirtualList::dropKept(int index, int k) {
    bool any = false;
    for (const Pooled &p : _pool)
        any = any || p.key;
    if (!any)
        return;
    const int n = int(_h.size());
    for (int i = std::max(0, index); i < std::min(n, index + k); ++i)
        if (const uint64_t key = _adapter->key(i))
            for (Pooled &p : _pool)
                if (p.key == key)
                    p.key = 0;
    trimPool();
}

void VirtualList::unkeepAll() {
    for (Pooled &p : _pool)
        p.key = 0;
    trimPool();
}

float VirtualList::measureItem(int i) {
    size_t li = findLive(i);
    View  *v;
    // A changed item may have become another kind of row (a message that
    // stopped continuing a group): its old view cannot show it.
    if (li != kNone && _live[li].dirty && _live[li].kind != _adapter->kind(i)) {
        release(li);
        li = kNone;
    }
    if (li == kNone) {
        bool fresh = false;
        v          = acquire(i, &fresh);
    } else {
        v = _live[li].view;
        if (_live[li].dirty) {
            _live[li].dirty = false;
            _live[li].key   = _keep ? _adapter->key(i) : 0;
            _adapter->bind(*v, i);
        }
    }
    float       h   = std::max(0.001f, v->measure(_width, kInf).h);
    const float old = _h[size_t(i)];
    if (old == h)
        return h;
    int         u0;
    const float k0 = knownHeight(i, &u0);
    if (old > 0) {
        _measuredSum += h - old;
    } else {
        ++_measured;
        _measuredSum += h;
    }
    _h[size_t(i)] = h;
    fenwickAdd(i, h - k0, -u0);
    return h;
}

// ── Anchor maintenance ──────────────────────────────────────────────────────

bool VirtualList::normalize() {
    const int n = int(_h.size());
    if (n == 0) {
        _anchorIdx = 0;
        _anchorOff = 0;
        return false;
    }
    _anchorIdx = std::clamp(_anchorIdx, 0, n - 1);
    while (_anchorOff < 0) {
        if (_anchorIdx == 0) {
            _anchorOff = 0;
            return false;
        }
        --_anchorIdx;
        _anchorOff += measureItem(_anchorIdx) + _gap;
    }
    for (;;) {
        const float h = measureItem(_anchorIdx) + _gap;
        if (_anchorOff < h || _anchorIdx == n - 1)
            break;
        _anchorOff -= h;
        ++_anchorIdx;
    }
    return true;
}

bool VirtualList::clampBottom() {
    const int   n = int(_h.size());
    const float H = height();
    float       y = -_anchorOff;
    int         i = _anchorIdx;
    while (i < n && y < H) {
        y += measureItem(i) + (i < n - 1 ? _gap : 0);
        ++i;
    }
    return i == n && y <= H + 0.01f;
}

void VirtualList::anchorFromBottom() {
    const int n = int(_h.size());
    float     y = height();
    for (int i = n - 1; i >= 0; --i) {
        y -= measureItem(i);
        if (y <= 0) {
            _anchorIdx = i;
            _anchorOff = -y;
            return;
        }
        if (i > 0)
            y -= _gap;
    }
    _anchorIdx = 0; // shorter than the viewport
    _anchorOff = 0;
}

void VirtualList::setAnchor(int index, float off, bool pin) {
    _anchorIdx = index;
    _anchorOff = off;
    _pinned    = pin;
    if (!pin) {
        normalize();
        if (clampBottom()) {
            anchorFromBottom();
            _pinned = _stick;
        }
    }
    _contentDirty = true;
    invalidateLayout();
    moved(0, 1, false); // notify + refresh the bar
}

// ── Layout ──────────────────────────────────────────────────────────────────

void VirtualList::layout() {
    const float W = width(), H = height();
    const int   n = _adapter->count();
    if (size_t(n) != _h.size()) { // the model changed without telling us
        _h.assign(size_t(n), 0.f);
        _measured    = 0;
        _measuredSum = 0;
        _fenDirty    = true;
        while (!_live.empty())
            release(_live.size() - 1);
        unkeepAll();
        _contentDirty = true;
    }
    if (W != _width) {
        _width = W;
        for (float &h : _h)
            if (h > 0)
                h = -h;
        _measured     = 0; // |h| stays as the estimate: the Fenwick sums hold
        _measuredSum  = 0;
        _contentDirty = true;
    }
    if (n == 0) {
        while (!_live.empty())
            release(_live.size() - 1);
        _anchorIdx = 0;
        _anchorOff = 0;
        update();
        return;
    }

    // 1. Resolve the anchor.
    if (_pinned) {
        anchorFromBottom();
    } else {
        normalize();
        if (clampBottom()) {
            anchorFromBottom();
            if (_stick)
                _pinned = true;
        }
    }

    // Where every live row was, to tell a pure scroll (blit) from a change.
    for (Live &l : _live) {
        l.wasLive = true;
        l.prevY   = l.view->frame().y;
        l.prevH   = l.view->frame().h;
    }

    // 2. Place from the anchor down, then up.
    float top = -_anchorOff;
    if (_anchorIdx == 0 && _anchorOff == 0 && _bottomAligned) {
        float sum = 0;
        for (int i = 0; i < n && sum < H; ++i)
            sum += measureItem(i) + (i < n - 1 ? _gap : 0);
        if (sum < H)
            top = H - sum;
    }
    std::vector<Placed> &placed = _placed;
    placed.clear();
    float y = top;
    for (int i = _anchorIdx; i < n && y < H + _overscan; ++i) {
        const float h = measureItem(i);
        placed.push_back({i, y, h});
        y += h + _gap;
    }
    y = top;
    for (int i = _anchorIdx - 1; i >= 0 && y > -_overscan; --i) {
        const float h = measureItem(i);
        y -= h + _gap;
        placed.push_back({i, y, h});
    }
    int lo = n, hi = -1;
    for (const Placed &p : placed) {
        lo = std::min(lo, p.index);
        hi = std::max(hi, p.index);
    }

    // 3. Recycle rows that left the range (measuring may have made extras).
    for (size_t li = _live.size(); li-- > 0;)
        if (_live[li].index < lo || _live[li].index > hi)
            release(li);

    // 4. Position (quietly: damage is decided below).
    bool                uniform = !_contentDirty, haveShift = false;
    float               shift = 0;
    std::vector<RectF> &fresh = _fresh;
    fresh.clear();
    for (const Placed &p : placed) {
        const size_t li = findLive(p.index);
        if (li == kNone)
            continue; // cannot happen: measureItem materialised it
        Live       &l  = _live[li];
        View       *v  = l.view;
        // Every row top on the pixel grid; scrollPixels() moves by whole
        // physical pixels, so a scroll shifts all rows by the same amount.
        const float sy = snapPx(p.y);
        if (l.wasLive) { // the same row view showing the same item as before
            const float d = sy - l.prevY;
            if (!haveShift) {
                shift     = d;
                haveShift = true;
            } else if (std::abs(d - shift) > 0.01f) {
                uniform = false;
            }
            if (std::abs(l.prevH - p.h) > 0.01f)
                uniform = false;
        } else {
            fresh.push_back({0, sy, W, p.h});
        }
        v->setFrameQuiet({0, sy, W, p.h});
    }
    if (!haveShift)
        uniform = false;
    // Rows that appeared must come in only through the exposed strip.
    if (uniform)
        for (const RectF &r : fresh) {
            const float s0 = shift < 0 ? H + shift : 0, s1 = shift < 0 ? H : shift;
            const float a = std::max(r.y, 0.f), b = std::min(r.y + r.h, H);
            if (b > a && (a < s0 - 0.01f || b > s1 + 0.01f))
                uniform = false;
        }
    // A hover-only overlay (the dismiss ×) leaves a trail if we just shift
    // the old pixels for it, so force a real repaint instead.
    if (uniform)
        for (const Live &l : _live)
            if (l.view->hasLiveOverlay()) {
                uniform = false;
                break;
            }

    // 5. Damage: a blit and the strip, or the whole viewport.
    if (!uniform) {
        update();
    } else if (shift != 0) {
        if (Window *w = window()) {
            if (opaqueBackground())
                w->scrollBlit(this, int(std::lround(-shift * w->scale())));
            else
                update(); // transparent: what shows through does not move
            update(barZone());
            w->refreshHover();
        }
    }
    _contentDirty = false;
}

// ── Scrolling ───────────────────────────────────────────────────────────────

float VirtualList::scrollPixels(float dy) {
    const int n = int(_h.size());
    if (n == 0 || dy == 0)
        return 0;
    // Whole physical pixels only (the rest carries over): a blit needs an
    // integer shift, and sub-pixel touchpad deltas still add up.
    const float s    = windowScale();
    const float want = dy + _subpixel;
    dy               = std::trunc(want * s) / s;
    _subpixel        = want - dy;
    if (dy == 0)
        return want;
    if (_pinned) {
        if (dy >= 0)
            return 0;
        _pinned = false;
        anchorFromBottom();
    }
    const int   oi = _anchorIdx;
    const float oo = _anchorOff;
    _anchorOff += dy;
    float applied = dy;
    if (!normalize() && oi == _anchorIdx && std::abs(oo - _anchorOff) < 0.01f)
        applied = 0; // already at the top
    if (dy > 0 && clampBottom()) {
        anchorFromBottom();
        if (_stick)
            _pinned = true;
        if (oi == _anchorIdx && std::abs(oo - _anchorOff) < 0.01f)
            applied = 0;
    }
    if (applied != 0) {
        invalidateLayout();
        moved(0, 1, false); // the blit is decided in layout() from real row moves
    }
    return applied;
}

void VirtualList::setOffset(float y) {
    const int n = int(_h.size());
    if (n == 0)
        return;
    const float ext = contentExtent(), H = height();
    y = std::clamp(y, 0.f, std::max(0.f, ext - H));
    if (y >= ext - H - 0.5f && ext > H) {
        _pinned = _stick;
        anchorFromBottom();
        _contentDirty = true;
        invalidateLayout();
        moved(0, 1, false);
        return;
    }
    const int i = indexAt(y);
    setAnchor(i, y - offsetOf(i), false);
}

void VirtualList::scrollTo(float y, bool animated) {
    // Far glides would measure every row on the way: jump instead.
    if (!animated || app()->reducedMotion() || std::abs(y - offset()) > 2 * height()) {
        stopScrolling();
        setOffset(y);
        return;
    }
    ScrollArea::scrollTo(y, true);
}

void VirtualList::placeAt(int i, ItemAlign a) {
    const float H = height();
    const float h = measureItem(i);
    switch (a) {
    case ItemAlign::Start:
        setAnchor(i, 0, false);
        break;
    case ItemAlign::Center:
        setAnchor(i, std::floor((h - H) / 2), false);
        break;
    case ItemAlign::End:
        setAnchor(i, h - H, false);
        break;
    case ItemAlign::Nearest: {
        if (View *v = viewFor(i)) {
            const float y = v->frame().y;
            if (y >= 0 && y + h <= H)
                return;
            setAnchor(i, y < 0 ? 0 : h - H, false);
        } else {
            setAnchor(i, offsetOf(i) < offset() ? 0 : h - H, false);
        }
        break;
    }
    }
}

void VirtualList::scrollToAnchor(Anchor a) {
    const int n = int(_h.size());
    if (n == 0)
        return;
    stopScrolling();
    _jumpIdx      = -1;
    // Resolved by the next layout (at the list's width), like any anchor.
    _anchorIdx    = std::clamp(a.index, 0, n - 1);
    _anchorOff    = a.offset;
    _pinned       = false;
    _contentDirty = true;
    invalidateLayout();
    moved(0, 1, false); // notify + refresh the bar
}

void VirtualList::scrollToItem(int index, ItemAlign a, bool animated) {
    const int n = int(_h.size());
    if (n == 0)
        return;
    index = std::clamp(index, 0, n - 1);
    stopScrolling();
    _jumpIdx = -1;
    if (a == ItemAlign::Nearest) {
        if (View *v = viewFor(index)) {
            const float y = v->frame().y;
            if (y >= 0 && y + v->frame().h <= height())
                return;
            a = y < 0 ? ItemAlign::Start : ItemAlign::End;
        } else {
            a = offsetOf(index) < offset() ? ItemAlign::Start : ItemAlign::End;
        }
    }
    if (!animated || app()->reducedMotion() || height() <= 0) {
        placeAt(index, a);
        return;
    }
    // Land ~1.5 viewports before a far target, then glide the rest.
    const float target = offsetOf(index), cur = offset(), H = height();
    if (std::abs(target - cur) > 2 * H) {
        const float land = target + (target > cur ? -1.5f * H : 1.5f * H);
        const float y    = std::max(0.f, land);
        const int   i    = indexAt(y);
        setAnchor(i, y - offsetOf(i), false);
    }
    _jumpIdx   = index;
    _jumpAlign = a;
    startCustomAnimation();
}

bool VirtualList::tickCustom(double dtMs) {
    if (_jumpIdx < 0 || _jumpIdx >= int(_h.size()))
        return false;
    const float H = height();
    float       d;
    if (View *v = viewFor(_jumpIdx)) { // exact once the target is on screen
        const float h    = v->frame().h;
        const float want = _jumpAlign == ItemAlign::Center ? std::floor((H - h) / 2)
                           : _jumpAlign == ItemAlign::End  ? H - h
                                                           : 0;
        d                = v->frame().y - want;
    } else {
        d = offsetOf(_jumpIdx) - offset();
    }
    if (std::abs(d) < 0.5f) {
        placeAt(_jumpIdx, _jumpAlign);
        _jumpIdx = -1;
        return false;
    }
    float step = d * (1 - std::exp(-float(dtMs) / kJumpTau));
    if (std::abs(step) < 0.5f)
        step = d;
    if (scrollPixels(step) == 0) {
        _jumpIdx = -1;
        return false;
    }
    return true;
}

void VirtualList::scrollToBottom(bool animated) {
    const int n = int(_h.size());
    if (n == 0)
        return;
    if (animated && !app()->reducedMotion() &&
        contentExtent() - offset() - height() < 2 * height()) {
        scrollToItem(n - 1, ItemAlign::End, true);
        return;
    }
    stopScrolling();
    _pinned = _stick;
    anchorFromBottom();
    _contentDirty = true;
    invalidateLayout();
    moved(0, 1, false);
}

// ── Model changes ───────────────────────────────────────────────────────────

void VirtualList::itemsInserted(int index, int k) {
    if (k <= 0)
        return;
    const int n = int(_h.size());
    index       = std::clamp(index, 0, n);
    _h.insert(_h.begin() + index, size_t(k), 0.f);
    if (index == n)
        fenwickAppend(); // new messages: the common case
    else
        _fenDirty = true;
    dropKept(index, k); // an item back under a key a removed one had
    for (Live &l : _live)
        if (l.index >= index)
            l.index += k;
    // Inserted above (or at) the anchor: the anchor item moved down by k, so
    // the viewport keeps showing the same content. Pinned lists re-pin.
    if (!_pinned && n > 0 && index <= _anchorIdx)
        _anchorIdx += k;
    if (_jumpIdx >= index)
        _jumpIdx += k;
    invalidateLayout();
}

void VirtualList::itemsRemoved(int index, int k) {
    const int n = int(_h.size());
    if (k <= 0 || index >= n)
        return;
    k = std::min(k, n - index);
    for (size_t li = _live.size(); li-- > 0;)
        if (_live[li].index >= index && _live[li].index < index + k) {
            _live[li].view->update();
            _live[li].dirty = true; // gone: not kept
            release(li);
        }
    for (Live &l : _live)
        if (l.index >= index + k)
            l.index -= k;
    for (int i = index; i < index + k; ++i)
        if (_h[size_t(i)] > 0) {
            --_measured;
            _measuredSum -= _h[size_t(i)];
        }
    _h.erase(_h.begin() + index, _h.begin() + index + k);
    _fenDirty = true;
    if (_anchorIdx >= index + k)
        _anchorIdx -= k;
    else if (_anchorIdx >= index) {
        _anchorIdx = index;
        _anchorOff = 0;
    }
    _anchorIdx = std::clamp(_anchorIdx, 0, std::max(0, int(_h.size()) - 1));
    if (_jumpIdx >= index + k)
        _jumpIdx -= k;
    else if (_jumpIdx >= index)
        _jumpIdx = -1;
    invalidateLayout();
}

void VirtualList::itemsChanged(int index, int k) {
    const int n = int(_h.size());
    for (int i = std::max(0, index); i < std::min(n, index + k); ++i) {
        if (_h[size_t(i)] > 0) {
            --_measured;
            _measuredSum -= _h[size_t(i)];
            _h[size_t(i)] = -_h[size_t(i)];
        }
        const size_t li = findLive(i);
        if (li != kNone)
            _live[li].dirty = true;
    }
    if (size_t(k) > 2 * _keep)
        unkeepAll(); // a change to every row: no key to look up
    else
        dropKept(index, k);
    // Stale heights keep their magnitude as the estimate: offsets unchanged.
    invalidateLayout();
}

void VirtualList::reset() {
    while (!_live.empty())
        release(_live.size() - 1);
    unkeepAll();
    _h.assign(size_t(std::max(0, _adapter->count())), 0.f);
    _measured     = 0;
    _measuredSum  = 0;
    _fenDirty     = true;
    _anchorIdx    = 0;
    _anchorOff    = 0;
    _pinned       = _stick;
    _jumpIdx      = -1;
    _contentDirty = true;
    stopScrolling();
    invalidateLayout();
}

int VirtualList::firstVisible() const {
    for (const Live &l : _live)
        if (l.view->frame().y + l.view->frame().h > 0 && l.view->frame().y < height())
            return l.index;
    return -1;
}

int VirtualList::lastVisible() const {
    for (size_t i = _live.size(); i-- > 0;) {
        const RectF &f = _live[i].view->frame();
        if (f.y < height() && f.y + f.h > 0)
            return _live[i].index;
    }
    return -1;
}

} // namespace ui
