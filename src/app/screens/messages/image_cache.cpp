#include "app/screens/messages/image_cache.h"

#include "app/model/image_size.h"
#include "app/screens/common/remote_images.h"
#include "base/file.h"
#include "base/thread.h"
#include "base/time.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace screens {

namespace {

// naturalSize's memo stays this small (it is cleared when full).
constexpr size_t  kMaxSizeMemos  = 4096;
// How long "a URL not on disk yet" is believed without a new look.
constexpr int64_t kMissingMemoMs = 5000;
// Failed entries kept (each answers failed() and stops a retry storm); the
// oldest beyond go, so a long session's dead links don't pile up.
constexpr size_t  kMaxFailed     = 256;
// A picture delivered to waiting views counts as in use this long, until
// they paint and take it (a frame, give or take a busy loop).
constexpr int64_t kFreshMs       = 1000;

} // namespace

ImageCache::Key ImageCache::keyOf(const Ref &r, bool animated) {
    return {
        r.path,
        r.width,
        r.height,
        r.shape == Shape::Rounded ? int(r.radius * 4) : 0,
        uint8_t(r.shape),
        animated
    };
}

size_t ImageCache::KeyHash::operator()(const Key &k) const {
    size_t h = std::hash<std::string_view>{}(k.path);
    for (const uint64_t v :
         {uint64_t(uint32_t(k.width)) << 32 | uint32_t(k.height),
          uint64_t(uint32_t(k.radius4)) << 16 | uint64_t(k.shape) << 1 | uint64_t(k.animated)})
        h ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

struct ImageCache::Entry {
    std::string             path; // the Key's view points here
    int                     width = 0, height = 0;
    float                   radius = 0;
    Shape                   shape  = Shape::Square;
    Bitmap                  still;
    Frames                  frames;
    std::vector<ui::View *> waiters;
    size_t                  bytes = 0;
    uint64_t                id    = 0;
    int64_t                 fresh = 0; // ms delivered to waiters, until one asks again
    Entry                  *prev = nullptr, *next = nullptr; // the LRU list (Ready only)
    bool                    retry = false; // a failed download: ask again once RemoteImages would
    enum : uint8_t { Loading, Ready, Failed } state = Loading;
    bool animated                                   = false;
    bool inLru                                      = false;

    Key     key() const { return keyOf(Ref{path, width, height, shape, radius}, animated); }
    Request request() const { return {path, width, height, shape, radius}; }
    // Whether something besides the cache keeps the pixels: a view painting
    // them (CachedImage holds them between lookups), or one they were just
    // delivered to and that hasn't taken them yet.
    bool    inUse(int64_t now) const {
        return (animated ? frames.use_count() : still.use_count()) > 1 ||
               (fresh && now - fresh < kFreshMs);
    }
};

// ── Worker ──────────────────────────────────────────────────────────────────

struct ImageCache::Impl {
    struct Result {
        std::vector<gfx::AnimFrame> frames;
        int                         naturalW = 0, naturalH = 0;
        bool                        ok = false, sized = false;
    };
    // One thread decodes, in order (urgent ones first); it ends when idle.
    base::WorkerPool worker{{.maxWorkers = 1, .maxParked = 1}};
    plat::App       *app      = nullptr;
    ImageCache      *owner    = nullptr; // UI thread only; null once destroyed
    size_t           inflight = 0;       // UI thread: queued, decoding or posted

    static void work(const Request &r, bool animated, Result &out) {
        std::string data;
        if (!file::readAll(r.path, &data))
            return;
        // naturalSize's answer for it, while the file is at hand.
        if (int32_t nw = 0, nh = 0; model::imageSize(r.path, &nw, &nh)) {
            out.sized    = true;
            out.naturalW = nw;
            out.naturalH = nh;
        }
        auto &frames = out.frames;
        bool  ok     = false;
        if (animated) {
            // Each frame is shrunk as it is decoded, under the frame budget.
            gfx::AnimOptions o;
            o.width  = r.width;
            o.height = r.height;
            ok       = gfx::decodeAnimation(data, &frames, o) && !frames.empty();
        } else {
            gfx::AnimFrame f;
            ok = gfx::decodeImage(data, &f.frame);
            if (ok)
                frames.push_back(std::move(f));
        }
        // An SVG (the Claude Code teammates' glyph tiles): rendered to cover
        // the requested size, as the shell's avatars are.
        if (gfx::AnimFrame f; !ok && gfx::renderSvgCover(data, r.width, r.height, &f.frame)) {
            ok = true;
            frames.clear();
            frames.push_back(std::move(f));
        }
        data = {};
        if (!ok) {
            frames.clear();
            return;
        }
        for (gfx::AnimFrame &f : frames) {
            if (r.width > 0 && r.height > 0 &&
                (r.width != f.frame.width() || r.height != f.frame.height()))
                f.frame = gfx::coverResize(f.frame.view(), r.width, r.height);
            if (r.shape == Shape::Circle)
                gfx::maskRoundedRect(f.frame, 1e9f);
            else if (r.shape == Shape::Rounded)
                gfx::maskRoundedRect(f.frame, r.radius);
        }
        out.ok = true;
    }

    // Worker thread.
    static void run(const std::shared_ptr<Impl> &self, uint64_t id, Request &r, bool animated) {
        auto res = std::make_shared<Result>();
        work(r, animated, *res);
        self->app->post([self, id, path = std::move(r.path), res] {
            ImageCache *owner = self->owner;
            if (!owner)
                return;
            if (res->sized) { // under the path asked for (a URL, not its local copy)
                const auto it = owner->_loading.find(id);
                owner->noteSize(
                    it != owner->_loading.end() ? it->second->path : path,
                    res->naturalW,
                    res->naturalH,
                    true
                );
            }
            owner->deliver(id, std::move(res->frames), res->ok);
        });
    }
};

ImageCache::ImageCache(plat::App &app, size_t budget)
    : _app(app), _impl(std::make_shared<Impl>()), _budget(budget) {
    _impl->app   = &app;
    _impl->owner = this;
}

ImageCache::~ImageCache() {
    _impl->owner = nullptr; // results still in the post queue are dropped
    _impl->worker.stop();   // and queued decodes
}

void ImageCache::lruUnlink(Entry &e) {
    if (!e.inLru)
        return;
    if (e.prev)
        e.prev->next = e.next;
    else
        _lruHead = e.next;
    if (e.next)
        e.next->prev = e.prev;
    else
        _lruTail = e.prev;
    e.prev = e.next = nullptr;
    e.inLru         = false;
}

void ImageCache::lruPushFront(Entry &e) {
    e.prev = nullptr;
    e.next = _lruHead;
    if (_lruHead)
        _lruHead->prev = &e;
    _lruHead = &e;
    if (!_lruTail)
        _lruTail = &e;
    e.inLru = true;
}

ImageCache::Entry *ImageCache::lookup(const Ref &r, bool animated, ui::View *waiter) {
    if (r.path.empty())
        return nullptr;
    if (const auto it = _entries.find(keyOf(r, animated)); it != _entries.end()) {
        Entry &e = *it->second;
        // Asked again: a view holds it from here, or doesn't.
        e.fresh  = 0;
        if (e.inLru && _lruHead != &e) { // now the most recently used
            lruUnlink(e);
            lruPushFront(e);
        }
        // Its cool-down is RemoteImages' (which a sign-in also ends).
        if (e.state == Entry::Failed && e.retry && _remote && !_remote->failedRecently(e.path)) {
            e.state = Entry::Loading;
            e.retry = false;
            load(e);
        }
        if (e.state == Entry::Loading && waiter &&
            std::find(e.waiters.begin(), e.waiters.end(), waiter) == e.waiters.end()) {
            e.waiters.push_back(waiter);
            _waited.insert(&e);
        }
        return &e;
    }
    auto e      = std::make_shared<Entry>();
    e->path     = std::string(r.path);
    e->width    = r.width;
    e->height   = r.height;
    e->shape    = r.shape;
    e->radius   = r.radius;
    e->id       = _nextId++;
    e->animated = animated;
    if (waiter) {
        e->waiters.push_back(waiter);
        _waited.insert(e.get());
    }
    Entry *raw = e.get();
    _entries.emplace(raw->key(), std::move(e));
    load(*raw);
    return raw;
}

void ImageCache::load(Entry &e) {
    ++_impl->inflight; // until deliver(), whichever way the bytes come
    _loading[e.id]    = &e;
    const uint64_t id = e.id;
    const bool     an = e.animated;
    if (!RemoteImages::isRemote(e.path)) {
        decode(id, e.request(), an);
        return;
    }
    if (!_remote) {
        _app.post([impl = _impl, id] {
            if (impl->owner)
                impl->owner->deliver(id, {}, false);
        });
        return;
    }
    if (std::string local = _remote->cachedPath(e.path); !local.empty()) {
        Request lr = e.request();
        lr.path    = std::move(local);
        decode(id, std::move(lr), an);
        return;
    }
    _remote->fetch(e.path, [impl = _impl, id, r = e.request(), an](const std::string &path) {
        if (!impl->owner)
            return;
        // What naturalSize remembered about it not being here is over.
        impl->owner->_sizes.erase(r.path);
        if (path.empty()) {
            impl->owner->deliver(id, {}, false, true);
            return;
        }
        Request lr = r;
        lr.path    = path;
        impl->owner->decode(id, std::move(lr), an);
    });
}

void ImageCache::decode(uint64_t id, Request r, bool animated, bool urgent) {
    _impl->worker.post(
        [impl = _impl, id, r = std::move(r), animated]() mutable {
            Impl::run(impl, id, r, animated);
        },
        urgent
    );
}

void ImageCache::decodeOnce(Request r, std::function<void(gfx::Bitmap)> done, bool urgent) {
    ++_impl->inflight; // until deliver()
    const uint64_t id = _nextId++;
    _once.emplace(id, std::move(done));
    decode(id, std::move(r), false, urgent);
}

ImageCache::Bitmap ImageCache::get(const Ref &r, ui::View *waiter, Handle *held) {
    Entry *e = lookup(r, false, waiter);
    if (!e || e->state != Entry::Ready)
        return nullptr;
    if (held)
        held->_e = _entries.find(e->key())->second;
    return e->still;
}

ImageCache::Frames ImageCache::frames(const Ref &r, ui::View *waiter, Handle *held) {
    Entry *e = lookup(r, true, waiter);
    if (!e || e->state != Entry::Ready)
        return nullptr;
    if (held)
        held->_e = _entries.find(e->key())->second;
    return e->frames;
}

bool ImageCache::touch(const Handle &h) {
    const std::shared_ptr<Entry> e = h._e.lock();
    if (!e || !e->inLru)
        return false;
    if (_lruHead != e.get()) {
        lruUnlink(*e);
        lruPushFront(*e);
    }
    return true;
}

bool ImageCache::failed(const Ref &r) const {
    for (const bool animated : {false, true})
        if (const auto it = _entries.find(keyOf(r, animated));
            it != _entries.end() && it->second->state == Entry::Failed)
            return true;
    return false;
}

void ImageCache::forget(ui::View *waiter) {
    // Only loading entries have waiters.
    for (auto it = _waited.begin(); it != _waited.end();) {
        auto &ws = (*it)->waiters;
        ws.erase(std::remove(ws.begin(), ws.end(), waiter), ws.end());
        it = ws.empty() ? _waited.erase(it) : std::next(it);
    }
}

void ImageCache::deliver(uint64_t id, std::vector<gfx::AnimFrame> frames, bool ok, bool transient) {
    --_impl->inflight;
    if (const auto once = _once.find(id); once != _once.end()) {
        auto done = std::move(once->second);
        _once.erase(once);
        done(ok && !frames.empty() ? std::move(frames.front().frame) : gfx::Bitmap());
        return;
    }
    const auto it = _loading.find(id);
    if (it == _loading.end())
        return;
    Entry *e = it->second;
    _loading.erase(it);
    if (ok) {
        e->state = Entry::Ready;
        e->bytes = 0;
        for (const gfx::AnimFrame &f : frames)
            e->bytes += size_t(f.frame.width()) * size_t(f.frame.height()) * 4;
        if (e->animated) {
            e->frames = std::make_shared<const std::vector<gfx::AnimFrame>>(std::move(frames));
        } else {
            e->still = std::make_shared<const gfx::Bitmap>(std::move(frames.front().frame));
        }
        _bytes += e->bytes;
        lruPushFront(*e);
        if (!e->waiters.empty())
            e->fresh = base::monotonicMs();
    } else {
        e->state = Entry::Failed;
        e->retry = transient; // the download failed, not the decode: ask again later
        _failed.push_back(_entries.find(e->key())->second);
    }
    std::vector<ui::View *> waiters = std::move(e->waiters);
    e->waiters.clear();
    _waited.erase(e);
    for (ui::View *v : waiters)
        v->update();
    evict();
}

void ImageCache::evict() {
    // Pictures in use stay whatever goes, so they don't count against the
    // budget (one bigger than all of it neither goes nor pushes the rest
    // out); of the others, the least recently used goes first. A walk over
    // the ready entries, only while over budget.
    if (_bytes > _budget) {
        const int64_t now  = base::monotonicMs();
        size_t        held = 0;
        for (const Entry *e = _lruHead; e; e = e->next)
            if (e->inUse(now))
                held += e->bytes;
        for (Entry *e = _lruTail; e && _bytes - held > _budget;) {
            Entry *const prev = e->prev;
            if (!e->inUse(now)) {
                lruUnlink(*e);
                _bytes -= e->bytes;
                _entries.erase(e->key()); // destroys it; held handles expire
            }
            e = prev;
        }
    }
    // Failed ones, oldest first (one asked for again since is not failed now).
    if (_failed.size() <= kMaxFailed)
        return;
    const size_t drop = _failed.size() - kMaxFailed;
    for (size_t i = 0; i < drop; ++i)
        if (const std::shared_ptr<Entry> e = _failed[i].lock(); e && e->state == Entry::Failed)
            _entries.erase(e->key());
    _failed.erase(_failed.begin(), _failed.begin() + ptrdiff_t(drop));
}

void ImageCache::setBudget(size_t b) {
    _budget = b;
    evict();
}

size_t ImageCache::cachedBytes() const {
    const int64_t now = base::monotonicMs();
    size_t        n   = 0;
    for (const Entry *e = _lruHead; e; e = e->next)
        if (!e->inUse(now))
            n += e->bytes;
    return n;
}

size_t ImageCache::entryCount() const {
    return _entries.size();
}
size_t ImageCache::pending() const {
    return _impl->inflight;
}

void ImageCache::noteSize(const std::string &path, int w, int h, bool ok) {
    if (_sizes.size() >= kMaxSizeMemos && !_sizes.count(path))
        _sizes.clear(); // a bound, not a policy: an answer is cheap to find again
    _sizes[path] = {w, h, ok, false, 0};
}

bool ImageCache::naturalSize(const std::string &path, int *w, int *h) {
    *w = *h = 0;
    if (const auto it = _sizes.find(path); it != _sizes.end()) {
        const SizeMemo &m = it->second;
        if (!m.missing) {
            *w = m.w;
            *h = m.h;
            return m.ok;
        }
        if (base::monotonicMs() - m.at < kMissingMemoMs)
            return false;
        _sizes.erase(it); // look again
    }
    ++_sizeProbes;
    std::string file = path;
    if (RemoteImages::isRemote(path)) {
        // Only once it is on disk; until then the caller has its own guess.
        file = _remote ? _remote->cachedPath(path) : std::string();
        if (file.empty()) {
            if (_sizes.size() >= kMaxSizeMemos)
                _sizes.clear();
            _sizes[path] = {0, 0, false, true, base::monotonicMs()};
            return false;
        }
    }
    // The header only (a few KB), no decode.
    int32_t    iw = 0, ih = 0;
    const bool ok = model::imageSize(file, &iw, &ih);
    noteSize(path, iw, ih, ok);
    *w = iw;
    *h = ih;
    return ok;
}

// ── CachedImage ─────────────────────────────────────────────────────────────

CachedImage::CachedImage(ImageCache &cache, std::string path, ImageCache::Shape shape, float radius)
    : _cache(cache), _path(std::move(path)), _radius(radius), _shape(shape) {
    setRole(ui::Role::Image);
}

CachedImage::~CachedImage() {
    ui::app()->cancelTimer(_timer);
    ui::app()->cancelTimer(_settle);
    if (_waiter) // only then can a pending request name it
        _cache.forget(this);
}

void CachedImage::setPath(std::string path) {
    if (path == _path)
        return;
    _path = std::move(path);
    drop();
    _frame = 0;
    update();
}

void CachedImage::setLoadingText(std::string text, ui::C color) {
    _loadingText  = std::move(text);
    _loadingColor = color;
    _loadingLayout.reset();
    update();
}

void CachedImage::styleChanged() {
    _loadingLayout.reset(); // theme or text size changed
    View::styleChanged();
}

void CachedImage::paintPlaceholder(gfx::Painter &p, ui::RectF r, float radius) {
    if (_placeholder == ui::C::None)
        return;
    if (radius > 0)
        p.fillRoundRect(r, radius, ui::color(_placeholder));
    else
        p.fillRect(r, ui::color(_placeholder));
}

void CachedImage::setShape(ImageCache::Shape s, float radius) {
    if (s != _shape || radius != _radius)
        drop();
    _shape  = s;
    _radius = radius;
    update();
}

void CachedImage::setAnimated(bool on, bool emoji) {
    _animated = on;
    _emoji    = emoji;
    drop();
    update();
}

void CachedImage::drop() {
    _frames.reset();
    _still.reset();
    _held.reset();
    _heldW = _heldH = 0;
    _askW = _askH = 0;
}

ImageCache::Ref CachedImage::ref() const {
    const float s = windowScale();
    return {
        _path, int(std::lround(width() * s)), int(std::lround(height() * s)), _shape, _radius * s
    };
}

void CachedImage::windowChanged() {
    if (!window()) {
        ui::app()->cancelTimer(_timer);
        ui::app()->cancelTimer(_settle);
        _timer = _settle = 0;
    }
}

bool CachedImage::askNow(int w, int h) {
    // Asked already: looked up until it lands (no new decode).
    if (w == _askW && h == _askH)
        return true;
    // Moved by more than a tenth since last asked: at once (a drag asks
    // every tenth of the way, not every pixel).
    const auto far = [](int a, int b) { return std::abs(a - b) * 10 > b; };
    if (far(w, _askW) || far(h, _askH))
        return true;
    // A few pixels: once the size has held still.
    if (w == _settleW && h == _settleH)
        return !_settle;
    _settleW = w;
    _settleH = h;
    ui::app()->cancelTimer(_settle);
    _settle = ui::app()->addTimer(200, false, [this] {
        _settle = 0;
        update();
    });
    return false;
}

void CachedImage::scheduleFrame() {
    if (_timer || !_frames || _frames->size() < 2 || !window() || ui::app()->reducedMotion() ||
        !(_emoji ? _cache.animateEmoji() : _cache.animate()))
        return;
    const int delay = std::max(20, (*_frames)[size_t(_frame) % _frames->size()].delayMs);
    _timer          = ui::app()->addTimer(delay, false, [this] {
        _timer = 0;
        _frame = int(size_t(_frame + 1) % _frames->size());
        update();
    });
}

void CachedImage::paint(gfx::Painter &p) {
    View::paint(p);
    if (width() <= 0 || height() <= 0)
        return;
    const gfx::Bitmap    *bmp  = nullptr;
    const ImageCache::Ref q    = ref();
    // Once ready, the picture is kept and painted without a lookup; the
    // cache only hears that it is still in use (its LRU). A new size asks
    // again, the old picture painted scaled until it is there (no
    // placeholder flash while a window is resized).
    const bool            have = _animated ? bool(_frames) : bool(_still);
    if (have && q.width == _heldW && q.height == _heldH) {
        _cache.touch(_held);
    } else if (!have || askNow(q.width, q.height)) {
        _askW = q.width;
        _askH = q.height;
        if (_animated) {
            if (auto f = _cache.frames(q, this, &_held))
                _frames = std::move(f), _heldW = q.width, _heldH = q.height;
            else
                _waiter = true;
        } else if (auto b = _cache.get(q, this, &_held)) {
            _still = std::move(b), _heldW = q.width, _heldH = q.height;
        } else {
            _waiter = true;
        }
    }
    if (_animated && _frames) {
        bmp = &(*_frames)[size_t(_frame) % _frames->size()].frame;
        scheduleFrame();
    } else if (!_animated) {
        bmp = _still.get();
    }
    const ui::RectF r      = bounds();
    const float     radius = _shape == ImageCache::Shape::Circle    ? std::min(r.w, r.h) / 2
                             : _shape == ImageCache::Shape::Rounded ? _radius
                                                                    : 0;
    if (!bmp) {
        paintPlaceholder(p, r, radius);
        if (_loadingText.empty())
            return;
        if (!_loadingLayout) // one line ("Loading image…")
            _loadingLayout = text::layoutPlain(
                _loadingText, ui::font(ui::Font::Body, _loadingColor), windowScale()
            );
        const text::Layout &l = *_loadingLayout;
        p.save();
        p.clipRect(r);
        l.paint(p, snapPx({std::floor((r.w - l.width()) / 2), std::floor((r.h - l.height()) / 2)}));
        p.restore();
        return;
    }
    // Scaled and masked for exactly this size: a 1:1 blit (resampled only
    // while another size is on its way).
    p.drawBitmap(
        bmp->view(),
        r,
        _heldW == q.width && _heldH == q.height ? gfx::Sampling::Nearest : gfx::Sampling::Bilinear
    );
}

} // namespace screens
