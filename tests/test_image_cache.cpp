// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for ImageCache's in-memory bound (the LRU eviction added so the decoded
// pixmaps no longer grow unbounded for the whole process lifetime):
//   - resident bytes never exceed the configured cap once it is crossed
//   - a plain cache hit neither re-accounts nor reorders (paint-path stays O(1))
//   - staying under the cap evicts nothing
//   - the freshly inserted url survives eviction triggered by its own insert

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QBuffer>
#include <QDeadlineTimer>
#include <QHash>
#include <QImage>

#include "fake_http_server.h"
#include "ui/image_cache.h"

#include <functional>

namespace {

bool waitFor(std::function<bool()> pred, int timeoutMs = 5000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// Let any pending request reach the server, so "no second request" is a real
// observation rather than a race we won.
void settle(int ms = 300) {
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-image-cache");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

// A 256×256 ARGB image decodes to 256*256*4 = 256 KiB resident — the cost
// ImageCache accounts (it measures the decoded pixmap, not the encoded bytes).
static constexpr int    kSide      = 256;
static constexpr qint64 kEntryCost = qint64(kSide) * kSide * 4;

static QByteArray makePng() {
    QImage img(kSide, kSide, QImage::Format_ARGB32);
    img.fill(Qt::red);
    QByteArray bytes;
    QBuffer    buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

// Wire a synchronous in-memory disk backend so get() inserts without any
// network round-trip: every requested url resolves to the same PNG bytes.
static void wireDisk(ImageCache &cache) {
    static const QByteArray png = makePng();
    cache.setDiskCache(
        [](const QString &) { return png; }, [](const QString &, const QByteArray &) {}
    );
}

TEST_CASE("ImageCache stays under its memory cap") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    wireDisk(cache);
    // Room for ~4 entries.
    const qint64 cap = 4 * kEntryCost + kEntryCost / 2;
    cache.setMemoryCap(cap);

    for (int i = 0; i < 50; ++i) {
        const QPixmap px = cache.get(QStringLiteral("img-%1").arg(i));
        REQUIRE_FALSE(px.isNull()); // disk path returns it now
        REQUIRE(cache.memoryBytes() <= cap);
    }
    // Bounded, not unbounded: 50 inserts must not have retained 50 entries.
    REQUIRE(cache.memoryBytes() <= 5 * kEntryCost);
    REQUIRE(cache.memoryBytes() >= kEntryCost); // but it did keep the recent ones
}

TEST_CASE("Under-cap usage evicts nothing") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    wireDisk(cache);
    cache.setMemoryCap(100 * kEntryCost);

    for (int i = 0; i < 10; ++i)
        cache.get(QStringLiteral("img-%1").arg(i));

    REQUIRE(cache.memoryBytes() == 10 * kEntryCost);
}

TEST_CASE("A repeated hit does not re-account or grow memory") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    wireDisk(cache);
    cache.setMemoryCap(100 * kEntryCost);

    cache.get(QStringLiteral("same"));
    const qint64 after1 = cache.memoryBytes();
    for (int i = 0; i < 20; ++i)
        cache.get(QStringLiteral("same"));

    REQUIRE(cache.memoryBytes() == after1);
    REQUIRE(after1 == kEntryCost);
}

TEST_CASE("Eviction drops the least recently used entry, and a hit counts as a use") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    wireDisk(cache);
    cache.setMemoryCap(3 * kEntryCost);

    cache.get(QStringLiteral("a"));
    cache.get(QStringLiteral("b"));
    cache.get(QStringLiteral("c"));
    REQUIRE(cache.memoryBytes() == 3 * kEntryCost);

    // Touch the oldest insert: it must now outlive "b", the least recently used.
    cache.get(QStringLiteral("a"));
    cache.get(QStringLiteral("d")); // over cap → one eviction
    REQUIRE(cache.memoryBytes() == 3 * kEntryCost);

    // Re-fetching an evicted url re-inserts it (and grows memory until the next
    // eviction); a resident one is a free hit. Probe with a cap raise so the
    // probe itself evicts nothing.
    cache.setMemoryCap(100 * kEntryCost);
    const qint64 before = cache.memoryBytes();
    cache.get(QStringLiteral("a"));
    cache.get(QStringLiteral("c"));
    cache.get(QStringLiteral("d"));
    CHECK(cache.memoryBytes() == before); // all three still resident
    cache.get(QStringLiteral("b"));
    CHECK(cache.memoryBytes() == before + kEntryCost); // "b" was the victim
}

