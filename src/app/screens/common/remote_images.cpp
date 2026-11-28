#include "app/screens/common/remote_images.h"

#include "app/identity.h"
#include "base/crypto.h"
#include "base/mime.h"
#include "base/file.h"
#include "base/log.h"
#include "base/str.h"
#include "base/thread.h"
#include "base/time.h"
#include "gfx/gfx.h"
#include "plat/plat.h"

#include <algorithm>
#include <unordered_map>

namespace screens {

namespace {

// The cache sweep's cadence: shortly after start, then every 30
// minutes, and after a burst of 32 MB of downloads.
constexpr int     kFirstSweepMs    = 15000;
constexpr int     kSweepEveryMs    = 30 * 60 * 1000;
constexpr int64_t kSweepAfterBytes = int64_t(32) << 20;
constexpr int64_t kDefaultLimit    = int64_t(250) << 20;
// Impl::onDisk stays this small.
constexpr size_t  kMaxOnDisk       = 4096;

// What gfx can decode: PNG, JPEG, GIF, WebP by magic bytes, and SVG (text,
// so parsed; the "Slack" system user's avatar is one). Anything else —
// Slack's HTML sign-in page for a file fetched without auth, a JSON error —
// is not kept, so it can't poison the disk cache. Worker thread.
bool looksLikeImage(std::string_view b) {
    float w = 0, h = 0;
    return str::startsWith(mime::sniff(b), "image/") || gfx::svgSize(b, &w, &h);
}

// The cache sweep: while everything (the blob folders and the
// kept ones) is over `limit`, deletes the least recently used blob. The
// blob bytes left. Worker thread.
int64_t sweepDirs(
    const std::vector<std::string> &blobDirs,
    const std::vector<std::string> &keptDirs,
    int64_t                         limit
) {
    struct Blob {
        std::string path;
        int64_t     size, mtime;
    };
    std::vector<Blob> blobs;
    int64_t           total = 0, blobBytes = 0;
    for (const std::string &dir : blobDirs) {
        std::vector<file::DirEntry> all;
        if (file::listDir(dir, &all))
            for (const file::DirEntry &e : all)
                if (!e.isDir) {
                    blobs.push_back({file::join(dir, e.name), e.size, e.mtime});
                    blobBytes += e.size;
                }
    }
    total = blobBytes;
    if (limit != INT64_MAX) // only measuring: the kept folders don't matter
        for (const std::string &dir : keptDirs)
            total += file::treeBytes(dir);
    // Oldest first, one at a time: a sweep usually deletes a handful, and a
    // sort would be one more template instantiation in the binary.
    while (total > limit) {
        Blob *oldest = nullptr;
        for (Blob &b : blobs)
            if (!b.path.empty() && (!oldest || b.mtime < oldest->mtime))
                oldest = &b;
        if (!oldest)
            break;
        // A file another process holds open (Windows) stays for the next sweep.
        if (file::remove(oldest->path)) {
            total -= oldest->size;
            blobBytes -= oldest->size;
        }
        oldest->path.clear(); // done with it either way
    }
    return blobBytes;
}

} // namespace

struct RemoteImages::Impl {
    plat::App               &app;
    net::Client             *client;
    std::string              dir;
    // coverDirs(): the other folders under the limit.
    std::vector<std::string> blobDirs, keptDirs;
    AuthHook                 auth;
    Fetch                    fetch;

    struct Pending {
        std::string       url;
        std::vector<Done> waiters;
        net::RequestId    req     = 0;
        bool              started = false;
    };
    std::vector<Pending>                     pending; // running ones and the queue, in order
    int                                      active = 0;
    // Failed URLs → until when they stay failed (ms, base::monotonicMs);
    // the ones over are dropped as new ones come.
    std::unordered_map<std::string, int64_t> failed;
    int64_t                                  limit = kDefaultLimit, disk = 0, sinceSweep = 0;
    plat::TimerId                            firstSweep = 0, everySweep = 0;
    bool                                     alive = true; // UI thread; false once destroyed

    // URLs found on disk (or written) this session → their file: a hit
    // costs no hash, stat or mtime bump again. Forgotten by every sweep,
    // which may delete them (bounded: cleared when full).
    std::unordered_map<std::string, std::string> onDisk;

    // The worker: disk writes, mtime bumps, sweeps, one at a time and in
    // order; it ends when idle. Not the image decoder's: a sweep must not
    // hold up a picture.
    base::WorkerPool worker{{.maxWorkers = 1, .maxParked = 1}};

    Impl(plat::App &a, net::Client *c, std::string d) : app(a), client(c), dir(std::move(d)) {}

