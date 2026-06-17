// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "dropdown.h"

#include "ui/context_menu/context_menu.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

namespace {
constexpr int kPadH    = 12; // left/right text padding
constexpr int kPadV    = 8;  // top/bottom padding
constexpr int kChevron = 16; // chevron icon side
constexpr int kGap     = 8;  // gap between text and chevron
} // namespace

Dropdown::Dropdown(QWidget *parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    applyStyle();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyStyle(); });
}

// Render the border + background through Qt's stylesheet engine (identical to
// the dialog's text inputs) rather than a hand-painted stroke — antialiased
// QPainter strokes read noticeably lighter than QSS-rendered borders.
void Dropdown::applyStyle() {
    const bool   active = _hover || _open;
    const QColor border = active ? Th::c().composer.borderFocus : Th::c().composer.border;
    setStyleSheet(QString(
                      "Dropdown {"
                      "  border: %1px solid %2; border-radius: 6px; background: %3;"
                      "}"
    )
                      .arg(active ? 2 : 1)
                      .arg(Th::qss(border), Th::qss(Th::c().surface.raised)));
    update();
}

void Dropdown::addItem(const QString &text) {
    _items.push_back(text);
    if (_current < 0)
        _current = 0;
    updateGeometry();
    update();
}

void Dropdown::setItems(const QStringList &items) {
    _items   = items;
    _current = items.isEmpty() ? -1 : 0;
    updateGeometry();
    update();
}

void Dropdown::setCurrentIndex(int index) {
    if (index < 0 || index >= _items.size() || index == _current)
        return;
    _current = index;
    update();
    emit currentIndexChanged(_current);
}

QString Dropdown::currentText() const {
    return (_current >= 0 && _current < _items.size()) ? _items.at(_current) : QString();
}

QSize Dropdown::sizeHint() const {
    const QFontMetrics fm(font());
    int                widest = 0;
    for (const QString &s : _items)
        widest = qMax(widest, fm.horizontalAdvance(s));
    return QSize(kPadH + widest + kGap + kChevron + kPadH, fm.height() + 2 * kPadV);
}

void Dropdown::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Border + background via the stylesheet engine (matches the text inputs).
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    // Current selection text (elided to fit before the chevron).
    const QRect textRect(kPadH, 0, width() - 2 * kPadH - kChevron - kGap, height());
    p.setPen(Th::c().text.primary);
    const QString label =
        p.fontMetrics().elidedText(currentText(), Qt::ElideRight, textRect.width());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    // Chevron on the right.
    const qreal   dpr     = devicePixelRatioF();
    const QPixmap chevron = svgPixmapPhys(
        QStringLiteral(":/ui/chevron-down.svg"),
        QSize(kChevron, kChevron),
        Th::c().text.secondary,
        dpr
    );
    const QRect chevRect(width() - kPadH - kChevron, (height() - kChevron) / 2, kChevron, kChevron);
    p.drawPixmap(chevRect, chevron);
}

void Dropdown::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton)
        openMenu();
}

void Dropdown::enterEvent(QEnterEvent *) {
    _hover = true;
    applyStyle();
}

void Dropdown::leaveEvent(QEvent *) {
    _hover = false;
    applyStyle();
}

void Dropdown::openMenu() {
    if (_items.isEmpty())
        return;
    auto *menu = new ContextMenu(this);
    for (int i = 0; i < _items.size(); ++i)
        menu->addItem(
            _items.at(i),
            [this, i] { setCurrentIndex(i); },
            /*destructive=*/false,
            /*iconPath=*/{},
            /*selected=*/i == _current
        );
    _open = true;
    applyStyle();
    // The popup self-deletes on close (WA_DeleteOnClose); clear our highlight then.
    connect(menu, &QObject::destroyed, this, [this] {
        _open = false;
        applyStyle();
    });
    // Anchor flush under the field, left-aligned (ContextMenu flips up near edges).
    menu->popup(mapToGlobal(QPoint(0, height() + 2)));
}