TEST_CASE("Asynchronous decode: get() returns null, then loaded() delivers the pixmap") {
    ImageCache cache; // default: decode on a pool thread
    wireDisk(cache);
    int     loads = 0;
    QString last;
    QObject::connect(&cache, &ImageCache::loaded, [&](const QString &u) {
        ++loads;
        last = u;
    });

    // Disk hit: sentinel now, pixels later — same contract as a download.
    REQUIRE(cache.get(QStringLiteral("async-0")).isNull());
    // In flight: geometry arrives with loaded(), like a download.
    REQUIRE(cache.sizeOf(QStringLiteral("async-0")).isEmpty());
    REQUIRE(waitFor([&] { return loads == 1; }));
    CHECK(cache.sizeOf(QStringLiteral("async-0")) == QSize(kSide, kSide));
    CHECK(last == QStringLiteral("async-0"));
    CHECK(cache.get(QStringLiteral("async-0")).size() == QSize(kSide, kSide));
    CHECK(cache.memoryBytes() == kEntryCost);
    // Repeat hits are free and emit nothing.
    cache.get(QStringLiteral("async-0"));
    CHECK(loads == 1);
}

// Minimal valid two-frame 1×1 GIF89a — enough for QImageReader to report a
// multi-frame animation and for QMovie to construct a valid player.
static QByteArray makeTwoFrameGif() {
    static const char frame[] = "21f904040a000000"     // graphic control ext
                                "2c000000000100010000" // image descriptor 1×1
                                "0202440100";          // LZW data
    return QByteArray::fromHex(
        QByteArray(
            "474946383961" // "GIF89a"
            "010001008000" // 1×1, global color table (2 colors)
            "00"           // aspect
            "000000ffffff"
        ) // color table: black, white
        + QByteArray(frame) + QByteArray(frame) + QByteArray("3b")
    );
}

TEST_CASE("A held movie pins its entry; releaseMovie unpins it") {
    static const QByteArray gif = makeTwoFrameGif();
    REQUIRE(ImageCache::isAnimatedImage(gif));

    ImageCache cache;
    cache.setSynchronousDecode(true);
    static const QByteArray png = makePng();
    cache.setDiskCache(
        [](const QString &url) { return url.startsWith(u"anim") ? gif : png; },
        [](const QString &, const QByteArray &) {}
    );
    cache.setMemoryCap(1); // everything unpinned must be evicted immediately

    REQUIRE_FALSE(cache.get(QStringLiteral("anim")).isNull());
    QMovie *m1 = cache.movie(QStringLiteral("anim"));
    REQUIRE(m1 != nullptr);
    QMovie *m2 = cache.movie(QStringLiteral("anim")); // second holder, same player
    REQUIRE(m2 == m1);

    // Eviction pressure: the pinned animation must survive it.
    cache.get(QStringLiteral("img-0"));
    const qint64 whilePinned = cache.memoryBytes();
    REQUIRE(whilePinned > 0);

    // One holder releasing keeps the pin (the other still uses the player).
    cache.releaseMovie(QStringLiteral("anim"));
    REQUIRE(cache.memoryBytes() > 0);

    // Last holder gone: the entry becomes evictable and the 1-byte cap purges it.
    cache.releaseMovie(QStringLiteral("anim"));
    REQUIRE(cache.memoryBytes() == 0);

    // Releasing an unknown or already-released url is a harmless no-op.
    cache.releaseMovie(QStringLiteral("anim"));
    cache.releaseMovie(QStringLiteral("never-seen"));
}

// ── Animation retention (Settings → Appearance → Visual effects) ─────────────
//
// Turning animations off is meant to save memory: the cache's share is the
// encoded bytes it keeps for every animated download so a QMovie can be built
// later. With retention off those bytes must never be held, and switching
// back on must bring the animation back (via a re-fetch) rather than leave a
// still behind.

