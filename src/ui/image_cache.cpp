// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "image_cache.h"

#include <QBuffer>
#include <QImageReader>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

ImageCache::ImageCache(QObject *parent) : QObject(parent), _nam(new QNetworkAccessManager(this)) {}

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
            QPixmap px;
            if (px.loadFromData(bytes) && !px.isNull()) {
                auto &entry  = _cache[url];
                entry.pixmap = px;
                if (isAnimatedImage(bytes))
                    entry.animatedBytes = bytes;
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
            QPixmap    px;
            if (px.loadFromData(bytes) && !px.isNull()) {
                e.pixmap = px;
                if (isAnimatedImage(bytes))
                    e.animatedBytes = bytes;
                if (_diskSave)
                    _diskSave(url, bytes);
            }
        }
        emit loaded(url);
    });

    return {}; // null — caller will be notified via loaded()
}
