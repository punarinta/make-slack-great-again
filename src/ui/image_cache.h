// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <cstdint>
#include <functional>
#include <QHash>
#include <QList>
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
    // Each non-null return is an acquire: the entry is pinned against eviction
    // until the caller hands it back with releaseMovie(). A caller that stores
    // the pointer must release it exactly once when done (and drop the pointer).
    QMovie *movie(const QString &url);

    // Release one movie() acquisition. When the last holder releases, the
    // QMovie (and its decoded frames) is deleted and the entry becomes a
    // normal LRU citizen again — re-acquiring later recreates the player
    // from the retained bytes (or a fresh download if evicted meanwhile).
    void releaseMovie(const QString &url);

    // True when bytes decode to a multi-frame animation.
    static bool isAnimatedImage(const QByteArray &bytes);

    // Wire a persistent backing store: load is called before any network fetch;
    // save is called after each successful download so the bytes survive restarts.
    // Passing empty functions disables the backing store.
    void setDiskCache(
        std::function<QByteArray(const QString &)>               load,
        std::function<void(const QString &, const QByteArray &)> save
    );

    // Soft ceiling on the decoded pixels + animation bytes held in RAM. The map
    // used to grow unbounded for the whole process lifetime — every avatar,
    // preview and GIF ever scrolled past stayed resident. Once over the cap the
    // least-recently-used entries are dropped (see evictIfNeeded for what is
    // pinned). Default below; the setter exists for tests.
    static constexpr qint64 kDefaultMemoryCap = 64LL * 1024 * 1024;
    void                    setMemoryCap(qint64 bytes) { _memoryCap = bytes; }
    [[nodiscard]] qint64    memoryBytes() const { return _memBytes; }

signals:
    void loaded(const QString &url);

private:
    struct Entry {
        QPixmap    pixmap;              // first frame for animated images
        QByteArray animatedBytes;       // raw bytes, kept only for multi-frame images
        QMovie    *movie     = nullptr; // lazily created from animatedBytes
        int        movieRefs = 0;       // movie() acquisitions not yet released
        bool       inFlight  = false;
        qint64     cost      = 0; // last accounted bytes (pixmap + animatedBytes)
    };

    // Recompute url's memory cost, mark it most-recently-used, then evict down
    // to the cap. Call after any change to an entry's pixmap/animatedBytes.
    void account(const QString &url);
    // Drop LRU entries until under the cap, skipping pinned ones (in-flight, or
    // backing a live QMovie that callers may still hold a pointer to).
    void evictIfNeeded(const QString &protectUrl);

    QHash<QString, Entry>  _cache;
    QList<QString>         _lru; // front = most recently used
    qint64                 _memBytes  = 0;
    qint64                 _memoryCap = kDefaultMemoryCap;
    QNetworkAccessManager *_nam;

    std::function<QByteArray(const QString &)>               _diskLoad;
    std::function<void(const QString &, const QByteArray &)> _diskSave;
};
