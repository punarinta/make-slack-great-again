// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/icon_utils.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QSize>
#include <QString>

// Shared "ghost" button chrome for the dark nav surfaces: a translucent white
// fill + hairline white border that brightens on hover, with a monochrome SVG
// (or caller-drawn glyph) recolored to match. Used by the workspace switcher's
// add/settings buttons and the conversation-list presence-toggle footer so they
// stay visually identical.
//
// The colours are intentional non-tokens — white-alpha overlays that read
// correctly on any nav tint (nav.bg or nav.primary) — kept in one place so
// every nav ghost button matches.
namespace NavGhostButton {

inline QColor iconColor(bool hovered) {
    return hovered ? QColor(245, 240, 245) : QColor(180, 165, 180);
}

inline void paintChrome(QPainter &p, const QRectF &r, bool hovered, qreal radius) {
    const QColor fill(255, 255, 255, hovered ? 55 : 22);
    const QColor border(255, 255, 255, hovered ? 180 : 90);
    p.setBrush(fill);
    p.setPen(QPen(border, 1.5));
    p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), radius, radius);
}

// Centered, hover-recolored SVG sized to `dimFactor` of the button rect.
inline void paintIcon(
    QPainter &p, const QRectF &r, bool hovered, const QString &svgPath, qreal dimFactor = 0.52
) {
    const qreal   dim = r.width() * dimFactor;
    const QRectF  ir(r.x() + (r.width() - dim) / 2.0, r.y() + (r.height() - dim) / 2.0, dim, dim);
    const int     sz = qMax(1, qRound(dim));
    const QPixmap px = svgPixmap(svgPath, QSize(sz, sz), iconColor(hovered));
    p.drawPixmap(ir.toRect(), px);
}

} // namespace NavGhostButton
