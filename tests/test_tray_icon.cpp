// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Custom tray icon (GitHub issue #73):
//   - CustomTrayIcon::decode / prepare / toMonochrome: SVG at full size, fit
//     (not crop) into the square, silhouette from alpha or from the backdrop
//   - install / enabled / current / remove: the file, the switch, the cache
//   - TrayIconDialog result contract (picture, monochrome-only change, reset)

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <utility>

#include "ui/tray_icon_dialog/tray_icon_dialog.h"
#include "util/custom_tray_icon.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-tray-icon");
    app.setOrganizationName("msga-test");
    // Never touch the real settings file or the real app data dir.
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QStandardPaths::setTestModeEnabled(true);
    return msga_test::runCatch(argc, argv);
}

namespace {

constexpr int kSide = CustomTrayIcon::kStoredSize;

struct Fixture {
    Fixture() { reset(); }
    ~Fixture() { reset(); }
    static void reset() {
        CustomTrayIcon::remove(); // also drops the current() cache
        QSettings s("msga", "msga");
        s.clear();
        s.sync();
        CustomTrayIcon::setEnabled(false);
    }
};

QImage solid(int w, int h, const QColor &c) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(c);
    return img;
}

} // namespace

// ── pure helpers ──────────────────────────────────────────────────────────────

TEST_CASE("prepare fits a wide picture into a transparent square", "[tray][prepare]") {
    const QImage out = CustomTrayIcon::prepare(solid(200, 100, Qt::red));
    REQUIRE(out.size() == QSize(kSide, kSide));
    // Letterboxed, not cropped: the band above and below stays transparent.
    CHECK(qAlpha(out.pixel(kSide / 2, 2)) == 0);
    CHECK(qAlpha(out.pixel(kSide / 2, kSide - 3)) == 0);
    CHECK(out.pixel(kSide / 2, kSide / 2) == QColor(Qt::red).rgba());
    CHECK(out.pixel(1, kSide / 2) == QColor(Qt::red).rgba()); // full width kept
    CHECK(CustomTrayIcon::prepare(QImage()).isNull());
}

TEST_CASE("prepare upscales a tiny picture to the stored size", "[tray][prepare]") {
    CHECK(CustomTrayIcon::prepare(solid(16, 16, Qt::blue)).size() == QSize(kSide, kSide));
}

TEST_CASE("monochrome of a transparent logo follows its alpha", "[tray][mono]") {
    QImage src = solid(4, 1, QColor(255, 0, 0, 255));
    src.setPixel(1, 0, qRgba(0, 0, 255, 0));
    src.setPixel(2, 0, qRgba(0, 200, 0, 128));
    const QImage m = CustomTrayIcon::toMonochrome(src);
    for (int x = 0; x < 4; ++x) {
        const QRgb px = m.pixel(x, 0);
        CHECK(qRed(px) == 255);
        CHECK(qGreen(px) == 255);
        CHECK(qBlue(px) == 255);
        CHECK(qAlpha(px) == qAlpha(src.pixel(x, 0)));
    }
    CHECK(CustomTrayIcon::toMonochrome(QImage()).isNull());
}

TEST_CASE("monochrome of an opaque logo keys on the backdrop", "[tray][mono]") {
    // Dark mark on a light backdrop, and the reverse: both give the mark.
    for (const auto &[bg, fg] :
         {std::pair{QColor(Qt::white), QColor(Qt::black)},
          std::pair{QColor(Qt::black), QColor(Qt::white)}}) {
        QImage   src = solid(40, 40, bg);
        QPainter p(&src);
        p.fillRect(10, 10, 20, 20, fg);
        p.end();
        // Through prepare(): the letterbox padding must not make it "translucent".
        const QImage m = CustomTrayIcon::styled(src, true);
        CHECK(qAlpha(m.pixel(kSide / 2, kSide / 2)) == 255); // the mark
        CHECK(qAlpha(m.pixel(4, 4)) == 0);                   // the backdrop
        CHECK(qRed(m.pixel(kSide / 2, kSide / 2)) == 255);   // white ink
    }
}

TEST_CASE("monochrome makes every colour of a logo solid", "[tray][mono]") {
    // Orange is much lighter than navy; both must be solid mark. Red on a green
    // backdrop of similar brightness must still stand out.
    QImage   src = solid(60, 30, QColor(255, 255, 255));
    QPainter p(&src);
    p.fillRect(5, 5, 20, 20, QColor(255, 140, 0));
    p.fillRect(35, 5, 20, 20, QColor(20, 60, 160));
    p.end();
    const QImage m = CustomTrayIcon::toMonochrome(src);
    CHECK(qAlpha(m.pixel(15, 15)) == 255);
    CHECK(qAlpha(m.pixel(45, 15)) == 255);
    CHECK(qAlpha(m.pixel(30, 28)) == 0);

    QImage   rg = solid(30, 30, QColor(0, 150, 0));
    QPainter q(&rg);
    q.fillRect(10, 10, 10, 10, QColor(200, 30, 30));
    q.end();
    const QImage n = CustomTrayIcon::toMonochrome(rg);
    CHECK(qAlpha(n.pixel(15, 15)) == 255);
    CHECK(qAlpha(n.pixel(2, 2)) == 0);
}

TEST_CASE("monochrome of a flat picture is the whole square", "[tray][mono]") {
    const QImage m = CustomTrayIcon::styled(solid(64, 64, Qt::red), true);
    CHECK(qAlpha(m.pixel(kSide / 2, kSide / 2)) == 255);
    CHECK(qAlpha(m.pixel(1, 1)) == 255);
}

