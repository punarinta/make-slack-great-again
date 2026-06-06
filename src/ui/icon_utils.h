// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QSvgRenderer>
#include <QGuiApplication>

// Renders SVG at physical size (size * dpr) with DPR set AFTER painting so
// QPainter's coordinate system is never scaled — avoids the "top-left only"
// corruption that happens when DPR is set before QPainter::begin().
static inline QPixmap svgPixmapPhys(const QString &path,
                                    const QSize &logicalSize,
                                    const QColor &color,
                                    qreal dpr)
{
    QSvgRenderer renderer(path);
    if (!renderer.isValid()) return {};
    const QSize phys(qRound(logicalSize.width() * dpr),
                     qRound(logicalSize.height() * dpr));
    QPixmap px(phys);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(px.rect(), color);
    p.end();
    px.setDevicePixelRatio(dpr);  // metadata only — painting is already done
    return px;
}

// For use with QLabel::setPixmap — crisp at the current screen DPR.
inline QPixmap svgPixmap(const QString &path, const QSize &size, const QColor &color)
{
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    return svgPixmapPhys(path, size, color, dpr);
}

// For QPushButton/QToolButton::setIcon — adds plain 1× and 2× pixmaps (no DPR
// metadata) so QIcon's matching picks the 2× pixmap on HiDPI and returns it
// with dpr=2 set, giving crisp rendering without triggering Qt's addPixmap/DPR bug.
inline QIcon svgIcon(const QString &path, const QSize &size, const QColor &color)
{
    auto plain = [&](int scale) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QPixmap{};
        const QSize phys(size.width() * scale, size.height() * scale);
        QPixmap px(phys);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(px.rect(), color);
        return px;  // intentionally no setDevicePixelRatio
    };
    QIcon icon;
    icon.addPixmap(plain(1));
    icon.addPixmap(plain(2));
    return icon;
}
