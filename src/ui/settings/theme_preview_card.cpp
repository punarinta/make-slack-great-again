// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme_preview_card.h"
#include "ui/theme_manager.h"

#include <QPainter>
#include <QPainterPath>

ThemePreviewCard::ThemePreviewCard(
    QString themeId, QString displayName, const Th::Theme &preview, QWidget *parent
)
    : QAbstractButton(parent), _themeId(std::move(themeId)), _name(std::move(displayName)),
      _preview(preview) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    connect(this, &QAbstractButton::toggled, this, [this] { update(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
}

QSize ThemePreviewCard::sizeHint() const {
    return {kCardW, kMockH + kLabelH};
}

void ThemePreviewCard::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto &active = Th::c();  // selection chrome
    const auto &th     = _preview; // mock content

    // ── Mock card ─────────────────────────────────────────────────────
    const QRectF mock(1.5, 1.5, width() - 3.0, kMockH - 3.0);
    QPainterPath clip;
    clip.addRoundedRect(mock, kRadius, kRadius);
    p.save();
    p.setClipPath(clip);

    const int    railW = 18; // workspace rail
    const int    navW  = 46; // conv list column
    const QRectF railRect(mock.left(), mock.top(), railW, mock.height());
    const QRectF navRect(mock.left() + railW, mock.top(), navW, mock.height());

    // Both columns share one vertical gradient (lighter at top), in lockstep —
    // the same effect Th::navGradient() produces in the live sidebar.
    QLinearGradient railGrad(railRect.topLeft(), railRect.bottomLeft());
    railGrad.setColorAt(0.0, th.nav.bgGradTop);
    railGrad.setColorAt(1.0, th.nav.bgGradBottom);
    p.fillRect(railRect, railGrad);

    QLinearGradient navGrad(navRect.topLeft(), navRect.bottomLeft());
    navGrad.setColorAt(0.0, th.nav.primaryGradTop);
    navGrad.setColorAt(1.0, th.nav.primaryGradBottom);
    p.fillRect(navRect, navGrad);

    p.fillRect(
        QRectF(mock.left() + railW + navW, mock.top(), mock.width() - railW - navW, mock.height()),
        th.surface.content
    );

    p.setPen(Qt::NoPen);

    // Workspace bubble on the rail.
    p.setBrush(th.nav.workspaceBubble);
    p.drawRoundedRect(QRectF(mock.left() + 4, mock.top() + 8, 10, 10), 3, 3);

    // Conv list: selected row pill + dim rows + mention badge.
    const qreal navX = mock.left() + railW + 5;
    p.setBrush(th.nav.itemSelected);
    p.drawRoundedRect(QRectF(navX, mock.top() + 10, 34, 8), 3, 3);
    p.setBrush(th.nav.itemTextDim);
    p.drawRoundedRect(QRectF(navX, mock.top() + 24, 28, 5), 2, 2);
    p.drawRoundedRect(QRectF(navX, mock.top() + 35, 31, 5), 2, 2);
    p.drawRoundedRect(QRectF(navX, mock.top() + 46, 24, 5), 2, 2);
    p.setBrush(th.badge.mention);
    p.drawEllipse(QRectF(navX + 31, mock.top() + 45.5, 6, 6));

    // Message area: author + two text lines, then an accent button blob.
    const qreal msgX = mock.left() + railW + navW + 8;
    p.setBrush(th.text.primary);
    p.drawRoundedRect(QRectF(msgX, mock.top() + 12, 30, 6), 2, 2);
    p.setBrush(th.text.secondary);
    p.drawRoundedRect(QRectF(msgX, mock.top() + 24, 52, 5), 2, 2);
    p.drawRoundedRect(QRectF(msgX, mock.top() + 34, 44, 5), 2, 2);
    p.setBrush(th.accent.def);
    p.drawRoundedRect(QRectF(msgX, mock.top() + 50, 26, 10), 3, 3);

    p.restore();

    // Border: accent when selected, divider otherwise (hover = stronger).
    const bool sel = isChecked();
    QPen       border(
        sel ? active.accent.def : (underMouse() ? active.divider.strong : active.divider.def),
        sel ? 2.0 : 1.0
    );
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(mock, kRadius, kRadius);

    // ── Label ─────────────────────────────────────────────────────────
    QFont f = font();
    f.setPixelSize(active.fonts.caption);
    f.setBold(sel);
    p.setFont(f);
    p.setPen(sel ? active.accent.def : active.text.secondary);
    p.drawText(QRect(0, kMockH, width(), kLabelH), Qt::AlignHCenter | Qt::AlignVCenter, _name);
}
