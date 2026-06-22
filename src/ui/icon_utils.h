// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QApplication>
#include <QIcon>
#include <QIconEngine>
#include <QPaintDevice>
#include <QPixmap>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QSvgRenderer>
#include <QGuiApplication>

// Renders SVG at physical size (size * dpr) with DPR set AFTER painting so
// QPainter's coordinate system is never scaled — avoids the "top-left only"
// corruption that happens when DPR is set before QPainter::begin().
static inline QPixmap
svgPixmapPhys(const QString &path, const QSize &logicalSize, const QColor &color, qreal dpr) {
    QSvgRenderer renderer(path);
    if (!renderer.isValid())
        return {};
    const QSize phys(qRound(logicalSize.width() * dpr), qRound(logicalSize.height() * dpr));
    QPixmap     px(phys);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(px.rect(), color);
    p.end();
    px.setDevicePixelRatio(dpr); // metadata only — painting is already done
    return px;
}

// For use with QLabel::setPixmap — crisp at the current screen DPR.
inline QPixmap svgPixmap(const QString &path, const QSize &size, const QColor &color) {
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    return svgPixmapPhys(path, size, color, dpr);
}

// Icon engine that rasterises (and recolours) the SVG on demand at the exact
// requested size × devicePixelRatio, instead of pre-baking fixed 1×/2× pixmaps.
// At a fractional display scale (e.g. 1.3333×) there is no integer pixmap that
// matches the device, so a pre-baked QIcon has to resample 16px/32px up or down
// to ~21px → visibly pixelated button icons. Rendering per request keeps them
// pixel-perfect at any DPR (the same path the custom-painted chrome icons use).
class RecoloredSvgIconEngine final : public QIconEngine {
public:
    RecoloredSvgIconEngine(QString path, QSize natural, QColor color)
        : _path(std::move(path)), _natural(natural), _color(color) {}

    QIconEngine *clone() const override {
        return new RecoloredSvgIconEngine(_path, _natural, _color);
    }

    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return {_natural}; }

    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override {
        return size.isValid() && !size.isEmpty() ? size : _natural;
    }

    void paint(QPainter *p, const QRect &rect, QIcon::Mode m, QIcon::State s) override {
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        p->drawPixmap(rect, scaledPixmap(rect.size(), m, s, dpr));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode m, QIcon::State s) override {
        return scaledPixmap(size, m, s, 1.0);
    }

    // `size` is logical; return a pixmap of size×scale physical pixels tagged
    // with devicePixelRatio = scale, so Qt draws it at the right logical size
    // without resampling.
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State, qreal scale) override {
        QSize        logical = (size.isValid() && !size.isEmpty()) ? size : _natural;
        QSvgRenderer renderer(_path);
        if (!renderer.isValid())
            return {};
        const QSize phys(qRound(logical.width() * scale), qRound(logical.height() * scale));
        QPixmap     px(phys);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&p);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(px.rect(), _color);
        p.end();
        px.setDevicePixelRatio(scale); // metadata only — painting already done at 1:1
        // Match the old QIcon behaviour: it stored only Normal-mode pixmaps and
        // let the style synthesise the Disabled/Active fade. Reproduce that so
        // disabled buttons still get a greyed icon.
        if (mode != QIcon::Normal) {
            QStyleOption opt(0);
            opt.palette = QApplication::palette();
            if (QStyle *st = QApplication::style())
                return st->generatedIconPixmap(mode, px, &opt);
        }
        return px;
    }

private:
    QString _path;
    QSize   _natural;
    QColor  _color;
};

// For QPushButton/QToolButton::setIcon — crisp at any (including fractional) DPR.
// `size` is the icon's natural size, used when the widget doesn't request one.
inline QIcon svgIcon(const QString &path, const QSize &size, const QColor &color) {
    return QIcon(new RecoloredSvgIconEngine(path, size, color));
}