TEST_CASE("Animations not retained: no bytes kept, movie() is null, re-enable re-fetches") {
    static const QByteArray gif   = makeTwoFrameGif();
    static const QByteArray png   = makePng();
    int                     loads = 0;
    ImageCache              cache;
    cache.setSynchronousDecode(true);
    cache.setDiskCache(
        [&](const QString &url) {
            ++loads;
            return url.startsWith(u"anim") ? gif : png;
        },
        [](const QString &, const QByteArray &) {}
    );

    cache.setAnimationsRetained(false);
    REQUIRE_FALSE(cache.animationsRetained());
    REQUIRE_FALSE(cache.get(QStringLiteral("anim")).isNull()); // the still first frame
    REQUIRE(loads == 1);
    // A 1×1 still costs 4 bytes; the retained GIF would add its encoded size.
    REQUIRE(cache.memoryBytes() < gif.size());
    REQUIRE(cache.movie(QStringLiteral("anim")) == nullptr);
    REQUIRE_FALSE(cache.get(QStringLiteral("anim")).isNull()); // still cached, no refetch
    REQUIRE(loads == 1);

    // Back on: the still is forgotten so the next get() fetches the animation.
    cache.setAnimationsRetained(true);
    REQUIRE_FALSE(cache.get(QStringLiteral("anim")).isNull());
    REQUIRE(loads == 2);
    REQUIRE(cache.memoryBytes() >= gif.size());
    QMovie *m = cache.movie(QStringLiteral("anim"));
    REQUIRE(m != nullptr);
    cache.releaseMovie(QStringLiteral("anim"));
}

TEST_CASE("Turning retention off drops the animation bytes already held, except a played one") {
    static const QByteArray gif = makeTwoFrameGif();
    ImageCache              cache;
    cache.setSynchronousDecode(true);
    cache.setDiskCache(
        [](const QString &) { return gif; }, [](const QString &, const QByteArray &) {}
    );
    REQUIRE_FALSE(cache.get(QStringLiteral("anim-a")).isNull());
    REQUIRE_FALSE(cache.get(QStringLiteral("anim-b")).isNull());
    const qint64 both = cache.memoryBytes();
    REQUIRE(both >= 2 * gif.size());
    QMovie *held = cache.movie(QStringLiteral("anim-b"));
    REQUIRE(held != nullptr);

    cache.setAnimationsRetained(false);
    // anim-a lost its bytes; anim-b is being played and keeps them until released.
    REQUIRE(cache.memoryBytes() < both);
    REQUIRE(cache.memoryBytes() >= gif.size());
    REQUIRE(cache.movie(QStringLiteral("anim-a")) == nullptr);
    REQUIRE(cache.movie(QStringLiteral("anim-b")) == held);
    cache.releaseMovie(QStringLiteral("anim-b"));
    cache.releaseMovie(QStringLiteral("anim-b"));
}

TEST_CASE("discardAnimation frees one url's bytes; restoreDiscardedAnimations re-fetches it") {
    static const QByteArray gif   = makeTwoFrameGif();
    int                     loads = 0;
    ImageCache              cache;
    cache.setSynchronousDecode(true);
    cache.setDiskCache(
        [&](const QString &) {
            ++loads;
            return gif;
        },
        [](const QString &, const QByteArray &) {}
    );
    REQUIRE_FALSE(cache.get(QStringLiteral("emoji")).isNull());
    REQUIRE_FALSE(cache.get(QStringLiteral("giphy")).isNull());
    const qint64 both = cache.memoryBytes();

    cache.discardAnimation(QStringLiteral("giphy"));
    REQUIRE(cache.memoryBytes() < both);
    REQUIRE(cache.movie(QStringLiteral("giphy")) == nullptr);
    QMovie *m = cache.movie(QStringLiteral("emoji")); // the other kind is untouched
    REQUIRE(m != nullptr);
    cache.releaseMovie(QStringLiteral("emoji"));
    cache.discardAnimation(QStringLiteral("never-seen")); // harmless

    // Only the discarded url is forgotten and re-fetched.
    cache.restoreDiscardedAnimations();
    REQUIRE(loads == 2);
    REQUIRE_FALSE(cache.get(QStringLiteral("emoji")).isNull());
    REQUIRE(loads == 2);
    REQUIRE_FALSE(cache.get(QStringLiteral("giphy")).isNull());
    REQUIRE(loads == 3);
    REQUIRE(cache.movie(QStringLiteral("giphy")) != nullptr);
    cache.releaseMovie(QStringLiteral("giphy"));
}