    void work(std::function<void()> job) { worker.post(std::move(job)); }

    std::string pathFor(const std::string &url) const {
        const auto digest = crypto::sha256(url);
        return file::join(dir, crypto::hex(crypto::bytes(digest)).substr(0, 32));
    }

    void remember(const std::string &url, const std::string &path) {
        if (onDisk.size() >= kMaxOnDisk)
            onDisk.clear(); // a bound, not a policy: a miss only costs a stat
        onDisk[url] = path;
    }

    Pending *find(const std::string &url) {
        for (Pending &p : pending)
            if (p.url == url)
                return &p;
        return nullptr;
    }

    // Down to `limit` (the setting by default; INT64_MAX only measures, 0
    // clears), then disk = what is left.
    static void
    sweep(const std::shared_ptr<Impl> &self, std::function<void()> done = {}, int64_t limit = -1) {
        self->onDisk.clear();
        self->sinceSweep               = 0;
        std::vector<std::string> blobs = self->blobDirs;
        blobs.insert(blobs.begin(), self->dir);
        if (limit < 0)
            limit = self->limit;
        self->work(
            [self, blobs = std::move(blobs), kept = self->keptDirs, limit, done = std::move(done)] {
                const int64_t left = sweepDirs(blobs, kept, limit);
                self->app.post([self, left, done] {
                    if (!self->alive)
                        return;
                    self->disk = left;
                    self->onDisk.clear(); // what it found on the way may be gone
                    if (done)
                        done();
                });
            }
        );
    }

    static void pump(const std::shared_ptr<Impl> &self) {
        for (size_t i = 0; i < self->pending.size() && self->active < kMaxParallel; ++i) {
            Pending &p = self->pending[i];
            if (p.started)
                continue;
            p.started = true;
            ++self->active;
            net::Request req;
            req.url = p.url;
            if (self->auth)
                self->auth(p.url, req.headers);
            std::string url = p.url;
            auto        cb  = [self, url](net::Response r) { landed(self, url, std::move(r)); };
            if (self->fetch)
                self->fetch(std::move(req), std::move(cb));
            else
                p.req = self->client->send(std::move(req), std::move(cb));
        }
    }

    static void landed(const std::shared_ptr<Impl> &self, const std::string &url, net::Response r) {
        if (!self->alive)
            return;
        --self->active;
        if (Pending *p = self->find(url))
            p->req = 0;
        if (r.ok()) {
            std::string  path = self->pathFor(url);
            const size_t n    = r.body.size();
            self->work([self, url, path, n, body = std::move(r.body)] {
                const bool image = looksLikeImage(body);
                const bool ok    = image && file::writeAtomic(path, body, 0644, false);
                self->app.post([self, url, path, n, image, ok] {
                    if (!self->alive)
                        return;
                    if (!image)
                        LOG_DEBUG("images", "%.80s: not an image", url.c_str());
                    finish(self, url, ok ? path : std::string(), ok ? n : 0);
                });
            });
        } else {
            LOG_DEBUG(
                "images",
                "%.80s: %d %s",
                url.c_str(),
                r.status,
                r.error.empty() ? "failed" : r.error.c_str()
            );
            finish(self, url, {}, 0);
        }
        pump(self);
    }

    // A burst of downloads sweeps before the next periodic sweep would.
    static void written(const std::shared_ptr<Impl> &self, int64_t n) {
        self->disk += n;
        self->sinceSweep += n;
        if (self->sinceSweep >= kSweepAfterBytes)
            sweep(self);
    }