TEST_CASE("monochrome treats a letterboxed opaque picture as opaque", "[tray][mono]") {
    QImage   src = solid(80, 40, Qt::white); // wide: prepare() pads top and bottom
    QPainter p(&src);
    p.fillRect(30, 10, 20, 20, Qt::black);
    p.end();
    const QImage m = CustomTrayIcon::styled(src, true);
    CHECK(qAlpha(m.pixel(kSide / 2, kSide / 2)) == 255); // the mark
    CHECK(qAlpha(m.pixel(4, kSide / 2)) == 0);           // white backdrop keyed out
    CHECK(qAlpha(m.pixel(kSide / 2, 2)) == 0);           // padding stays clear
}

TEST_CASE("decode renders a small SVG at the stored size", "[tray][decode]") {
    const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'>"
                           "<rect width='24' height='24' fill='#ff0000'/></svg>";
    const QImage     img = CustomTrayIcon::decode(svg);
    CHECK(img.size() == QSize(kSide, kSide));
}

TEST_CASE("decode reads a raster and rejects junk", "[tray][decode]") {
    QByteArray png;
    QBuffer    buf(&png);
    buf.open(QIODevice::WriteOnly);
    REQUIRE(solid(40, 20, Qt::green).save(&buf, "PNG"));
    CHECK(CustomTrayIcon::decode(png).size() == QSize(40, 20));
    CHECK(CustomTrayIcon::decode("not an image").isNull());
}

// ── storage ───────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(Fixture, "install, switch and current", "[tray][store]") {
    CHECK_FALSE(CustomTrayIcon::hasImage());
    CHECK(CustomTrayIcon::current().isNull());

    REQUIRE(CustomTrayIcon::install(solid(64, 64, Qt::red)));
    CHECK(CustomTrayIcon::hasImage());
    CHECK(CustomTrayIcon::stored().size() == QSize(kSide, kSide));
    CHECK(CustomTrayIcon::current().isNull()); // installed but switched off

    CustomTrayIcon::setEnabled(true);
    CustomTrayIcon::setMonochrome(false);
    CHECK(CustomTrayIcon::current().pixel(kSide / 2, kSide / 2) == QColor(Qt::red).rgba());

    CustomTrayIcon::setMonochrome(true); // the cache follows the option
    const QRgb g = CustomTrayIcon::current().pixel(kSide / 2, kSide / 2);
    CHECK(g == qRgba(255, 255, 255, qAlpha(g)));

    CustomTrayIcon::setEnabled(false); // the picture outlives the switch
    CHECK(CustomTrayIcon::current().isNull());
    CHECK(CustomTrayIcon::hasImage());
}

TEST_CASE_METHOD(Fixture, "a new install replaces the picture", "[tray][store]") {
    CustomTrayIcon::setEnabled(true);
    CustomTrayIcon::setMonochrome(false);
    REQUIRE(CustomTrayIcon::install(solid(64, 64, Qt::red)));
    CHECK(CustomTrayIcon::current().pixel(kSide / 2, kSide / 2) == QColor(Qt::red).rgba());
    REQUIRE(CustomTrayIcon::install(solid(64, 64, Qt::blue)));
    CHECK(CustomTrayIcon::current().pixel(kSide / 2, kSide / 2) == QColor(Qt::blue).rgba());
}

TEST_CASE_METHOD(Fixture, "remove deletes the file and switches off", "[tray][store]") {
    REQUIRE(CustomTrayIcon::install(solid(64, 64, Qt::red)));
    CustomTrayIcon::setEnabled(true);
    CustomTrayIcon::remove();
    CHECK_FALSE(CustomTrayIcon::hasImage());
    CHECK_FALSE(CustomTrayIcon::enabled());
    CHECK(CustomTrayIcon::current().isNull());
    CustomTrayIcon::remove(); // idempotent
    CHECK_FALSE(CustomTrayIcon::install(QImage()));
    CHECK_FALSE(CustomTrayIcon::hasImage());
}

// ── dialog ────────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(Fixture, "dialog reports the picked picture", "[tray][dialog]") {
    QWidget host;
    host.resize(800, 600);
    TrayIconDialog dlg(&host);
    CHECK(dlg.chosenImage().isNull());
    CHECK_FALSE(dlg.resetRequested());
    CHECK(dlg.monochrome() == CustomTrayIcon::monochrome());

    QTemporaryDir dir;
    const QString png = dir.path() + "/pic.png";
    REQUIRE(solid(300, 100, Qt::red).save(png));
    CHECK(dlg.loadFile(png));
    CHECK_FALSE(dlg.chosenImage().isNull());

    const QString junk = dir.path() + "/junk.png";
    {
        QFile f(junk);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not an image");
    }
    CHECK_FALSE(dlg.loadFile(junk));
    CHECK_FALSE(dlg.chosenImage().isNull()); // a bad file leaves the good pick alone
}

TEST_CASE_METHOD(Fixture, "dialog starts from the stored options", "[tray][dialog]") {
    CustomTrayIcon::setMonochrome(false);
    REQUIRE(CustomTrayIcon::install(solid(64, 64, Qt::red)));
    QWidget host;
    host.resize(800, 600);
    TrayIconDialog dlg(&host);
    CHECK_FALSE(dlg.monochrome());
    CHECK(dlg.chosenImage().isNull()); // the stored picture is not a new pick
}