// ── Sizing without decoding (the layout treadmill) ───────────────────────────
//
// rebuildLayout() measures EVERY row in the conversation, and it used to size
// preview images through get() — so laying out a preview-heavy channel required
// the decoded pixmap of every image in it to be resident at once. Past the
// memory cap that became an eviction treadmill: each measuring pass evicted the
// entry the next pass asked for first, so every layout re-read and re-decoded
// the entire image cache. sizeOf() exists to break that coupling.

TEST_CASE("sizeOf reports geometry without making pixels resident") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    wireDisk(cache);
    cache.setMemoryCap(100 * kEntryCost);

    REQUIRE(cache.sizeOf(QStringLiteral("img-0")) == QSize(kSide, kSide));
    // The decisive property: geometry cost no resident pixels at all, so
    // measuring can never apply eviction pressure.
    REQUIRE(cache.memoryBytes() == 0);
}

static QByteArray makePng(int w, int h, const char *fmt = "PNG") {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::blue);
    QByteArray bytes;
    QBuffer    buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, fmt);
    return bytes;
}

TEST_CASE("Oversized sources decode bounded to maxDecodeDim, aspect kept") {
    // A 4000×3000 unfurl decoded natively is 48 MB of pixels for a 400×300
    // preview (issue #64). The bound caps any one entry at ~2 MB at 2×.
    const int maxDim = ImageCache::maxDecodeDim();
    REQUIRE(maxDim >= 2 * ImageCache::kMaxDecodeLogical); // never below 2× quality
    REQUIRE(maxDim % 4 == 0);                             // the size arithmetic below is exact
    const QSize wide = ImageCache::boundedSize(QSize(4000, 3000));
    CHECK(wide == QSize(maxDim, maxDim * 3 / 4));
    CHECK(ImageCache::boundedSize(QSize(300, 4000)) == QSize(maxDim * 300 / 4000, maxDim));
    // Never upscaled.
    CHECK(ImageCache::boundedSize(QSize(kSide, kSide)) == QSize(kSide, kSide));

    const QPixmap png = ImageCache::decodeBounded(makePng(2 * maxDim, maxDim / 2));
    REQUIRE_FALSE(png.isNull());
    CHECK(png.size() == QSize(maxDim, maxDim / 4));

    const QPixmap jpg = ImageCache::decodeBounded(makePng(maxDim / 2, 2 * maxDim, "JPEG"));
    REQUIRE_FALSE(jpg.isNull());
    CHECK(jpg.size() == QSize(maxDim / 4, maxDim));

    // Small images are untouched.
    CHECK(ImageCache::decodeBounded(makePng()).size() == QSize(kSide, kSide));
    CHECK(ImageCache::decodeBounded(QByteArrayLiteral("not an image")).isNull());
}

TEST_CASE("A bounded entry is accounted and measured at its bounded size") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    const int               maxDim = ImageCache::maxDecodeDim();
    static const QByteArray big    = makePng(2 * maxDim, maxDim);
    cache.setDiskCache(
        [](const QString &) { return big; }, [](const QString &, const QByteArray &) {}
    );

    const QSize expect(maxDim, maxDim / 2);
    // Header-only measurement and the resident pixmap must agree (layout vs paint).
    REQUIRE(cache.sizeOf(QStringLiteral("big-0")) == expect);
    REQUIRE(cache.get(QStringLiteral("big-0")).size() == expect);
    REQUIRE(cache.memoryBytes() == qint64(expect.width()) * expect.height() * 4);
}

TEST_CASE("sizeOf agrees with the decoded pixmap size") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    wireDisk(cache);
    cache.setMemoryCap(100 * kEntryCost);

    // Paint scales from the pixmap, layout from sizeOf: a mismatch would show up
    // as rows whose reserved height disagrees with what is drawn in them.
    const QSize measured = cache.sizeOf(QStringLiteral("img-0"));
    REQUIRE(measured == cache.get(QStringLiteral("img-0")).size());

    // And the reverse order agrees too: a resident pixmap is the size authority.
    ImageCache other;
    other.setSynchronousDecode(true);
    wireDisk(other);
    REQUIRE(other.get(QStringLiteral("img-1")).size() == other.sizeOf(QStringLiteral("img-1")));
}

