// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "emoji_pixmap.h"

#include "emoji_font.h"

#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>
#include <QtMath>

namespace EmojiPix {
namespace {

// Bounding box of non-transparent pixels; null rect when fully transparent.
QRect inkBounds(const QImage &img) {
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            minX = qMin(minX, x);
            maxX = qMax(maxX, x);
            minY = qMin(minY, y);
            maxY = qMax(maxY, y);
        }
    }
    if (maxX < 0)
        return {};
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

QPixmap render(const QString &emoji, int px, const QColor &color, qreal dpr) {
    const QFont        f = emojiFont(px);
    const QFontMetrics fm(f);
    // Color emoji fonts report advances up to ~2× the ink width, and ink can
    // slightly overhang the em box — render generously, then crop to the ink.
    const int          w = qMax(px * 3, fm.horizontalAdvance(emoji) + px);
    const int          h = fm.height() + px;
    QImage             img(qCeil(w * dpr), qCeil(h * dpr), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QPoint(px / 2, fm.ascent()), emoji);
    }
    const QRect ink = inkBounds(img); // device pixels
    if (ink.isNull())
        return {};
    QImage    cropped = img.copy(ink);
    // Overflowing glyphs are fit into the px box like the custom-emoji image
    // path does; smaller ink is kept 1:1 rather than upscaled.
    const int boxDev  = qCeil(px * dpr);
    if (cropped.width() > boxDev || cropped.height() > boxDev)
        cropped = cropped.scaled(boxDev, boxDev, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    cropped.setDevicePixelRatio(dpr);
    return QPixmap::fromImage(cropped);
}

QString cacheKey(const QString &emoji, int px, const QColor &color, qreal dpr) {
    return QStringLiteral("emjpx:%1:%2:%3:%4").arg(emoji).arg(px).arg(color.rgba()).arg(dpr);
}

} // namespace

QPixmap pixmap(const QString &emoji, int px, const QColor &color, qreal dpr) {
    if (emoji.trimmed().isEmpty() || px <= 0)
        return {};
    if (dpr <= 0)
        dpr = 1;
    const QString key = cacheKey(emoji, px, color, dpr);
    QPixmap       pm;
    if (QPixmapCache::find(key, &pm))
        return pm;
    pm = render(emoji, px, color, dpr);
    // QPixmapCache refuses null pixmaps, so ink-less glyphs re-render on each
    // call — harmless, callers don't pass those from paint paths.
    if (!pm.isNull())
        QPixmapCache::insert(key, pm);
    return pm;
}

void draw(QPainter &p, const QRect &box, const QString &emoji, int px, const QColor &color) {
    const QPixmap pm = pixmap(emoji, px, color, p.device()->devicePixelRatioF());
    if (pm.isNull())
        return;
    const QSizeF ls = pm.deviceIndependentSize();
    p.drawPixmap(
        QPoint(
            box.x() + qRound((box.width() - ls.width()) / 2.0),
            box.y() + qRound((box.height() - ls.height()) / 2.0)
        ),
        pm
    );
}

int width(const QString &emoji, int px, qreal dpr) {
    return qCeil(pixmap(emoji, px, QColor(Qt::black), dpr).deviceIndependentSize().width());
}

} // namespace EmojiPix
