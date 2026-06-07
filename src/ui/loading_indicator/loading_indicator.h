// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QTimer>
#include <QColor>
#include <QWidget>
#include <functional>
#include <array>

class QPainter;
class QRect;

// Animated four-segment ring using app brand colors.
// Non-widget: owns a QTimer and paints itself into any rect on demand.
// Usage:
//   _loader.setUpdateCallback([this] { viewport()->update(); });
//   _loader.start();     // show spinner
//   _loader.stop();      // hide spinner
//   // in paintEvent:
//   if (_loader.isRunning()) _loader.paint(p, rect());
class LoadingIndicator {
public:
    LoadingIndicator();
    ~LoadingIndicator();

    void setUpdateCallback(std::function<void()> cb);
    void start();
    void stop();
    bool isRunning() const;

    // Paint a centered spinner into `rect` using painter `p`.
    void paint(QPainter &p, const QRect &rect) const;

private:
    QTimer                _timer;
    int                   _step = 0;
    std::function<void()> _onUpdate;

    static constexpr int kDiameter   = 52;
    static constexpr int kStroke     = 7;
    static constexpr int kGapDeg     = 3; // degrees of visual gap on each end of a segment
    static constexpr int kIntervalMs = 160;
    static const std::array<QColor, 4> kColors;
};

// QWidget wrapper — drop into any layout; starts/stops the animation with visibility.
class LoadingIndicatorWidget : public QWidget {
    Q_OBJECT
public:
    explicit LoadingIndicatorWidget(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    LoadingIndicator _anim;
};
