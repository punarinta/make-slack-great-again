// ImageCache — decoded, scaled-to-need, byte-bounded images for every screen
// (avatars, message images, GIFs, custom emoji, favicons).
//
//   auto bmp = ctx.images.get({path, 72, 72, ImageCache::Shape::Circle}, this);
//   if (bmp) p.drawBitmap(bmp->view(), rect); else paint a placeholder;
//
// get() never blocks: it returns what is cached for that exact request (path,
// physical size, shape) or null, and queues the decode on a worker thread.
// When the result lands (on the UI thread, via plat::App::post) every view
// that asked for it as `waiter` gets update(). A waiter view MUST call
// forget(this) from its destructor (CachedImage does).
//
// What is cached is exactly what is painted: the file is decoded, scaled to
// the requested physical size (cover-cropped when the aspect differs) and,
// for Circle/Rounded, pre-masked with anti-aliased corners — so painting is a
// straight blit with no per-frame resampling or clipping. Entries are evicted
// least-recently-used once the total passes the budget (default 48 MB);
// handed-out bitmaps are shared_ptrs, so eviction never pulls pixels from
// under a paint. One a view still holds is neither evicted nor counted
// against the budget: dropping it would free nothing, only cost a decode
// (a GIF bigger than the whole budget stays while shown, and goes once no
// view holds it). A file that fails to decode is remembered (no retry storm).
//
// A path is an absolute local file (fixture assets, uploads) or an http(s)
// URL: URLs are downloaded to disk by RemoteImages (setRemote; without one
// they fail) and decoded from there, waiters notified the same way. A failed
// download is asked for again once RemoteImages' cool-down for it is over.
#pragma once

#include "gfx/gfx.h"
#include "plat/plat.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace screens {

class RemoteImages;

class ImageCache {
    struct Entry;

public:
    enum class Shape : uint8_t { Square, Rounded, Circle };
    // What is asked for, without owning the path (a paint asks with a view
    // into its own string: no allocation per paint).
    struct Ref {
        std::string_view path;
        int              width = 0, height = 0; // physical px; 0,0 = the natural size
        Shape            shape  = Shape::Square;
        float            radius = 0; // physical px, Rounded only
    };
    struct Request {
        std::string path;
        int         width = 0, height = 0; // physical px; 0,0 = the natural size
        Shape       shape  = Shape::Square;
        float       radius = 0; // physical px, Rounded only
                    operator Ref() const { return {path, width, height, shape, radius}; }
    };
    using Bitmap = std::shared_ptr<const gfx::Bitmap>;
    using Frames = std::shared_ptr<const std::vector<gfx::AnimFrame>>;
    // A ready image a view keeps painting without a lookup (CachedImage):
    // touch() keeps it recently used, and tells when it was evicted.
    class Handle {
    public:
        void reset() { _e.reset(); }

    private:
        friend class ImageCache;
        std::weak_ptr<Entry> _e;
    };

    explicit ImageCache(plat::App &app, size_t budgetBytes = size_t(48) << 20);
    ~ImageCache(); // stops and joins the worker
    ImageCache(const ImageCache &)            = delete;
    ImageCache &operator=(const ImageCache &) = delete;

    // The still image (first frame of a GIF), or null while loading/failed.
    // With `held`, a ready one's handle is stored there too.
    Bitmap get(const Ref &r, ui::View *waiter = nullptr, Handle *held = nullptr);
    // Every frame of an animation (one frame for a still), or null.
    Frames frames(const Ref &r, ui::View *waiter = nullptr, Handle *held = nullptr);
    // Marks a held image used (LRU); false once it was evicted.
    bool   touch(const Handle &h);
    // True once the request finished decoding and failed (show a fallback).
    bool   failed(const Ref &r) const;
    // Drop `waiter` from every pending request (call from its destructor).
    void   forget(ui::View *waiter);

    // Natural pixel size from the file header (no decode; memoised, also
    // from every decode the worker does). False for a URL not downloaded yet
    // (remembered for a few seconds, and until its download lands here).
    bool naturalSize(const std::string &path, int *w, int *h);

    // Decodes the local file `r.path` on the worker (cover-scaled to
    // width×height, then masked for the shape) and hands the bitmap to
    // `done` on the UI thread — empty when it can't be read as an image.
    // Nothing is cached here (the caller keeps it: shell::Avatars); `urgent`
    // jumps the queue (small pictures a window waits for).
    void decodeOnce(Request r, std::function<void(gfx::Bitmap)> done, bool urgent = false);

    // Where URLs come from (main's; may stay null).
    void setRemote(RemoteImages *r) { _remote = r; }

    // Settings → Animate GIFs and images: off, animations show their first frame.
    void setAnimate(bool on) { _animate = on; }
    bool animate() const { return _animate; }
    // Settings → Animate emoji: animated custom emoji (message text,
    // reactions) play; off, they show their first frame.
    void setAnimateEmoji(bool on) { _animateEmoji = on; }
    bool animateEmoji() const { return _animateEmoji; }

