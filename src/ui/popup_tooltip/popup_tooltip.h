// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

// Dark rounded-rect tooltip with a downward-pointing chevron.
// Call showAbove(text, targetGlobalRect) to position and reveal it;
// hide() to dismiss.  The chevron tip points at the center-top of targetGlobalRect.
class PopupTooltip : public QWidget {
    Q_OBJECT
public:
    explicit PopupTooltip(QWidget *parent = nullptr);
    void showAbove(const QString &text, const QRect &targetGlobalRect);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QString _text;
    int     _arrowX = 0;     // arrow-tip x in widget coords, set after screen clamp
    bool    _below  = false; // true when tooltip is shown below the target

    static constexpr int kPadH   = 10;
    static constexpr int kPadV   = 5;
    static constexpr int kRadius = 6;
    static constexpr int kArrowW = 7; // half-width of arrow base
    static constexpr int kArrowH = 6; // height of arrow
    static constexpr int kShadow = 5; // transparent padding around widget
    static constexpr int kGap    = 4; // gap between arrow tip and target top
};
