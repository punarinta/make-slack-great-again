// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QAbstractScrollArea>

class QColor;
class QPainter;
class QPaintEvent;
class QMouseEvent;

// Base for zero-widget virtual-painted scroll areas.
// Provides: uniform event-filter dispatch, scrollbar drag state, scrollbar thumb helper.
class VirtualListWidget : public QAbstractScrollArea {
    Q_OBJECT
protected:
    explicit VirtualListWidget(QWidget *parent = nullptr);

    virtual void doPaint(QPaintEvent *event) = 0;
    virtual void doMousePress(QMouseEvent *event) { Q_UNUSED(event) }
    virtual void doMouseMove(QMouseEvent *event) { Q_UNUSED(event) }
    virtual void doMouseRelease(QMouseEvent *event) { Q_UNUSED(event) }
    virtual void doMouseLeave() {}

    // Returns true if vpY falls within the scrollbar thumb.
    // totalH — total document height in pixels.
    bool isOnScrollThumb(int vpY, int totalH) const;

    // Paints the Telegram-style overlay scrollbar thumb.
    void paintScrollThumb(QPainter &p, int totalH, const QColor &color) const;

    bool _sbDragging        = false;
    int  _sbDragStartY      = 0;
    int  _sbDragStartScroll = 0;

    static constexpr int kScrollW = 4; // thumb width in logical pixels

    bool eventFilter(QObject *obj, QEvent *event) override;
};