    size_t bytes() const { return _bytes; } // pictures held by views included
    size_t cachedBytes() const;             // without them: what clearing frees
    size_t budget() const { return _budget; }
    void   setBudget(size_t b);
    size_t entryCount() const;
    // Requests queued or decoding (tests pump until this is 0).
    size_t pending() const;
    // How many naturalSize answers came from the disk, not the memo (tests).
    size_t sizeProbes() const { return _sizeProbes; }

    struct Impl; // worker state, shared with posted results

private:
    // An entry's identity: a view into its own path, so lookups by a Ref
    // never allocate.
    struct Key {
        std::string_view path;
        int              width, height, radius4;
        uint8_t          shape;
        bool             animated;
        bool             operator==(const Key &) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key &k) const;
    };
    static Key keyOf(const Ref &r, bool animated);

    Entry *lookup(const Ref &r, bool animated, ui::View *waiter);
    void   load(Entry &e);
    void   decode(uint64_t id, Request r, bool animated, bool urgent = false);
    void deliver(uint64_t id, std::vector<gfx::AnimFrame> frames, bool ok, bool transient = false);
    void evict();
    void lruUnlink(Entry &e);
    void lruPushFront(Entry &e);
    void noteSize(const std::string &path, int w, int h, bool ok);

    plat::App                                               &_app;
    std::shared_ptr<Impl>                                    _impl;
    RemoteImages                                            *_remote = nullptr;
    std::unordered_map<Key, std::shared_ptr<Entry>, KeyHash> _entries;
    std::unordered_map<uint64_t, Entry *>                    _loading; // by id, until delivered
    std::unordered_set<Entry *>                              _waited;  // entries with waiters
    std::vector<std::weak_ptr<Entry>> _failed; // failed entries, oldest first
    std::unordered_map<uint64_t, std::function<void(gfx::Bitmap)>> _once; // decodeOnce
    Entry *_lruHead = nullptr, *_lruTail = nullptr; // ready entries, most recent first
    struct SizeMemo {
        int     w = 0, h = 0;
        bool    ok      = false;
        bool    missing = false; // a URL not on disk (yet)
        int64_t at      = 0;     // ms (base::monotonicMs), when `missing` was found
    };
    std::unordered_map<std::string, SizeMemo> _sizes; // bounded (kMaxSizeMemos)
    size_t                                    _bytes = 0, _budget, _sizeProbes = 0;
    uint64_t                                  _nextId       = 1;
    bool                                      _animate      = true;
    bool                                      _animateEmoji = true;
};

// A view showing one cached image at its own size (avatars, favicons,
// thumbnails). Placeholder colour until the image is ready; animated GIFs
// play while the view is in a window (and not under reduced motion).
class CachedImage : public ui::View {
public:
    CachedImage(
        ImageCache       &cache,
        std::string       path   = {},
        ImageCache::Shape shape  = ImageCache::Shape::Square,
        float             radius = 0
    );
    ~CachedImage() override;

    void               setPath(std::string path);
    void               setShape(ImageCache::Shape s, float radius = 0); // radius logical px
    // Play GIF frames; `emoji`: gated by animateEmoji(), not animate().
    void               setAnimated(bool on, bool emoji = false);
    void               setPlaceholder(ui::C c) { _placeholder = c; }
    // "Loading image…": text centred over the placeholder until the
    // picture is there (Font::Body in `color`).
    void               setLoadingText(std::string text, ui::C color);
    const std::string &path() const { return _path; }

    void paint(gfx::Painter &p) override;
    void windowChanged() override;
    void styleChanged() override;

protected:
    // What shows while the image isn't there (default: the placeholder colour).
    virtual void paintPlaceholder(gfx::Painter &p, ui::RectF r, float radius);

private:
    ImageCache::Ref ref() const;
    void            drop(); // forget the picture held (path, shape or mode changed)
    void            scheduleFrame();
    // Whether to ask for size w×h now while another size is painted (see paint).
    bool            askNow(int w, int h);

    std::string                   _loadingText;
    std::unique_ptr<text::Layout> _loadingLayout;
    ui::C                         _loadingColor = ui::C::TextFaint;

    ImageCache        &_cache;
    std::string        _path;
    ImageCache::Frames _frames; // while animating
    ImageCache::Bitmap _still;  // once ready: painted without a lookup
    ImageCache::Handle _held;   // what _still / _frames came from
    int                _heldW = 0, _heldH = 0;
    int                _askW = 0, _askH = 0;       // the size last asked for
    int                _settleW = 0, _settleH = 0; // the size _settle waits on
    plat::TimerId      _timer  = 0;
    plat::TimerId      _settle = 0; // a new size holding still
    float              _radius = 0;
    int                _frame  = 0;
    ImageCache::Shape  _shape;
    ui::C              _placeholder = ui::C::Border;
    bool               _animated    = false;
    bool               _emoji       = false;
    bool               _waiter      = false; // asked for one not ready yet (forget on exit)
};

} // namespace screens
