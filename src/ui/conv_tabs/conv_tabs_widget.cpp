// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_tabs_widget.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>

ConvTabsWidget::ConvTabsWidget(QWidget *parent) : QWidget(parent) {
    setFixedHeight(kStripH);
    setMouseTracking(true);
    setAttribute(Qt::WA_StyledBackground);
    rebuildIcons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        rebuildIcons();
        update();
    });
}

void ConvTabsWidget::setActiveTab(Tab tab) {
    if (_active == tab)
        return;
    _active = tab;
    update();
}

void ConvTabsWidget::setCanvasInfo(bool hasCanvas, const QString &title) {
    _hasCanvas   = hasCanvas;
    _canvasTitle = title;
    rebuildIcons();
    relayout();
    update();
}

void ConvTabsWidget::setCanvasTabVisible(bool visible) {
    if (_canvasTabVisible == visible)
        return;
    _canvasTabVisible = visible;
    if (!visible)
        _active = Tab::Messages; // can't stay on a hidden tab
    relayout();
    update();
}

void ConvTabsWidget::rebuildIcons() {
    const auto &th        = Th::c();
    const QSize sz        = QSize(kIconSz, kIconSz);
    const auto  canvasSvg = _hasCanvas ? QStringLiteral(":/ui/canvas.svg")
                                       : QStringLiteral(":/ui/sticky-note-plus.svg");

    _tabs[0].text    = tr("Messages");
    _tabs[0].icon    = svgPixmap(":/ui/message-circle.svg", sz, th.text.secondary);
    _tabs[0].iconHot = svgPixmap(":/ui/message-circle.svg", sz, th.text.primary);

    _tabs[1].text =
        _hasCanvas ? (_canvasTitle.isEmpty() ? tr("Untitled") : _canvasTitle) : tr("Add canvas");
    _tabs[1].icon    = svgPixmap(canvasSvg, sz, th.text.secondary);
    _tabs[1].iconHot = svgPixmap(canvasSvg, sz, th.text.primary);

    relayout();
}

void ConvTabsWidget::relayout() {
    // Measure with the bold (active) font so tabs don't shift when activated.
    QFont f = font();
    f.setPixelSize(Th::c().fonts.md);
    f.setBold(true);
    const QFontMetrics fm(f);

    const int tabCount = _canvasTabVisible ? 2 : 1;
    int       x        = kPadLeft;
    for (int i = 0; i < 2; ++i) {
        auto &tab = _tabs[i];
        if (i >= tabCount) {
            tab.rect = QRect(); // hidden — not painted, not hit-tested
            continue;
        }
        const int w = kTabPadH + kIconSz + kIconGap +
                      fm.horizontalAdvance(fm.elidedText(tab.text, Qt::ElideRight, 240)) + kTabPadH;
        tab.rect = QRect(x, 4, w, kStripH - 4 - kUnderlnH - 1);
        x += w + kTabGap;
    }
}

int ConvTabsWidget::tabAt(const QPoint &pos) const {
    for (int i = 0; i < 2; ++i)
        if (_tabs[i].rect.contains(pos))
            return i;
    return -1;
}

void ConvTabsWidget::paintEvent(QPaintEvent *) {
    QPainter    p(this);
    const auto &th = Th::c();

    p.fillRect(rect(), th.surface.content);

    // Bottom divider across the full strip width.
    p.fillRect(QRect(0, height() - 1, width(), 1), th.divider.def);

    QFont f = font();
    f.setPixelSize(th.fonts.md);

    const int tabCount = _canvasTabVisible ? 2 : 1;
    for (int i = 0; i < tabCount; ++i) {
        const auto &tab    = _tabs[i];
        const bool  active = (static_cast<int>(_active) == i);

        if (!active && i == _hovered) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(th.surface.highlight);
            p.drawRoundedRect(tab.rect, 6, 6);
            p.setRenderHint(QPainter::Antialiasing, false);
        }

        f.setBold(active);
        p.setFont(f);
        const QFontMetrics fm(f);

        const QPixmap &icon = active ? tab.iconHot : tab.icon;
        const int      iy   = tab.rect.y() + (tab.rect.height() - kIconSz) / 2;
        p.drawPixmap(tab.rect.x() + kTabPadH, iy, icon);

        p.setPen(active ? th.text.primary : th.text.secondary);
        const QString elided = fm.elidedText(tab.text, Qt::ElideRight, 240);
        p.drawText(
            QRect(
                tab.rect.x() + kTabPadH + kIconSz + kIconGap,
                tab.rect.y(),
                tab.rect.width() - kTabPadH * 2 - kIconSz - kIconGap,
                tab.rect.height()
            ),
            Qt::AlignLeft | Qt::AlignVCenter,
            elided
        );

        // Active underline sits on top of the bottom divider.
        if (active)
            p.fillRect(
                QRect(tab.rect.x(), height() - kUnderlnH, tab.rect.width(), kUnderlnH),
                th.text.primary
            );
    }
}

void ConvTabsWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const int idx = tabAt(e->pos());
    if (idx < 0)
        return;
    const Tab tab = static_cast<Tab>(idx);
    if (tab == _active)
        return;
    _active = tab;
    update();
    emit tabSelected(tab);
}

void ConvTabsWidget::mouseMoveEvent(QMouseEvent *e) {
    const int idx = tabAt(e->pos());
    if (idx != _hovered) {
        _hovered = idx;
        setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ConvTabsWidget::leaveEvent(QEvent *) {
    if (_hovered != -1) {
        _hovered = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}
