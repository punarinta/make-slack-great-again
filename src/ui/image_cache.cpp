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

QPixmap ImageCache::get(const QString &url) {
    if (url.isEmpty()) return {};

    auto it = _cache.find(url);
    if (it != _cache.end()) {
        return it->pixmap; // null while in-flight, real pixmap when done
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
            QPixmap px;
            if (px.loadFromData(reply->readAll()) && !px.isNull())
                e.pixmap = px;
        }
        emit loaded(url);
    });

    return {}; // null — caller will be notified via loaded()
}
