// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "theme.h"

#include <QBrush>
#include <QColor>
#include <QPainter>
#include <QRectF>

// Small shared QPainter idioms that were copy-pasted across popups, cards and
// the message list. Header-only (like icon_utils.h) — the bodies are tiny and
// hot paths benefit from inlining.
namespace Paint {

// The soft drop-shadow shared by popups and cards: concentric translucent
// rounded-rect halos around `body`. Layers run from `spread` (outermost,
// faintest) down to `minLayer` (innermost, darkest); layer i expands `body` by
// i px — with a half-px inset so the rounded edges stay crisp under AA — and
// uses radius `radius + i`. Alpha for layer i is `baseAlpha + (spread - i) * alphaStep`.
//
// The caller still draws the body fill itself afterwards. The painter's pen is
// left as NoPen on return (every caller sets its own pen for the fill).
//
//   PopupTooltip:    dropShadow(p, body, kRadius, kShadow, 0, 3);
//   ContextMenu:     dropShadow(p, mrf,  kRadius, kShadow, 2, 2, /*minLayer*/ 1);
//   UserProfileCard: dropShadow(p, card, kRadius, kShadow, 0, 4);
inline void dropShadow(
    QPainter     &p,
    const QRectF &body,
    qreal         radius,
    int           spread,
    int           baseAlpha,
    int           alphaStep,
    int           minLayer = 2
) {
    p.setPen(Qt::NoPen);
    for (int i = spread; i >= minLayer; --i) {
        p.setBrush(QColor(0, 0, 0, baseAlpha + (spread - i) * alphaStep));
        p.drawRoundedRect(
            body.adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), radius + i, radius + i
        );
    }
}

// A fully-rounded "pill" fill: corner radius = half the rect's height. The
// caller sets the brush/pen first; this just names the `drawRoundedRect(r, h/2,
// h/2)` idiom (huddle pill, unread badge body, workspace active bar, …).
inline void pill(QPainter &p, const QRectF &rect) {
    p.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
}

// Subtle rounded selection/hover fill (the mention-row highlight). Sets NoPen +
// `fill` itself; pass `Th::c().surface.highlight` as the brush.
inline void rowHighlight(QPainter &p, const QRectF &rect, const QBrush &fill, qreal radius = 6) {
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(rect, radius, radius);
}

// Stroked rounded rect with a half-pixel inset so a 1px border stays crisp under
// antialiasing (the code-block / file-chip frame idiom). Caller sets pen + brush.
inline void borderedRect(QPainter &p, const QRectF &rect, qreal radius = 4) {
    p.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

// The floating action-toolbar card behind the message hover toolbar and the
// file action bar: a soft drop shadow (integer inset, biased 1px downward so it
// "sits" on the row), a raised-surface fill and a hairline. The shadow alphas
// are an intentional non-token (runtime-alpha shadow layers); the fill/hairline
// read the live theme so the card darkens with a dark content area. The caller
// fills the card's contents afterwards.
inline void toolbarCard(QPainter &p, const QRectF &card, qreal radius) {
    p.setPen(Qt::NoPen);
    for (int i = 4; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, 5 + (4 - i) * 3));
        p.drawRoundedRect(card.adjusted(-i, -i, i, i + 1), radius + i, radius + i);
    }
    p.setBrush(Th::c().surface.raised);
    p.setPen(Th::c().divider.def);
    p.drawRoundedRect(card, radius, radius);
}

} // namespace Paint
