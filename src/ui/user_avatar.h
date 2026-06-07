// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QApplication>
#include <QFont>

// Shared avatar drawing logic used by HeaderAvatarWidget and ConvListWidget.
// Draws: rounded-rect avatar (or initial-letter placeholder) + presence/DND dot.
namespace UserAvatar {

struct State {
    bool isActive   = false;
    bool dndEnabled = false;
};

// Paints a rounded-rect avatar + a 10px presence/DND indicator dot.
// The dot is placed at the bottom-right corner of `rect`, overhanging it by 3px
// on each side — callers must allocate that extra space (e.g. a 36×36 widget
// for a 28×28 avatar).
//
//   rect          full avatar rect (e.g. QRect(0,0,28,28))
//   pixmap        null → grey placeholder with initial letter
//   initial       first letter for the placeholder; may be empty
//   state         isActive / dndEnabled
//   cornerRadius  rounded-rect corner radius
//   dpr           device pixel ratio (p.device()->devicePixelRatioF())
//   borderColor   thin ring drawn around the dot; pass Qt::transparent for no ring.
//                 Should match the container background (creates the "floating dot" look).
inline void paint(
    QPainter      &p,
    QRect          rect,
    const QPixmap &pixmap,
    const QString &initial,
    const State   &state,
    int            cornerRadius,
    qreal          dpr,
    QColor         borderColor = Qt::transparent
) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    // ── Avatar image or placeholder ────────────────────────────────────
    if (!pixmap.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect), cornerRadius, cornerRadius);
        p.setClipPath(clip);
        QPixmap scaled = pixmap.scaled(
            rect.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
        );
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(rect, scaled);
        p.setClipping(false);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().presence.away);
        p.drawRoundedRect(rect, cornerRadius, cornerRadius);
        if (!initial.isEmpty()) {
            p.setPen(Qt::white);
            QFont f = QApplication::font();
            f.setBold(true);
            f.setPointSizeF(rect.height() * 0.38);
            p.setFont(f);
            p.drawText(rect, Qt::AlignCenter, initial.left(1).toUpper());
        }
    }

    // ── Presence / DND dot ─────────────────────────────────────────────
    constexpr int dotD = 10;
    const QRect   dot(rect.right() - dotD + 3, rect.bottom() - dotD + 3, dotD, dotD);

    // Border ring — matches container background, creating the "cut-out" look.
    if (borderColor.alpha() > 0) {
        p.setPen(QPen(borderColor, 2));
    } else {
        p.setPen(Qt::NoPen);
    }

    if (state.dndEnabled) {
        p.setBrush(Th::c().divider.def);
        p.drawEllipse(dot);
        // Horizontal bar — "do not disturb"
        p.setPen(QPen(Th::c().presence.away, 1.5, Qt::SolidLine, Qt::RoundCap));
        const int cx = dot.center().x();
        const int cy = dot.center().y();
        p.drawLine(cx - 2, cy, cx + 2, cy);
    } else {
        p.setBrush(state.isActive ? Th::c().presence.online : Th::c().divider.def);
        p.drawEllipse(dot);
    }

    p.restore();
}

} // namespace UserAvatar
