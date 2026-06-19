// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

#include <algorithm>

// Shared popup positioning. The "place a panel next to an anchor, flip to the
// opposite edge if it doesn't fit, then clamp on-screen" logic used to be
// copy-pasted (and a couple of copies forgot to clamp one axis, so they ran
// off-screen). All coordinates are in ONE space: pass screen
// `availableGeometry()` as `bounds` for a top-level popup, or the parent rect
// (`QRect(0,0,parentW,parentH)`) for a child popup positioned in parent coords.
namespace Ui {

enum class Edge { Below, Above, Right, Left };
// Cross-axis alignment of the popup against the anchor.
enum class Align { Center, Start }; // Start = popup's leading edge at anchor's leading edge

// Shift `topLeft` minimally (no resize) so a `size` rect lies fully inside
// `bounds`. Width/height based so it works in both screen and parent coords.
inline QPoint clampInto(QSize size, QPoint topLeft, const QRect &bounds) {
    int x = topLeft.x();
    int y = topLeft.y();
    if (bounds.isValid()) {
        const int maxX = bounds.left() + bounds.width() - size.width();
        const int maxY = bounds.top() + bounds.height() - size.height();
        x              = std::max(bounds.left(), std::min(x, std::max(bounds.left(), maxX)));
        y              = std::max(bounds.top(), std::min(y, std::max(bounds.top(), maxY)));
    }
    return {x, y};
}

// Position `size` against `anchor` on the `preferred` edge with `gap` px of
// separation; flip to the opposite edge if the preferred side overflows
// `bounds`; center or start-align on the cross axis; then clamp fully inside
// `bounds`. `*flipped` (optional) reports whether the opposite edge was used.
inline QPoint placePopup(
    const QRect &anchor,
    QSize        size,
    const QRect &bounds,
    Edge         preferred,
    int          gap,
    Align        align   = Align::Center,
    bool        *flipped = nullptr
) {
    const bool vertical = (preferred == Edge::Below || preferred == Edge::Above);
    const bool valid    = bounds.isValid();
    bool       flip     = false;
    int        x        = 0;
    int        y        = 0;

    if (vertical) {
        const int below = anchor.bottom() + gap;              // popup top if placed below
        const int above = anchor.top() - gap - size.height(); // popup top if placed above
        if (valid) {
            if (preferred == Edge::Below)
                flip = (below + size.height() - 1 > bounds.bottom());
            else
                flip = (above < bounds.top());
        }
        const bool useBelow = (preferred == Edge::Below) != flip;
        y                   = useBelow ? below : above;
        x = (align == Align::Center) ? anchor.center().x() - size.width() / 2 : anchor.left();
    } else {
        const int right = anchor.right() + gap;               // popup left if placed right
        const int left  = anchor.left() - gap - size.width(); // popup left if placed left
        if (valid) {
            if (preferred == Edge::Right)
                flip = (right + size.width() - 1 > bounds.right());
            else
                flip = (left < bounds.left());
        }
        const bool useRight = (preferred == Edge::Right) != flip;
        x                   = useRight ? right : left;
        y = (align == Align::Center) ? anchor.center().y() - size.height() / 2 : anchor.top();
    }

    if (flipped)
        *flipped = flip;
    return clampInto(size, {x, y}, bounds);
}

} // namespace Ui
