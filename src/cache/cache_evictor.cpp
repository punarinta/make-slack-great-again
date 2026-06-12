// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cache_evictor.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <thread>
#include <vector>

CacheEvictor *CacheEvictor::instance() {
    // Intentionally leaked: a worker thread may still be queuing the finished()
    // event during static destruction, so the object must outlive main().
    static auto *inst = new CacheEvictor;
    return inst;
}

int CacheEvictor::capMb() {
    const int mb = QSettings("msga", "msga").value("storage/cacheCapMb").toInt();
    return mb > 0 ? mb : kDefaultCapMb;
}

void CacheEvictor::setCapMb(int mb) {
    QSettings("msga", "msga").setValue("storage/cacheCapMb", mb);
}

qint64 CacheEvictor::sweep(const QString &cacheRoot, qint64 capBytes) {
    struct Blob {
        QString   path;
        qint64    size;
        QDateTime used;
    };
    qint64            total = 0;
    std::vector<Blob> blobs;
    QDirIterator      it(cacheRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const auto fi = it.fileInfo();
        total += fi.size();
        if (fi.dir().dirName() == QLatin1String("images"))
            blobs.push_back({fi.filePath(), fi.size(), fi.lastModified()});
    }
    if (total <= capBytes)
        return 0;

    std::sort(blobs.begin(), blobs.end(), [](const Blob &a, const Blob &b) {
        return a.used < b.used;
    });

    qint64 freed = 0;
    for (const auto &b : blobs) {
        if (total <= capBytes)
            break;
        // remove() can fail if the file is open elsewhere (Windows); the blob
        // just stays for the next sweep.
        if (QFile::remove(b.path)) {
            total -= b.size;
            freed += b.size;
        }
    }
    return freed;
}

void CacheEvictor::noteBytesWritten(qint64 bytes) {
    constexpr qint64           kTrigger = 32 * 1024 * 1024;
    static std::atomic<qint64> accum{0};
    if (accum.fetch_add(bytes) + bytes >= kTrigger) {
        accum = 0;
        instance()->schedule();
    }
}

void CacheEvictor::schedule() {
    if (_running.exchange(true)) {
        _again = true;
        return;
    }
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache";
    std::thread([this, root] {
        do {
            _again = false;
            // Cap re-read each pass so a schedule() after setCapMb() applies
            // the new value even when it coalesced into this run.
            sweep(root, qint64(capMb()) * 1024 * 1024);
        } while (_again.exchange(false));
        _running = false;
        // A schedule() landing between the loop exit and the store above is
        // dropped; the periodic sweep picks it up, so no extra handshake.
        QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
    }).detach();
}
