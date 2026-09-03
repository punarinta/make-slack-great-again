// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <cstdint>
#include <functional>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSize>

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

    // Intrinsic (unscaled) pixel size of url's image, or an invalid QSize while
    // it is unknown. Starts a download on first call exactly like get(), so a
    // caller that only needs geometry never has to ask for the pixels.
    //
    // Layout MUST use this rather than get().size(): rebuildLayout() measures
    // every row in the conversation, so sizing through get() required the
    // decoded pixmap of every image in the channel to be resident at once.
    // Past the memory cap that turned into an eviction treadmill — each pass
    // evicted the entry the next pass asked for first, so every layout re-read
    // and re-decoded the whole image cache (~21 MB/s of PNG decoding on a
    // preview-heavy channel). Sizes are a few bytes and never evicted, and are
    // read from the image header without decoding where the format allows.
    QSize sizeOf(const QString &url);

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

    // Issue the network fetch for url and install the in-flight sentinel.
    // Shared by get() and sizeOf() so both enter the cache the same way.
    void startFetch(const QString &url);

    // Record url's intrinsic size once known (idempotent).
    void noteSize(const QString &url, const QSize &sz);

    // True while url is on the do-not-fetch list: bytes that arrived but could
    // not be decoded (permanent — they are not an image), or a transport error
    // still inside its retry cooldown.
    [[nodiscard]] bool isFailed(const QString &url) const;
    // permanent=false applies kErrorCooldownMs before another attempt is allowed.
    void               markFailed(const QString &url, bool permanent);

    // Recompute url's memory cost, mark it most-recently-used, then evict down
    // to the cap. Call after any change to an entry's pixmap/animatedBytes.
    void account(const QString &url);
    // Drop LRU entries until under the cap, skipping pinned ones (in-flight, or
    // backing a live QMovie that callers may still hold a pointer to).
    void evictIfNeeded(const QString &protectUrl);

    // Wait this long before retrying a url that failed with a transport error.
    static constexpr qint64 kErrorCooldownMs = 60'000;

    QHash<QString, Entry>  _cache;
    QList<QString>         _lru; // front = most recently used
    // Intrinsic sizes, and urls not to re-fetch. Both are keyed by url and hold
    // no pixels, so they are deliberately exempt from the memory cap and from
    // eviction: forgetting them is what produced repeated decodes and repeated
    // downloads of the same failing url on every layout pass.
    QHash<QString, QSize>  _sizes;
    QHash<QString, qint64> _failedUntil; // value 0 == never retry
    qint64                 _memBytes  = 0;
    qint64                 _memoryCap = kDefaultMemoryCap;
    QNetworkAccessManager *_nam;

    std::function<QByteArray(const QString &)>               _diskLoad;
    std::function<void(const QString &, const QByteArray &)> _diskSave;
};
