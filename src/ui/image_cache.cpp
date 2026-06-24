// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "image_cache.h"

#include "network/shared_nam.h"

#include <QBuffer>
#include <QImageReader>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrl>

ImageCache::ImageCache(QObject *parent) : QObject(parent), _nam(net::sharedNam()) {}

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
    it->cost = fresh;

    _lru.removeOne(url);
    _lru.prepend(url); // most recently used

    evictIfNeeded(url);
}

void ImageCache::evictIfNeeded(const QString &protectUrl) {
    while (_memBytes > _memoryCap) {
        // Walk from the least-recently-used end for the first entry we may drop.
        int victim = -1;
        for (int i = _lru.size() - 1; i >= 0; --i) {
            const QString &u = _lru.at(i);
            if (u == protectUrl)
                continue;
            auto it = _cache.find(u);
            // A live QMovie is handed out by pointer and cached by callers
            // (MessageListWidget::_gifMovies) with a frameChanged connection —
            // deleting it here would dangle. In-flight sentinels must survive so
            // their finished handler can complete. Both are pinned.
            if (it == _cache.end() || it->inFlight || it->movie)
                continue;
            victim = i;
            break;
        }
        if (victim < 0)
            break; // everything left is pinned — cap is a soft target

        const QString u  = _lru.takeAt(victim);
        auto          it = _cache.find(u);
        if (it != _cache.end()) {
            _memBytes -= it->cost;
            _cache.erase(it);
        }
    }
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
        it->movie = m;
    }
    return it->movie;
}

void ImageCache::setDiskCache(
    std::function<QByteArray(const QString &)>               load,
    std::function<void(const QString &, const QByteArray &)> save
) {
    _diskLoad = std::move(load);
    _diskSave = std::move(save);
}

// Decode downloaded bytes to a pixmap. Falls back to explicit SVG rendering for
// formats QImage can't decode itself (notably BIMI brand-logo SVGs).
static QPixmap pixmapFromData(const QByteArray &bytes) {
    QPixmap px;
    if (px.loadFromData(bytes) && !px.isNull())
        return px;
    QSvgRenderer r(bytes);
    if (r.isValid()) {
        QSize sz = r.defaultSize();
        if (sz.isEmpty())
            sz = QSize(128, 128);
        const int kMax = 256;
        if (sz.width() > kMax || sz.height() > kMax)
            sz.scale(kMax, kMax, Qt::KeepAspectRatio);
        QPixmap out(sz);
        out.fill(Qt::transparent);
        QPainter p(&out);
        r.render(&p);
        return out;
    }
    return {};
}

QPixmap ImageCache::get(const QString &url) {
    if (url.isEmpty())
        return {};

    auto it = _cache.find(url);
    if (it != _cache.end()) {
        return it->pixmap; // null while in-flight, real pixmap when done
    }

    // Check disk before going to the network.
    if (_diskLoad) {
        const auto bytes = _diskLoad(url);
        if (!bytes.isEmpty()) {
            QPixmap px = pixmapFromData(bytes);
            if (!px.isNull()) {
                auto &entry  = _cache[url];
                entry.pixmap = px;
                if (isAnimatedImage(bytes))
                    entry.animatedBytes = bytes;
                account(url);
                return px;
            }
        }
    }

    // First time seeing this URL — insert sentinel and start download.
    auto &entry    = _cache[url];
    entry.inFlight = true;

    auto *reply = _nam->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        reply->deleteLater();
        auto &e    = _cache[url];
        e.inFlight = false;
        if (reply->error() == QNetworkReply::NoError) {
            const auto bytes = reply->readAll();
            QPixmap    px    = pixmapFromData(bytes);
            if (!px.isNull()) {
                e.pixmap = px;
                if (isAnimatedImage(bytes))
                    e.animatedBytes = bytes;
                if (_diskSave)
                    _diskSave(url, bytes);
            }
        }
        account(url); // no-op cost change if the fetch yielded nothing
        emit loaded(url);
    });

    return {}; // null — caller will be notified via loaded()
}
