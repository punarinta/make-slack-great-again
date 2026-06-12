// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Background LRU sweeper for the on-disk cache (AppData/cache, all workspaces).
// "Last used" is a blob's file mtime: WorkspaceCache::saveImage stamps it on
// write and loadImage bumps it on read, so eviction order is least-recently-
// VIEWED, not least-recently-downloaded. Only image blobs are evicted; the
// structural JSON files (conversations, users, messages, meta) are small and
// always kept.
#pragma once

#include <QObject>
#include <atomic>

class CacheEvictor : public QObject {
    Q_OBJECT
public:
    static CacheEvictor *instance();

    static constexpr int kDefaultCapMb = 250;

    // Cache size cap in MB (QSettings "storage/cacheCapMb").
    static int  capMb();
    static void setCapMb(int mb);

    // Kick off an async sweep on a worker thread. Calls made while a sweep is
    // running coalesce into one re-run, so this is safe to call freely.
    void schedule();

    // O(1) write-side trigger: callers report bytes added to the cache and a
    // sweep is scheduled once enough accumulates. Keeps a download burst from
    // overshooting the cap until the next periodic sweep, without paying a
    // directory scan per write.
    static void noteBytesWritten(qint64 bytes);

    // Synchronous sweep core (also used directly by tests): while the total
    // size of all files under cacheRoot exceeds capBytes, delete files living
    // in an "images" directory, oldest mtime first. Returns bytes freed.
    static qint64 sweep(const QString &cacheRoot, qint64 capBytes);

signals:
    // Emitted on the main thread after an async sweep completes.
    void finished();

private:
    CacheEvictor() = default;

    std::atomic_bool _running{false};
    std::atomic_bool _again{false};
};
