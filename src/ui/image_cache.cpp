// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "image_cache.h"

#include "network/shared_nam.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSvgRenderer>
#include <QThreadPool>
#include <QUrl>
#include <QtMath>

ImageCache::ImageCache(QObject *parent) : QObject(parent), _nam(net::sharedNam()) {
    maxDecodeDim(); // reads QScreen geometry — resolve it here, never on a worker
}

namespace {

// Resident bytes an entry holds: decoded pixmap (raw device pixels) plus the
// retained encoded bytes of an animation.
qint64 entryCost(const QPixmap &px, const QByteArray &animatedBytes) {
    qint64 c = animatedBytes.size();
    if (!px.isNull())
        c += qint64(px.width()) * px.height() * qMax(1, px.depth() / 8);
    return c;
}

} // namespace

void ImageCache::account(const QString &url) {
    auto it = _cache.find(url);
    if (it == _cache.end())
        return;
    const qint64 fresh = entryCost(it->pixmap, it->animatedBytes);
    _memBytes += fresh - it->cost;
    it->cost     = fresh;
    it->lastUsed = ++_useTick;

    evictIfNeeded(url);
}

void ImageCache::evictIfNeeded(const QString &protectUrl) {
    while (_memBytes > _memoryCap) {
        // Least recently used entry we may drop.
        auto victim = _cache.end();
        for (auto it = _cache.begin(); it != _cache.end(); ++it) {
            if (it.key() == protectUrl)
                continue;
            // A live QMovie is handed out by pointer and cached by callers
            // (MessageListWidget::_gifMovies) with a frameChanged connection —
            // deleting it here would dangle; it stays pinned until every holder
            // calls releaseMovie(). In-flight sentinels must survive so their
            // finished handler can complete. Both are pinned.
            if (it->inFlight || it->movie)
                continue;
            if (victim == _cache.end() || it->lastUsed < victim->lastUsed)
                victim = it;
        }
        if (victim == _cache.end())
            break; // everything left is pinned — cap is a soft target

        _memBytes -= victim->cost;
        _cache.erase(victim);
    }
}

