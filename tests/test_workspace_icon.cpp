// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Custom (local) workspace icons:
//   - CustomWorkspaceIcon::prepare: centre square crop, bounded downscale
//   - install/remove: file under the app data dir, TokenStore path, old file gone
//   - TokenStore::displayIconUrl precedence and fallback to the server icon
//   - WorkspaceIconDialog result contract (loadFile / loadImage)

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include "auth/token_store.h"
#include "ui/workspace_icon_dialog/workspace_icon_dialog.h"
#include "util/custom_workspace_icon.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-workspace-icon");
    app.setOrganizationName("msga-test");
    // Never touch the real credentials file or the real app data dir.
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QStandardPaths::setTestModeEnabled(true);
    return msga_test::runCatch(argc, argv);
}

namespace {

struct Fixture {
    Fixture() { reset(); }
    ~Fixture() { reset(); }
    static void reset() {
        QSettings s("msga", "msga");
        s.clear();
        s.sync();
        QDir(CustomWorkspaceIcon::directory()).removeRecursively();
    }
};

WorkspaceKey key(const QString &id = "T0000TEST") {
    return WorkspaceKey{Service{QStringLiteral("slack")}, id};
}

// w×h image: left half red, right half blue — the crop's centre is visible.
QImage splitImage(int w, int h) {
    QImage   img(w, h, QImage::Format_ARGB32);
    QPainter p(&img);
    p.fillRect(0, 0, w / 2, h, Qt::red);
    p.fillRect(w / 2, 0, w - w / 2, h, Qt::blue);
    return img;
}

} // namespace

// ── prepare ───────────────────────────────────────────────────────────────────

TEST_CASE("prepare centre-crops a landscape image to a square", "[wsicon][prepare]") {
    const QImage out = CustomWorkspaceIcon::prepare(splitImage(300, 100));
    REQUIRE(out.size() == QSize(100, 100));
    // Columns 100..199 of the source: the red/blue seam sits in the middle.
    CHECK(out.pixelColor(10, 50) == QColor(Qt::red));
    CHECK(out.pixelColor(90, 50) == QColor(Qt::blue));
}

TEST_CASE("prepare centre-crops a portrait image to a square", "[wsicon][prepare]") {
    QImage src(100, 300, QImage::Format_ARGB32);
    src.fill(Qt::green);
    src.setPixelColor(50, 150, Qt::black); // dead centre survives the crop
    const QImage out = CustomWorkspaceIcon::prepare(src);
    REQUIRE(out.size() == QSize(100, 100));
    CHECK(out.pixelColor(50, 50) == QColor(Qt::black));
}

TEST_CASE("prepare bounds a large image but never upscales", "[wsicon][prepare]") {
    QImage big(1000, 1000, QImage::Format_RGB32);
    big.fill(Qt::white);
    CHECK(
        CustomWorkspaceIcon::prepare(big).size() ==
        QSize(CustomWorkspaceIcon::kStoredSize, CustomWorkspaceIcon::kStoredSize)
    );
    QImage small(50, 50, QImage::Format_RGB32);
    small.fill(Qt::white);
    CHECK(CustomWorkspaceIcon::prepare(small).size() == QSize(50, 50));
    CHECK(CustomWorkspaceIcon::prepare(QImage()).isNull());
}

// ── install / remove / displayIconUrl ─────────────────────────────────────────

TEST_CASE_METHOD(Fixture, "install stores a PNG and points TokenStore at it", "[wsicon][store]") {
    TokenStore::WorkspaceRecord rec;
    rec.key         = key();
    rec.displayName = "Test";
    rec.iconUrl     = "https://example.com/team.png";
    TokenStore::saveWorkspace(rec);
    CHECK(TokenStore::displayIconUrl(rec) == rec.iconUrl); // no override yet

    const QString path = CustomWorkspaceIcon::install(key(), splitImage(64, 64));
    REQUIRE_FALSE(path.isEmpty());
    CHECK(QFile::exists(path));
    CHECK(path.startsWith(CustomWorkspaceIcon::directory()));
    CHECK(path.endsWith(".png"));
    CHECK_FALSE(QFileInfo(path).fileName().contains(':')); // handle's ':' sanitised
    CHECK(TokenStore::customWorkspaceIconPath(key()) == path);
    CHECK(TokenStore::displayIconUrl(rec) == QUrl::fromLocalFile(path).toString());

    const QImage stored(path);
    CHECK(stored.size() == QSize(64, 64));
}

