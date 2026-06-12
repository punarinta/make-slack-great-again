// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <functional>
#include <QHash>
#include <QObject>
#include <QPixmap>

class QMovie;
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

    // Player for an animated image (GIF / animated WebP), or nullptr when the
    // url hasn't loaded yet or decodes to a single frame. The QMovie is created
    // lazily, shared between callers and owned by the cache — callers connect
    // to frameChanged() and drive start()/setPaused() by visibility.
    QMovie *movie(const QString &url);

    // True when bytes decode to a multi-frame animation.
    static bool isAnimatedImage(const QByteArray &bytes);

    // Wire a persistent backing store: load is called before any network fetch;
    // save is called after each successful download so the bytes survive restarts.
    // Passing empty functions disables the backing store.
    void setDiskCache(
        std::function<QByteArray(const QString &)>               load,
        std::function<void(const QString &, const QByteArray &)> save
    );

signals:
    void loaded(const QString &url);

private:
    struct Entry {
        QPixmap    pixmap;             // first frame for animated images
        QByteArray animatedBytes;      // raw bytes, kept only for multi-frame images
        QMovie    *movie    = nullptr; // lazily created from animatedBytes
        bool       inFlight = false;
    };

    QHash<QString, Entry>  _cache;
    QNetworkAccessManager *_nam;

    std::function<QByteArray(const QString &)>               _diskLoad;
    std::function<void(const QString &, const QByteArray &)> _diskSave;
};