int ImageCache::maxDecodeDim() {
    static const int dim = [] {
        qreal dpr = 2.0; // never below Retina quality, even when probed headless
        if (const auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
            for (const QScreen *sc : app->screens())
                dpr = std::max(dpr, sc->devicePixelRatio());
        return qCeil(kMaxDecodeLogical * std::min(dpr, 4.0));
    }();
    return dim;
}

QSize ImageCache::boundedSize(QSize sz, int maxDim) {
    if (maxDim <= 0)
        maxDim = maxDecodeDim();
    if (sz.isEmpty() || (sz.width() <= maxDim && sz.height() <= maxDim))
        return sz;
    return sz.scaled(maxDim, maxDim, Qt::KeepAspectRatio);
}

QImage ImageCache::decodeBoundedImage(const QByteArray &bytes, int maxDim) {
    if (maxDim <= 0)
        maxDim = maxDecodeDim();
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    // Header first: only ask for a scaled decode when the source is actually
    // larger than the bound, so small images (avatars, emoji) decode as before.
    // JPEG scales inside the decoder (DCT downscale); other formats decode then
    // shrink, so the transient peak is native size but nothing native is kept.
    const QSize  natural = reader.size();
    if (!natural.isEmpty()) {
        const QSize bounded = boundedSize(natural, maxDim);
        if (bounded != natural)
            reader.setScaledSize(bounded);
    }
    QImage img = reader.read();
    if (!img.isNull()) {
        // A plugin that ignores ScaledSize hands back the native image.
        const QSize bounded = boundedSize(img.size(), maxDim);
        if (bounded != img.size())
            img = img.scaled(bounded, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        return img;
    }
    // Not a raster QImageReader knows — try SVG (workspace/emoji icons).
    QSvgRenderer r(bytes);
    if (r.isValid()) {
        QSize sz = r.defaultSize();
        if (sz.isEmpty())
            sz = QSize(128, 128);
        const int kMax = 256;
        if (sz.width() > kMax || sz.height() > kMax)
            sz.scale(kMax, kMax, Qt::KeepAspectRatio);
        QImage out(sz, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        r.render(&p);
        return out;
    }
    return {};
}

QPixmap ImageCache::decodeBounded(const QByteArray &bytes, int maxDim) {
    return QPixmap::fromImage(decodeBoundedImage(bytes, maxDim));
}

bool ImageCache::isAnimatedImage(const QByteArray &bytes) {
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    return reader.supportsAnimation() && reader.imageCount() > 1;
}

QMovie *ImageCache::movie(const QString &url) {
    auto it = _cache.find(url);
    if (it == _cache.end() || it->animatedBytes.isEmpty())
        return nullptr;
    if (!it->movie) {
        auto *buf = new QBuffer;
        buf->setData(it->animatedBytes);
        buf->open(QIODevice::ReadOnly);
        auto *m = new QMovie(buf);
        buf->setParent(m);
        if (!m->isValid()) {
            delete m;
            it->animatedBytes.clear();
            return nullptr;
        }
        m->setParent(this);
        it->movie     = m;
        it->movieRefs = 0;
    }
    ++it->movieRefs;
    return it->movie;
}

void ImageCache::releaseMovie(const QString &url) {
    auto it = _cache.find(url);
    if (it == _cache.end() || !it->movie)
        return;
    if (--it->movieRefs > 0)
        return;
    // Last holder gone: free the player (its QBuffer is a child) and let the
    // entry be evicted like any other — a paused-forever QMovie was pinning
    // its raw bytes plus decoded frames past the memory cap indefinitely.
    delete it->movie;
    it->movie     = nullptr;
    it->movieRefs = 0;
    evictIfNeeded(QString());
}

void ImageCache::setAnimationsRetained(bool on) {
    if (_retainAnimations == on)
        return;
    _retainAnimations = on;
    if (on) {
        restoreDiscardedAnimations();
        return;
    }
    const auto urls = _cache.keys();
    for (const auto &url : urls)
        discardAnimation(url);
}

void ImageCache::discardAnimation(const QString &url) {
    auto it = _cache.find(url);
    // A held player still reads the bytes through its QBuffer; the holder
    // releases it first (releaseGifMovies) and the next paint discards.
    if (it == _cache.end() || it->animatedBytes.isEmpty() || it->movie)
        return;
    it->animatedBytes    = QByteArray();
    it->animationDropped = true;
    account(url);
}

void ImageCache::restoreDiscardedAnimations() {
    for (auto it = _cache.begin(); it != _cache.end();) {
        if (!it->animationDropped || it->inFlight) {
            ++it;
            continue;
        }
        _memBytes -= it->cost;
        it = _cache.erase(it);
    }
}

void ImageCache::setDiskCache(
    std::function<QByteArray(const QString &)>               load,
    std::function<void(const QString &, const QByteArray &)> save
) {
    _diskLoad = std::move(load);
    _diskSave = std::move(save);
}

// Intrinsic size straight from the image header, without decoding the pixels.
// Invalid for formats QImageReader can't introspect (notably SVG), where the
// caller falls back to a one-off decode.
static QSize intrinsicSize(const QByteArray &bytes) {
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    const QSize  sz = reader.size();
    // Report what the pixmap will actually be: paint scales from the (bounded)
    // pixmap, so layout must measure the same thing.
    return sz.isEmpty() ? QSize() : ImageCache::boundedSize(sz);
}

void ImageCache::noteSize(const QString &url, const QSize &sz) {
    if (!sz.isEmpty())
        _sizes.insert(url, sz);
}

bool ImageCache::isFailed(const QString &url) const {
    const auto it = _failedUntil.constFind(url);
    if (it == _failedUntil.constEnd())
        return false;
    if (*it == 0)
        return true; // undecodable bytes — never retry
    if (QDateTime::currentMSecsSinceEpoch() < *it)
        return true;
    return false;
}

void ImageCache::markFailed(const QString &url, bool permanent) {
    _failedUntil.insert(
        url, permanent ? 0 : QDateTime::currentMSecsSinceEpoch() + kErrorCooldownMs
    );
}

void ImageCache::startFetch(const QString &url) {
    auto &entry    = _cache[url];
    entry.inFlight = true; // sentinel from here on, whether running or queued
    if (_activeFetches >= kMaxParallelFetches) {
        _fetchQueue.enqueue(url);
        return;
    }
    issueFetch(url);
}

void ImageCache::pumpFetchQueue() {
    while (_activeFetches < kMaxParallelFetches && !_fetchQueue.isEmpty()) {
        const QString next = _fetchQueue.dequeue();
        // Defensive: eviction never touches an in-flight sentinel, so a queued
        // url should always still be there — skip it if that ever changes.
        if (const auto it = _cache.constFind(next); it == _cache.constEnd() || !it->inFlight)
            continue;
        issueFetch(next);
    }
}

void ImageCache::issueFetch(const QString &url) {
    // A file:// url (custom workspace icons) is read straight from disk, a
    // qrc:/ url (built-in avatars, e.g. Claude Code sessions) from the app's
    // resources: no download slot to hold, and nothing to copy into the disk
    // cache — the file IS the durable copy. Same completion path as a download
    // otherwise.
    const QUrl u(url);
    if (u.isLocalFile() || u.scheme() == QLatin1String("qrc")) {
        QFile f(u.isLocalFile() ? u.toLocalFile() : QLatin1Char(':') + u.path());
        if (f.open(QIODevice::ReadOnly)) {
            decodeAsync(url, f.readAll(), /*saveToDisk=*/false);
        } else {
            auto &e    = _cache[url];
            e.inFlight = false;
            markFailed(url, /*permanent=*/false);
            account(url);
            emit loaded(url);
        }
        return;
    }
    ++_activeFetches;
    auto *reply = _nam->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        reply->deleteLater();
        --_activeFetches;
        // Hand the slot on before decoding: the next download overlaps this
        // decode instead of waiting for it.
        pumpFetchQueue();
        if (reply->error() == QNetworkReply::NoError) {
            decodeAsync(url, reply->readAll(), /*saveToDisk=*/true);
        } else {
            auto &e    = _cache[url];
            e.inFlight = false;
            markFailed(url, /*permanent=*/false);
            account(url); // no-op cost change — keeps the entry's LRU stamp fresh
            emit loaded(url);
        }
    });
}

void ImageCache::decodeAsync(const QString &url, QByteArray bytes, bool saveToDisk) {
    // The entry keeps its in-flight sentinel until the decode lands, so callers
    // see "loading" (null pixmap) and eviction leaves it alone.
    _cache[url].inFlight = true;
    if (_syncDecode) {
        finishDecode(url, bytes, decodeBoundedImage(bytes), saveToDisk);
        return;
    }
    // Decode on a pool thread: a progressive JPEG or big PNG takes 50-200 ms,
    // and until this change it ran inside paint whenever an evicted unfurl
    // scrolled back into view (issue #64 follow-up: visible scroll hitching).
    // QImage is thread-safe to build off the GUI thread; the QPixmap is made in
    // finishDecode() back on it. The result is posted to the application
    // object (always alive while the loop runs) and checked against a QPointer
    // there, so a cache destroyed mid-decode is simply skipped — never touched
    // from the worker.
    QPointer<ImageCache> self(this);
    QThreadPool::globalInstance()->start(
        [self, url, bytes = std::move(bytes), saveToDisk]() mutable {
            QImage img = decodeBoundedImage(bytes);
            auto  *app = QCoreApplication::instance();
            if (!app)
                return;
            QMetaObject::invokeMethod(
                app,
                [self, url, bytes = std::move(bytes), img = std::move(img), saveToDisk]() mutable {
                    if (self)
                        self->finishDecode(url, bytes, std::move(img), saveToDisk);
                },
                Qt::QueuedConnection
            );
        }
    );
}

void ImageCache::finishDecode(
    const QString &url, const QByteArray &bytes, QImage img, bool saveToDisk
) {
    auto &e    = _cache[url];
    e.inFlight = false;
    if (!img.isNull()) {
        e.pixmap = QPixmap::fromImage(std::move(img));
        noteSize(url, e.pixmap.size());
        if (isAnimatedImage(bytes)) {
            if (_retainAnimations)
                e.animatedBytes = bytes;
            else
                e.animationDropped = true;
        }
        if (saveToDisk && _diskSave)
            _diskSave(url, bytes);
    } else {
        // The bytes are not an image (an HTML error page, say). Nothing is
        // stored and nothing is disk-saved, so without this sentinel every
        // later get() missed and re-issued the request — and each completion
        // emitted loaded(), driving another full relayout.
        markFailed(url, /*permanent=*/true);
    }
    account(url); // no-op cost change if the decode yielded nothing
    emit loaded(url);
}

QSize ImageCache::sizeOf(const QString &url) {
    if (url.isEmpty())
        return {};

    const auto known = _sizes.constFind(url);
    if (known != _sizes.constEnd())
        return *known;

    // A resident pixmap is the authority: paint scales from it, so geometry
    // measured from any other number could disagree by a pixel.
    const auto it = _cache.constFind(url);
    if (it != _cache.constEnd()) {
        if (!it->pixmap.isNull()) {
            noteSize(url, it->pixmap.size());
            return it->pixmap.size();
        }
        if (it->inFlight)
            return {}; // download pending; loaded() will bring the size
    }
    if (isFailed(url))
        return {};

    if (_diskLoad) {
        const auto bytes = _diskLoad(url);
        if (!bytes.isEmpty()) {
            if (const QSize sz = intrinsicSize(bytes); !sz.isEmpty()) {
                noteSize(url, sz);
                return sz;
            }
            // Header gave us nothing (SVG, or a format without size metadata).
            // Decode once to learn the size — recorded above, so this cannot
            // repeat on later layout passes. The pixmap is deliberately not
            // cached here: sizing must stay free of memory-cap pressure.
            if (const QPixmap px = ImageCache::decodeBounded(bytes); !px.isNull()) {
                noteSize(url, px.size());
                return px.size();
            }
            // Junk on disk — fall through and refetch.
        }
    }

    if (it == _cache.constEnd())
        startFetch(url);
    return {};
}

QPixmap ImageCache::get(const QString &url) {
    if (url.isEmpty())
        return {};

    auto it = _cache.find(url);
    if (it != _cache.end()) {
        it->lastUsed = ++_useTick; // a hit is a use — keep hot entries resident
        return it->pixmap;         // null while in-flight, real pixmap when done
    }
    if (isFailed(url))
        return {};

    // Check disk before going to the network. The decode is asynchronous (see
    // decodeAsync); the caller gets null now and loaded() shortly after, exactly
    // as for a download. In synchronous mode the pixmap is ready on return.
    if (_diskLoad) {
        const auto bytes = _diskLoad(url);
        if (!bytes.isEmpty()) {
            decodeAsync(url, bytes, /*saveToDisk=*/false);
            const auto e = _cache.constFind(url);
            return e != _cache.constEnd() ? e->pixmap : QPixmap();
        }
    }

    // First time seeing this URL — insert sentinel and start download.
    startFetch(url);

    return {}; // null — caller will be notified via loaded()
}
