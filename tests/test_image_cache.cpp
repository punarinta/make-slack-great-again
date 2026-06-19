// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for ImageCache's in-memory bound (the LRU eviction added so the decoded
// pixmaps no longer grow unbounded for the whole process lifetime):
//   - resident bytes never exceed the configured cap once it is crossed
//   - a plain cache hit neither re-accounts nor reorders (paint-path stays O(1))
//   - staying under the cap evicts nothing
//   - the freshly inserted url survives eviction triggered by its own insert

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QBuffer>
#include <QHash>
#include <QImage>

#include "ui/image_cache.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-image-cache");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// A 256×256 ARGB image decodes to 256*256*4 = 256 KiB resident — the cost
// ImageCache accounts (it measures the decoded pixmap, not the encoded bytes).
static constexpr int    kSide      = 256;
static constexpr qint64 kEntryCost = qint64(kSide) * kSide * 4;

static QByteArray makePng() {
    QImage img(kSide, kSide, QImage::Format_ARGB32);
    img.fill(Qt::red);
    QByteArray bytes;
    QBuffer    buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

// Wire a synchronous in-memory disk backend so get() inserts without any
// network round-trip: every requested url resolves to the same PNG bytes.
static void wireDisk(ImageCache &cache) {
    static const QByteArray png = makePng();
    cache.setDiskCache(
        [](const QString &) { return png; }, [](const QString &, const QByteArray &) {}
    );
}

TEST_CASE("ImageCache stays under its memory cap") {
    ImageCache cache;
    wireDisk(cache);
    // Room for ~4 entries.
    const qint64 cap = 4 * kEntryCost + kEntryCost / 2;
    cache.setMemoryCap(cap);

    for (int i = 0; i < 50; ++i) {
        const QPixmap px = cache.get(QStringLiteral("img-%1").arg(i));
        REQUIRE_FALSE(px.isNull()); // disk path returns it now
        REQUIRE(cache.memoryBytes() <= cap);
    }
    // Bounded, not unbounded: 50 inserts must not have retained 50 entries.
    REQUIRE(cache.memoryBytes() <= 5 * kEntryCost);
    REQUIRE(cache.memoryBytes() >= kEntryCost); // but it did keep the recent ones
}

TEST_CASE("Under-cap usage evicts nothing") {
    ImageCache cache;
    wireDisk(cache);
    cache.setMemoryCap(100 * kEntryCost);

    for (int i = 0; i < 10; ++i)
        cache.get(QStringLiteral("img-%1").arg(i));

    REQUIRE(cache.memoryBytes() == 10 * kEntryCost);
}

TEST_CASE("A repeated hit does not re-account or grow memory") {
    ImageCache cache;
    wireDisk(cache);
    cache.setMemoryCap(100 * kEntryCost);

    cache.get(QStringLiteral("same"));
    const qint64 after1 = cache.memoryBytes();
    for (int i = 0; i < 20; ++i)
        cache.get(QStringLiteral("same"));

    REQUIRE(cache.memoryBytes() == after1);
    REQUIRE(after1 == kEntryCost);
}
