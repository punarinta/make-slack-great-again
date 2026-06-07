// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "image_cache.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

ImageCache::ImageCache(QObject *parent)
    : QObject(parent)
    , _nam(new QNetworkAccessManager(this))
{}

void ImageCache::setDiskCache(
    std::function<QByteArray(const QString &)>               load,
    std::function<void(const QString &, const QByteArray &)> save)
{
    _diskLoad = std::move(load);
    _diskSave = std::move(save);
}

QPixmap ImageCache::get(const QString &url) {
    if (url.isEmpty()) return {};

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
                _cache[url].pixmap = px;
                return px;
            }
        }
    }

    // First time seeing this URL — insert sentinel and start download.
    auto &entry = _cache[url];
    entry.inFlight = true;

    auto *reply = _nam->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        reply->deleteLater();
        auto &e = _cache[url];
        e.inFlight = false;
        if (reply->error() == QNetworkReply::NoError) {
            const auto bytes = reply->readAll();
            QPixmap px;
            if (px.loadFromData(bytes) && !px.isNull()) {
                e.pixmap = px;
                if (_diskSave) _diskSave(url, bytes);
            }
        }
        emit loaded(url);
    });

    return {}; // null — caller will be notified via loaded()
}
