// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "virtual_list_widget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QScrollBar>

VirtualListWidget::VirtualListWidget(QWidget *parent) : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->installEventFilter(this);
}

bool VirtualListWidget::eventFilter(QObject *obj, QEvent *event) {
    if (obj == viewport()) {
        switch (event->type()) {
        case QEvent::Paint:
            doPaint(static_cast<QPaintEvent *>(event));
            return true;
        case QEvent::MouseButtonPress:
            doMousePress(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::MouseButtonRelease:
            doMouseRelease(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::MouseMove:
            doMouseMove(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            doMouseLeave();
            return false;
        default:
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(obj, event);
}

bool VirtualListWidget::isOnScrollThumb(int vpY, int totalH) const {
    const int vh = viewport()->height();
    if (totalH <= vh)
        return false;
    const int scrollY = verticalScrollBar()->value();
    const int thumbH  = std::max(20, vh * vh / totalH);
    const int thumbY  = scrollY * (vh - thumbH) / (totalH - vh);
    return vpY >= thumbY && vpY < thumbY + thumbH;
}

void VirtualListWidget::paintScrollThumb(QPainter &p, int totalH, const QColor &color) const {
    const int vh = viewport()->height();
    if (totalH <= vh)
        return;
    const int scrollY = verticalScrollBar()->value();
    const int thumbH  = std::max(20, vh * vh / totalH);
    const int thumbY  = (totalH - vh > 0) ? scrollY * (vh - thumbH) / (totalH - vh) : 0;
    const int sbX     = viewport()->width() - kScrollW - 2;
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(sbX, thumbY, kScrollW, thumbH, kScrollW / 2.0, kScrollW / 2.0);
}
