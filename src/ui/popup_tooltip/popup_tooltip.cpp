// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QPainter>
#include <QPaintEvent>
#include <QApplication>
#include <QFontMetrics>
#include <QScreen>
#include <QGuiApplication>
#include <algorithm>

PopupTooltip::PopupTooltip(QWidget *parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
}

void PopupTooltip::showAbove(const QString &text, const QRect &targetGlobalRect) {
    _text    = text;
    _rightOf = false;

    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    const QFontMetrics fm(f);

    const int bodyW   = fm.horizontalAdvance(text) + 2 * kPadH;
    const int bodyH   = fm.height() + 2 * kPadV;
    const int widgetW = bodyW + 2 * kShadow;
    const int widgetH = kShadow + bodyH + kArrowH + kShadow;

    const int arrowTipGX = targetGlobalRect.center().x();

    QScreen    *s     = QGuiApplication::screenAt(targetGlobalRect.center());
    const QRect avail = s ? s->availableGeometry() : QRect();

    // Prefer above; fall back to below if there isn't enough room
    const int neededAbove = kShadow + bodyH + kArrowH + kGap;
    _below                = avail.isValid() && (targetGlobalRect.top() - avail.top() < neededAbove);

    int wx, wy;
    if (_below) {
        // Arrow tip just below the target's bottom edge; tip is at widget y=kShadow
        wy = targetGlobalRect.bottom() + kGap - kShadow;
    } else {
        // Arrow tip just above the target's top edge; tip is at widget y=kShadow+bodyH+kArrowH
        wy = targetGlobalRect.top() - kGap - (kShadow + bodyH + kArrowH);
    }
    wx = arrowTipGX - widgetW / 2;

    if (avail.isValid()) {
        wx = std::max(avail.left(), std::min(wx, avail.right() - widgetW));
        if (_below)
            wy = std::min(wy, avail.bottom() - widgetH);
        else
            wy = std::max(avail.top(), wy);
    }

    _arrowX = std::clamp(arrowTipGX - wx, kShadow + kArrowW, widgetW - kShadow - kArrowW);

    setFixedSize(widgetW, widgetH);
    move(wx, wy);
    show();
    raise();
    update();
}

void PopupTooltip::showRightOf(const QString &text, const QRect &targetGlobalRect) {
    _text    = text;
    _below   = false;
    _rightOf = true;

    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    const QFontMetrics fm(f);

    const int bodyW   = fm.horizontalAdvance(text) + 2 * kPadH;
    const int bodyH   = fm.height() + 2 * kPadV;
    const int widgetW = kShadow + kArrowH + bodyW + kShadow;
    const int widgetH = bodyH + 2 * kShadow;

    const int arrowTipGY = targetGlobalRect.center().y();

    QScreen    *s     = QGuiApplication::screenAt(targetGlobalRect.center());
    const QRect avail = s ? s->availableGeometry() : QRect();

    int wx = targetGlobalRect.right() + kGap - kShadow;
    int wy = arrowTipGY - widgetH / 2;

    if (avail.isValid()) {
        wx = std::min(wx, avail.right() - widgetW);
        wy = std::max(avail.top(), std::min(wy, avail.bottom() - widgetH));
    }

    _arrowY = std::clamp(arrowTipGY - wy, kShadow + kArrowW, widgetH - kShadow - kArrowW);

    setFixedSize(widgetW, widgetH);
    move(wx, wy);
    show();
    raise();
    update();
}

void PopupTooltip::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF body;
    if (_rightOf) {
        body = QRectF(
            kShadow + kArrowH,
            kShadow,
            width() - kShadow - kArrowH - kShadow,
            height() - 2 * kShadow
        );
    } else {
        const int bodyH = height() - kShadow - kArrowH - kShadow;
        body            = _below ? QRectF(kShadow, kShadow + kArrowH, width() - 2 * kShadow, bodyH)
                                 : QRectF(kShadow, kShadow, width() - 2 * kShadow, bodyH);
    }

    // Light drop shadow around the body only
    for (int i = kShadow; i >= 2; --i) {
        const int alpha = (kShadow - i) * 3;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(
            body.adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), kRadius + i, kRadius + i
        );
    }

    // Fill body
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().text.primary);
    p.drawRoundedRect(body, kRadius, kRadius);

    // Arrow — base overlaps body by 3px to cover antialiased edge seam
    QPolygonF arrow;
    if (_rightOf) {
        // Tip points left toward the target
        const qreal baseX = body.left();
        const qreal tipX  = kShadow;
        const qreal cy    = _arrowY;
        arrow << QPointF(baseX + 3, cy - kArrowW) << QPointF(baseX + 3, cy + kArrowW)
              << QPointF(tipX, cy);
    } else {
        const qreal cx = _arrowX;
        if (_below) {
            const qreal baseY = body.top();
            const qreal tipY  = kShadow;
            arrow << QPointF(cx - kArrowW, baseY + 3) << QPointF(cx + kArrowW, baseY + 3)
                  << QPointF(cx, tipY);
        } else {
            const qreal baseY = body.bottom();
            const qreal tipY  = baseY + kArrowH;
            arrow << QPointF(cx - kArrowW, baseY - 3) << QPointF(cx + kArrowW, baseY - 3)
                  << QPointF(cx, tipY);
        }
    }
    p.drawPolygon(arrow);

    // Text
    QFont f = QApplication::font();
    f.setWeight(QFont::Weight(500));
    p.setFont(f);
    p.setPen(Th::c().text.onDark);
    p.drawText(
        QRectF(
            body.left() + kPadH,
            body.top() + kPadV,
            body.width() - 2 * kPadH,
            body.height() - 2 * kPadV
        ),
        Qt::AlignCenter,
        _text
    );
}
