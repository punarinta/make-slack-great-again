// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "custom_tray_icon.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QSvgRenderer>

namespace CustomTrayIcon {

namespace {

constexpr char kEnabledKey[]    = "tray/customIcon";
constexpr char kMonochromeKey[] = "tray/customIconMonochrome";

// current()'s cache; every setter below drops it.
QImage g_current;
bool   g_currentValid = false;

// Strongest colour distance below which an opaque picture counts as flat.
constexpr int kFlatContrast = 24;

void invalidate() {
    g_current      = {};
    g_currentValid = false;
}

} // namespace

QString path() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/tray_icon.png");
}

QImage decode(const QByteArray &bytes) {
    // SVG first: a raster reader with the svg image plugin would render it at
    // its (often tiny) default size. A non-XML payload fails the parse at once.
    if (QSvgRenderer svg(bytes); svg.isValid()) {
        QSize sz = svg.defaultSize();
        if (sz.isEmpty())
            sz = QSize(kStoredSize, kStoredSize);
        sz.scale(kStoredSize, kStoredSize, Qt::KeepAspectRatio);
        QImage out(sz, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        svg.render(&p);
        return out;
    }
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    // Ask for a scaled decode when the source is larger than we keep (JPEG
    // downscales inside the decoder); prepare() does the exact fit.
    if (QSize natural = reader.size();
        natural.width() > kStoredSize * 2 || natural.height() > kStoredSize * 2) {
        natural.scale(kStoredSize * 2, kStoredSize * 2, Qt::KeepAspectRatio);
        reader.setScaledSize(natural);
    }
    return reader.read();
}

QImage prepare(const QImage &src) {
    if (src.isNull())
        return {};
    const QImage scaled =
        src.scaled(kStoredSize, kStoredSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage out(kStoredSize, kStoredSize, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawImage((kStoredSize - scaled.width()) / 2, (kStoredSize - scaled.height()) / 2, scaled);
    p.end();
    return out;
}

QImage toMonochrome(const QImage &src) {
    if (src.isNull())
        return {};
    QImage     out = src.convertToFormat(QImage::Format_ARGB32);
    const auto px  = [&out](int x, int y) { return reinterpret_cast<QRgb *>(out.scanLine(y))[x]; };

    // The visible area: prepare()'s letterbox padding is transparent, and an
    // opaque picture inside it must still count as opaque.
    int  x0 = out.width(), y0 = out.height(), x1 = -1, y1 = -1;
    bool translucent = false;
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x)
            if (qAlpha(px(x, y)) > 0) {
                x0 = qMin(x0, x);
                y0 = qMin(y0, y);
                x1 = qMax(x1, x);
                y1 = qMax(y1, y);
            }
    if (x1 < 0)
        return out; // fully transparent: nothing to shape
    for (int y = y0; y <= y1 && !translucent; ++y)
        for (int x = x0; x <= x1; ++x)
            if (qAlpha(px(x, y)) < 250) {
                translucent = true;
                break;
            }

    // Opaque: colour distance from the backdrop (the corners' mean), so a red
    // mark on a green backdrop of the same brightness still stands out. Anything
    // at half the strongest contrast or more is solid mark — a two-colour logo
    // must not come out half-transparent — and the rest fades, which keeps
    // anti-aliased edges soft and faint compression noise invisible.
    int        maxDist  = 1;
    QRgb       backdrop = 0;
    const auto dist     = [&backdrop](QRgb c) {
        return qMax(
            qMax(qAbs(qRed(c) - qRed(backdrop)), qAbs(qGreen(c) - qGreen(backdrop))),
            qAbs(qBlue(c) - qBlue(backdrop))
        );
    };
    if (!translucent) {
        const QRgb corners[] = {px(x0, y0), px(x1, y0), px(x0, y1), px(x1, y1)};
        int        r = 0, g = 0, b = 0;
        for (const QRgb c : corners) {
            r += qRed(c);
            g += qGreen(c);
            b += qBlue(c);
        }
        backdrop = qRgb(r / 4, g / 4, b / 4);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                maxDist = qMax(maxDist, dist(px(x, y)));
    }
    // A flat picture (one colour, give or take compression noise) has no mark
    // to key out: the whole square is the silhouette, not nothing.
    const bool flat = !translucent && maxDist < kFlatContrast;
    for (int y = 0; y < out.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int a = translucent || flat || qAlpha(line[x]) == 0
                              ? qAlpha(line[x])
                              : qMin(255, dist(line[x]) * 2 * 255 / maxDist);
            line[x]     = qRgba(255, 255, 255, a);
        }
    }
    return out;
}

QImage styled(const QImage &src, bool mono) {
    const QImage img = prepare(src);
    return mono ? toMonochrome(img) : img;
}

bool install(const QImage &src) {
    const QImage img = prepare(src);
    if (img.isNull())
        return false;
    const QString file = path();
    if (!QDir().mkpath(QFileInfo(file).path()))
        return false;
    // Write aside and swap, so a failed save never leaves a half-written icon.
    const QString tmp = file + QStringLiteral(".new");
    if (!img.save(tmp, "PNG")) {
        QFile::remove(tmp);
        return false;
    }
    QFile::remove(file);
    if (!QFile::rename(tmp, file)) {
        QFile::remove(tmp);
        invalidate();
        return false;
    }
    invalidate();
    return true;
}

void remove() {
    QFile::remove(path());
    QSettings("msga", "msga").setValue(QLatin1String(kEnabledKey), false);
    invalidate();
}

bool hasImage() {
    return QFile::exists(path());
}

bool enabled() {
    return QSettings("msga", "msga").value(QLatin1String(kEnabledKey), false).toBool();
}

void setEnabled(bool on) {
    QSettings("msga", "msga").setValue(QLatin1String(kEnabledKey), on);
    invalidate();
}

bool monochrome() {
    // On by default: trays are monochrome, like the built-in icon.
    return QSettings("msga", "msga").value(QLatin1String(kMonochromeKey), true).toBool();
}

void setMonochrome(bool on) {
    QSettings("msga", "msga").setValue(QLatin1String(kMonochromeKey), on);
    invalidate();
}

QImage stored() {
    return prepare(QImage(path(), "PNG"));
}

QImage current() {
    if (!g_currentValid) {
        g_current      = enabled() ? styled(QImage(path(), "PNG"), monochrome()) : QImage();
        g_currentValid = true;
    }
    return g_current;
}

} // namespace CustomTrayIcon