    static void finish(
        const std::shared_ptr<Impl> &self, const std::string &url, const std::string &path, size_t n
    ) {
        if (path.empty()) {
            const int64_t now = base::monotonicMs();
            std::erase_if(self->failed, [now](const auto &f) { return f.second <= now; });
            self->failed[url] = now + kCooldownMs;
        } else {
            self->remember(url, path);
        }
        std::vector<Done> waiters;
        for (size_t i = 0; i < self->pending.size(); ++i)
            if (self->pending[i].url == url) {
                waiters = std::move(self->pending[i].waiters);
                self->pending.erase(self->pending.begin() + ptrdiff_t(i));
                break;
            }
        if (n)
            written(self, int64_t(n));
        for (Done &d : waiters)
            d(path);
    }
};

RemoteImages::RemoteImages(plat::App &app, net::Client *client, std::string dir)
    : _impl(std::make_shared<Impl>(app, client, std::move(dir))) {
    Impl &d = *_impl;
    if (d.dir.empty())
        return;
    file::makeDirs(d.dir);
    // How much is there (Settings shows it) — no deleting this early.
    Impl::sweep(_impl, {}, INT64_MAX);
    std::weak_ptr<Impl> weak = _impl;
    d.firstSweep             = app.addTimer(kFirstSweepMs, false, [weak] {
        if (auto self = weak.lock()) {
            self->firstSweep = 0;
            Impl::sweep(self);
        }
    });
    d.everySweep             = app.addTimer(kSweepEveryMs, true, [weak] {
        if (auto self = weak.lock())
            Impl::sweep(self);
    });
}

RemoteImages::~RemoteImages() {
    Impl &d = *_impl;
    d.alive = false; // results still on their way are dropped
    for (Impl::Pending &p : d.pending)
        if (p.req && d.client)
            d.client->cancel(p.req);
    d.pending.clear();
    if (d.firstSweep)
        d.app.cancelTimer(d.firstSweep);
    if (d.everySweep)
        d.app.cancelTimer(d.everySweep);
    d.worker.stop(); // drops the queued work, waits for the one under way
}

bool RemoteImages::isRemote(std::string_view path) {
    return str::startsWith(path, "https://") || str::startsWith(path, "http://");
}

std::string RemoteImages::defaultDir(plat::App &app) {
    const std::string cache = identity::cacheDir(app);
    return cache.empty() ? std::string() : file::join(cache, "images");
}

std::string RemoteImages::cachedPath(const std::string &url) {
    Impl &d = *_impl;
    if (d.dir.empty() || url.empty())
        return {};
    if (const auto it = d.onDisk.find(url); it != d.onDisk.end())
        return it->second;
    std::string path = d.pathFor(url);
    if (!file::exists(path))
        return {};
    // A hit is a use: the sweep deletes the least recently viewed first
    // (marked once a session; the memo answers the hits after it).
    d.work([path] { file::touch(path); });
    d.remember(url, path);
    return path;
}

bool RemoteImages::failedRecently(const std::string &url) const {
    const auto f = _impl->failed.find(url);
    return f != _impl->failed.end() && base::monotonicMs() < f->second;
}

void RemoteImages::fetch(const std::string &url, Done done) {
    Impl &d = *_impl;
    if (d.dir.empty() || (!d.client && !d.fetch) || failedRecently(url)) {
        std::weak_ptr<Impl> weak = _impl;
        d.app.post([weak, done = std::move(done)] {
            if (auto self = weak.lock(); self && self->alive && done)
                done({});
        });
        return;
    }
    if (Impl::Pending *p = d.find(url)) {
        p->waiters.push_back(std::move(done));
        return;
    }
    d.pending.push_back({url, {}, 0, false});
    d.pending.back().waiters.push_back(std::move(done));
    Impl::pump(_impl);
}

void RemoteImages::setAuth(AuthHook hook) {
    _impl->auth = std::move(hook);
    // A 401 or a sign-in page from before the workspace signed in: try again.
    _impl->failed.clear();
}

void RemoteImages::setFetch(Fetch fetch) {
    _impl->fetch = std::move(fetch);
}

void RemoteImages::coverDirs(std::vector<std::string> blobs, std::vector<std::string> kept) {
    _impl->blobDirs = std::move(blobs);
    _impl->keptDirs = std::move(kept);
    if (!_impl->dir.empty())
        Impl::sweep(_impl, {}, INT64_MAX); // what Settings shows now includes them
}

void RemoteImages::noteWritten(const std::string &path) {
    if (_impl->dir.empty() || path.empty())
        return;
    auto self = _impl;
    self->work([self, path] {
        const int64_t n = file::size(path);
        self->app.post([self, n] {
            if (self->alive && n > 0)
                Impl::written(self, n);
        });
    });
}

void RemoteImages::setLimitMb(int mb) {
    const int64_t limit = int64_t(std::max(mb, 1)) << 20;
    if (limit == _impl->limit)
        return;
    _impl->limit = limit;
    if (!_impl->dir.empty())
        Impl::sweep(_impl);
}

void RemoteImages::clear() {
    Impl &d = *_impl;
    if (d.dir.empty())
        return;
    d.disk = 0; // what Settings shows right after the click
    Impl::sweep(_impl, {}, 0);
}

int64_t RemoteImages::diskBytes() const {
    return _impl->disk;
}

size_t RemoteImages::pending() const {
    return _impl->pending.size();
}

void RemoteImages::sweepNow(std::function<void()> done) {
    if (_impl->dir.empty()) {
        if (done)
            _impl->app.post(std::move(done));
        return;
    }
    Impl::sweep(_impl, std::move(done));
}

} // namespace screens