TEST_CASE_METHOD(
    Fixture, "re-install writes a new file name and drops the old file", "[wsicon][store]"
) {
    const QString first = CustomWorkspaceIcon::install(key(), splitImage(64, 64));
    REQUIRE_FALSE(first.isEmpty());
    const QString second = CustomWorkspaceIcon::install(key(), splitImage(32, 32));
    REQUIRE_FALSE(second.isEmpty());
    CHECK(first != second); // the file:// url must change so caches refetch
    CHECK_FALSE(QFile::exists(first));
    CHECK(QFile::exists(second));
    CHECK(TokenStore::customWorkspaceIconPath(key()) == second);
}

TEST_CASE_METHOD(Fixture, "remove clears the setting and deletes the file", "[wsicon][store]") {
    const QString path = CustomWorkspaceIcon::install(key(), splitImage(64, 64));
    REQUIRE(QFile::exists(path));
    CustomWorkspaceIcon::remove(key());
    CHECK(TokenStore::customWorkspaceIconPath(key()).isEmpty());
    CHECK_FALSE(QFile::exists(path));
    CustomWorkspaceIcon::remove(key()); // idempotent
}

TEST_CASE_METHOD(
    Fixture, "displayIconUrl falls back to the server icon when the file is gone", "[wsicon][store]"
) {
    TokenStore::WorkspaceRecord rec;
    rec.key            = key();
    rec.iconUrl        = "https://example.com/team.png";
    const QString path = CustomWorkspaceIcon::install(key(), splitImage(64, 64));
    REQUIRE(QFile::remove(path));
    CHECK(TokenStore::displayIconUrl(rec) == rec.iconUrl);
}

TEST_CASE_METHOD(Fixture, "install rejects a null image and changes nothing", "[wsicon][store]") {
    CHECK(CustomWorkspaceIcon::install(key(), QImage()).isEmpty());
    CHECK(TokenStore::customWorkspaceIconPath(key()).isEmpty());
}

TEST_CASE_METHOD(
    Fixture, "the override survives a re-login that rewrites the record", "[wsicon][store]"
) {
    const QString               path = CustomWorkspaceIcon::install(key(), splitImage(64, 64));
    TokenStore::WorkspaceRecord rec;
    rec.key     = key();
    rec.iconUrl = "https://example.com/new-server-icon.png";
    TokenStore::saveWorkspace(rec); // what slack_auth does on every sign-in
    CHECK(TokenStore::customWorkspaceIconPath(key()) == path);
    CHECK(TokenStore::displayIconUrl(rec) == QUrl::fromLocalFile(path).toString());
}

// ── dialog ────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(Fixture, "dialog reports the picked picture", "[wsicon][dialog]") {
    QWidget host;
    host.resize(800, 600);
    WorkspaceIconDialog dlg("slack:T0000TEST", "Test", QPixmap(), /*hasCustom=*/false, &host);
    CHECK(dlg.chosenImage().isNull());
    CHECK_FALSE(dlg.resetRequested());

    QTemporaryDir dir;
    const QString png = dir.path() + "/pic.png";
    REQUIRE(splitImage(300, 100).save(png));
    CHECK(dlg.loadFile(png));
    CHECK_FALSE(dlg.chosenImage().isNull());
    CHECK_FALSE(dlg.resetRequested());

    const QString junk = dir.path() + "/junk.png";
    {
        QFile f(junk);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not an image");
    }
    CHECK_FALSE(dlg.loadFile(junk));
    CHECK_FALSE(dlg.loadFile(dir.path() + "/missing.png"));
    CHECK_FALSE(dlg.chosenImage().isNull()); // a bad file leaves the good pick alone
    CHECK_FALSE(dlg.loadImage(QImage()));
}
