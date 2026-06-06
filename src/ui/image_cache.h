// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>

class QNetworkAccessManager;

// Shared cache for public-URL image downloads: avatars, attachment previews, favicons.
// Deduplicates concurrent requests for the same URL via an in-flight sentinel.
// Emits loaded(url) when a download completes so callers can refresh their views.
//
// For auth-required Slack file downloads use Session::downloadFile() instead.
class ImageCache : public QObject {
    Q_OBJECT
public:
    explicit ImageCache(QObject *parent = nullptr);

    // Returns the cached pixmap for url (null QPixmap while loading or on error).
    // First call for a given url triggers a download; subsequent calls while the
    // download is in-flight return null immediately and wait for the loaded() signal.
    QPixmap get(const QString &url);

signals:
    void loaded(const QString &url);

private:
    struct Entry {
        QPixmap pixmap;
        bool    inFlight = false;
    };

    QHash<QString, Entry>  _cache;
    QNetworkAccessManager *_nam;
};