TEST_CASE("Measuring a working set larger than the cap decodes each url once") {
    ImageCache cache;
    cache.setSynchronousDecode(true);
    static const QByteArray png   = makePng();
    int                     loads = 0;
    cache.setDiskCache(
        [&loads](const QString &) {
            ++loads;
            return png;
        },
        [](const QString &, const QByteArray &) {}
    );
    cache.setMemoryCap(4 * kEntryCost); // room for ~4 of the 50 urls

    // Three full measuring passes over a working set 12× the cap — exactly the
    // shape of rebuildLayout() running on a channel full of link previews.
    const int kUrls = 50;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < kUrls; ++i)
            REQUIRE(cache.sizeOf(QStringLiteral("img-%1").arg(i)) == QSize(kSide, kSide));

    // One read per url for the whole run. Sizing through get() gave 150 here,
    // every one of them a full PNG decode, because the cap could hold only 4.
    REQUIRE(loads == kUrls);
    REQUIRE(cache.memoryBytes() == 0);
}

// ── The negative cache ───────────────────────────────────────────────────────
//
// A url whose bytes arrive but do not decode stores nothing and disk-saves
// nothing, so every later get() missed and re-issued the request — and each
// completion emitted loaded(), which drove another full relayout, which asked
// again. That is a self-sustaining loop, and it is what pinned a core at 100%.

TEST_CASE("Bytes that are not an image are fetched once, never again") {
    FakeHttpServer server;
    // 200 OK with an HTML body: what a CDN serves for an expired preview url.
    server.enqueueStatus(200, "OK", "text/html", "<html><body>gone</body></html>");

    ImageCache cache;
    cache.setSynchronousDecode(true);
    static const QByteArray png = makePng();
    // Only "img-" urls exist on disk, so the broken one must go to the network.
    cache.setDiskCache(
        [](const QString &url) { return url.contains(u"img-") ? png : QByteArray{}; },
        [](const QString &, const QByteArray &) {}
    );
    cache.setMemoryCap(2 * kEntryCost);

    const QString broken = server.baseUrl() + QStringLiteral("preview.png");

    REQUIRE(cache.get(broken).isNull()); // miss: starts the fetch
    REQUIRE(waitFor([&] { return server.requestCount >= 1; }));
    settle();
    REQUIRE(server.requestCount == 1);

    // The failed entry holds no pixels, so cap pressure happily evicts it — and
    // an evicted failure is exactly what used to go back to the network.
    for (int i = 0; i < 10; ++i)
        REQUIRE_FALSE(cache.get(QStringLiteral("img-%1").arg(i)).isNull());

    // Both accessors must now refuse it from the sentinel alone.
    REQUIRE(cache.get(broken).isNull());
    REQUIRE(cache.sizeOf(broken).isEmpty());
    settle();
    REQUIRE(server.requestCount == 1);

    // Repeated measuring passes — the layout loop — issue nothing either.
    for (int pass = 0; pass < 20; ++pass)
        REQUIRE(cache.sizeOf(broken).isEmpty());
    settle();
    REQUIRE(server.requestCount == 1);
}

TEST_CASE("A transport error does not permanently blacklist the url") {
    FakeHttpServer server;
    server.dropConnections = 1; // close the first request without responding

    ImageCache cache;
    cache.setSynchronousDecode(true);

    cache.setSynchronousDecode(true);
    cache.setDiskCache({}, {}); // network only
    cache.setMemoryCap(100 * kEntryCost);

    const QString url = server.baseUrl() + QStringLiteral("flaky.png");
    REQUIRE(cache.get(url).isNull());
    REQUIRE(waitFor([&] { return server.requestCount >= 1; }));

    // Unlike undecodable bytes, a dropped connection may well succeed later, so
    // the sentinel is a cooldown rather than a permanent verdict — it exists to
    // stop the storm, not to give up on the image.
    settle();
    const int afterError = server.requestCount;
    for (int pass = 0; pass < 20; ++pass)
        cache.get(url);
    settle();
    REQUIRE(server.requestCount == afterError); // inside the cooldown: silent
}
