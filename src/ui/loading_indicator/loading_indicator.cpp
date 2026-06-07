// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "loading_indicator.h"
#include "ui/theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QShowEvent>

LoadingIndicator::LoadingIndicator() {
    _timer.setTimerType(Qt::CoarseTimer);
    QObject::connect(&_timer, &QTimer::timeout, [this] {
        _step = (_step + 1) % 4;
        if (_onUpdate)
            _onUpdate();
    });
}

LoadingIndicator::~LoadingIndicator() {
    _timer.stop();
}

void LoadingIndicator::setUpdateCallback(std::function<void()> cb) {
    _onUpdate = std::move(cb);
}

void LoadingIndicator::start() {
    _step = 0;
    _timer.start(kIntervalMs);
    if (_onUpdate)
        _onUpdate();
}

void LoadingIndicator::stop() {
    _timer.stop();
    if (_onUpdate)
        _onUpdate();
}

bool LoadingIndicator::isRunning() const {
    return _timer.isActive();
}

void LoadingIndicator::paint(QPainter &p, const QRect &rect) const {
    const int cx   = rect.x() + rect.width() / 2;
    const int cy   = rect.y() + rect.height() / 2;
    const int half = kDiameter / 2;

    const QRectF arc(
        cx - half + kStroke / 2.0,
        cy - half + kStroke / 2.0,
        kDiameter - kStroke,
        kDiameter - kStroke
    );

    // Each segment spans 90°, minus a small gap on each end.
    // Segments go clockwise from 12 o'clock (Qt: positive y up → 90° is top).
    // Qt arc convention: 0° = 3 o'clock, positive = counter-clockwise.
    // For clockwise segments starting at top: startAngle = 90 - i*90 - kGapDeg, span = -(90 -
    // 2*kGapDeg).
    const int sweep = -(90 - 2 * kGapDeg) * 16;

    QPen pen;
    pen.setWidthF(kStroke);
    pen.setCapStyle(Qt::FlatCap);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    const std::array<QColor, 4> colors = {
        Th::c().loader.a,
        Th::c().loader.b,
        Th::c().loader.c,
        Th::c().loader.d,
    };
    for (int i = 0; i < 4; ++i) {
        QColor c = colors[i];
        c.setAlphaF(i == _step ? 1.0 : 0.2);
        pen.setColor(c);
        p.setPen(pen);
        const int start = (90 - i * 90 - kGapDeg) * 16;
        p.drawArc(arc, start, sweep);
    }
    p.restore();
}

// ── LoadingIndicatorWidget ────────────────────────────────────────────────────

LoadingIndicatorWidget::LoadingIndicatorWidget(QWidget *parent) : QWidget(parent) {
    _anim.setUpdateCallback([this] { update(); });
}

void LoadingIndicatorWidget::showEvent(QShowEvent *) {
    _anim.start();
}

void LoadingIndicatorWidget::hideEvent(QHideEvent *) {
    _anim.stop();
}

void LoadingIndicatorWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    _anim.paint(p, rect());
}
