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
    bool isSelected = false;
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
    // Layer 1: background circle in row colour — visually separates indicator from avatar.
    // Layer 2: smaller indicator circle on top — filled (online/DND) or hollow ring (offline).
    constexpr int bgD  = 10;
    constexpr int dotD = 6;
    const int     cx   = rect.right() - 2;
    const int     cy   = rect.bottom() - 2;
    const QRect   bgDot(cx - bgD / 2, cy - bgD / 2, bgD, bgD);
    const QRect   dot(cx - dotD / 2, cy - dotD / 2, dotD, dotD);

    if (borderColor.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(borderColor);
        p.drawEllipse(bgDot);
    }

    p.setPen(Qt::NoPen);
    if (state.dndEnabled) {
        p.setBrush(Th::c().divider.def);
        p.drawEllipse(dot);
        p.setPen(QPen(Th::c().presence.away, 1.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(cx - 2, cy, cx + 2, cy);
    } else if (state.isActive) {
        p.setBrush(Th::c().presence.online);
        p.drawEllipse(dot);
    } else {
        const QColor ring = state.isSelected ? Th::c().nav.primary : Th::c().nav.itemTextDim;
        p.setPen(QPen(ring, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(dot);
    }

    p.restore();
}

} // namespace UserAvatar
