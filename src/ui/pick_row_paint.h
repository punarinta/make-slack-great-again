// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "theme.h"
#include "user_avatar.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

// Painting shared by the rows of the floating pick lists: the @-mention popup
// (MentionRow) and the composer completer's channel / slash-command rows.
// Header-only like paint_utils.h. Callers pass already-translated text so each
// list keeps its own translation context.
namespace PickRow {

// The "Enter" key affordance on the selected row: a bordered caption-size
// chip right-aligned at `textRight`, vertically centred in a `rowH` row.
// Returns the new right edge for the text columns (`gap` left of the chip).
inline int paintEnterBadge(
    QPainter &p, const QFont &base, const QString &text, int textRight, int rowH, int gap
) {
    QFont ef = base;
    ef.setPixelSize(Th::c().fonts.caption);
    const QFontMetrics efm(ef);
    const int          ew = efm.horizontalAdvance(text) + 18;
    const int          eh = 22;
    const QRect        enterRect(textRight - ew, (rowH - eh) / 2, ew, eh);
    p.setPen(QPen(Th::c().divider.strong, 1));
    p.setBrush(Th::c().surface.raised);
    p.drawRoundedRect(QRectF(enterRect).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    p.setFont(ef);
    p.setPen(Th::c().text.secondary);
    p.drawText(enterRect, Qt::AlignCenter, text);
    return enterRect.left() - gap;
}

// The bold primary label from `x` to `textRight`, elided, vertically centred.
// Returns the width actually drawn.
inline int
paintBoldName(QPainter &p, const QFont &base, const QString &name, int x, int textRight, int rowH) {
    QFont nameF = base;
    nameF.setPixelSize(Th::c().fonts.base);
    nameF.setBold(true);
    const QFontMetrics nfm(nameF);
    const QString      nameE = nfm.elidedText(name, Qt::ElideRight, textRight - x);
    p.setFont(nameF);
    p.setPen(Th::c().text.primary);
    p.drawText(QRect(x, 0, textRight - x, rowH), Qt::AlignVCenter | Qt::AlignLeft, nameE);
    return nfm.horizontalAdvance(nameE);
}

// A rounded-square picture, or — while it is missing — a grey chip with a
// bold `initial` (skipped when empty) in `base` at 42% of the box height.
inline void paintIcon(
    QPainter      &p,
    const QRect   &iconR,
    const QPixmap &px,
    qreal          dpr,
    int            radius,
    const QFont   &base,
    const QString &initial
) {
    if (!px.isNull()) {
        UserAvatar::paintPhoto(p, iconR, px, dpr, radius);
        return;
    }
    QPainterPath clip;
    clip.addRoundedRect(QRectF(iconR), radius, radius);
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().presence.away);
    p.drawPath(clip);
    if (initial.isEmpty())
        return;
    QFont f = base;
    f.setBold(true);
    f.setPixelSize(qRound(iconR.height() * 0.42));
    p.setFont(f);
    p.setPen(Th::c().text.onDark);
    p.drawText(iconR, Qt::AlignCenter, initial);
}

} // namespace PickRow
