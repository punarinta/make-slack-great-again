// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "cache/cache_evictor.h"

namespace {

void writeBlob(const QString &path, int size) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(size, 'x'));
}

void setMtime(const QString &path, const QDateTime &when) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadWrite));
    REQUIRE(f.setFileTime(when, QFileDevice::FileModificationTime));
}

// root/T1/images/{a,b,c} + root/T2/images/d at 1000 bytes each,
// root/T1/users.json at 100 bytes. mtime order: a < d < b < c.
struct SweepFixture {
    QTemporaryDir tmp;
    QString       root = tmp.path() + "/cache";

    QString a    = root + "/T1/images/a";
    QString b    = root + "/T1/images/b";
    QString c    = root + "/T1/images/c";
    QString d    = root + "/T2/images/d";
    QString json = root + "/T1/users.json";

    SweepFixture() {
        const auto now = QDateTime::currentDateTimeUtc();
        writeBlob(a, 1000);
        writeBlob(b, 1000);
        writeBlob(c, 1000);
        writeBlob(d, 1000);
        writeBlob(json, 100);
        setMtime(a, now.addSecs(-4000));
        setMtime(d, now.addSecs(-3000));
        setMtime(b, now.addSecs(-2000));
        setMtime(c, now.addSecs(-1000));
    }
};

} // namespace

TEST_CASE_METHOD(SweepFixture, "sweep is a no-op under the cap", "[evictor]") {
    CHECK(CacheEvictor::sweep(root, 10000) == 0);
    CHECK(QFile::exists(a));
    CHECK(QFile::exists(d));
}

TEST_CASE_METHOD(
    SweepFixture, "sweep deletes least recently used first, across workspaces", "[evictor]"
) {
    // Total 4100; cap 2500 → the two oldest blobs (a, then d) must go.
    CHECK(CacheEvictor::sweep(root, 2500) == 2000);
    CHECK_FALSE(QFile::exists(a));
    CHECK_FALSE(QFile::exists(d));
    CHECK(QFile::exists(b));
    CHECK(QFile::exists(c));
    CHECK(QFile::exists(json));
}

TEST_CASE_METHOD(SweepFixture, "sweep never deletes structural JSON", "[evictor]") {
    // Cap below the JSON size alone: every image goes, the JSON stays.
    CHECK(CacheEvictor::sweep(root, 50) == 4000);
    CHECK_FALSE(QFile::exists(a));
    CHECK_FALSE(QFile::exists(b));
    CHECK_FALSE(QFile::exists(c));
    CHECK_FALSE(QFile::exists(d));
    CHECK(QFile::exists(json));
}
