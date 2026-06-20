// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "dropdown.h"

#include "ui/context_menu/context_menu.h"
#include "ui/control_metrics.h"
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
    setFixedHeight(Ui::kControlHeight); // align with StyledButton / StyledLineEdit
    applyStyle();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyStyle(); });
}

void Dropdown::setSize(Size size) {
    _size = size;
    setFixedHeight(size == Size::Small ? Ui::kControlHeightSmall : Ui::kControlHeight);
    updateGeometry();
}

// Render the border + background through Qt's stylesheet engine (identical to
// the dialog's text inputs) rather than a hand-painted stroke — antialiased
// QPainter strokes read noticeably lighter than QSS-rendered borders.
void Dropdown::applyStyle() {
    const bool   active = isEnabled() && (_hover || _open);
    const QColor border = active ? Th::c().composer.borderFocus : Th::c().composer.border;
    const QColor bg     = isEnabled() ? Th::c().surface.raised : Th::c().surface.sunken;
    setStyleSheet(QString(
                      "Dropdown {"
                      "  border: %1px solid %2; border-radius: 6px; background: %3;"
                      "}"
    )
                      .arg(active ? 2 : 1)
                      .arg(Th::qss(border), Th::qss(bg)));
    update();
}

void Dropdown::changeEvent(QEvent *e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::EnabledChange)
        applyStyle(); // repaint border/background + (via update) the dimmed text
}

void Dropdown::addItem(const QString &text, const QVariant &data) {
    _items.push_back(text);
    _data.push_back(data);
    _isSep.push_back(false);
    if (_current < 0)
        _current = 0;
    updateGeometry();
    update();
}

void Dropdown::addSeparator() {
    _items.push_back(QString());
    _data.push_back(QVariant());
    _isSep.push_back(true);
}

void Dropdown::setItems(const QStringList &items) {
    _items   = items;
    _data    = QList<QVariant>(items.size());
    _isSep   = QList<bool>(items.size(), false);
    _current = items.isEmpty() ? -1 : 0;
    updateGeometry();
    update();
}

void Dropdown::clear() {
    _items.clear();
    _data.clear();
    _isSep.clear();
    _current = -1;
    updateGeometry();
    update();
}

void Dropdown::setCurrentIndex(int index) {
    if (index < 0 || index >= _items.size() || index == _current || _isSep.at(index))
        return;
    _current = index;
    update();
    emit currentIndexChanged(_current);
}

QString Dropdown::currentText() const {
    return (_current >= 0 && _current < _items.size()) ? _items.at(_current) : QString();
}

QVariant Dropdown::currentData() const {
    return (_current >= 0 && _current < _data.size()) ? _data.at(_current) : QVariant();
}

int Dropdown::findData(const QVariant &data) const {
    for (int i = 0; i < _data.size(); ++i)
        if (!_isSep.at(i) && _data.at(i) == data)
            return i;
    return -1;
}

QSize Dropdown::sizeHint() const {
    const QFontMetrics fm(font());
    int                widest = 0;
    for (const QString &s : _items)
        widest = qMax(widest, fm.horizontalAdvance(s));
    // Height comes from the shared control-height constant (set as fixed height in
    // the ctor / setSize), so dropdowns line up with the inputs and buttons beside
    // them instead of being sized by font metrics.
    const int h = _size == Size::Small ? Ui::kControlHeightSmall : Ui::kControlHeight;
    return QSize(kPadH + widest + kGap + kChevron + kPadH, h);
}

void Dropdown::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Border + background via the stylesheet engine (matches the text inputs).
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    // Disabled fields dim their text + chevron so the whole row reads as inactive
    // (input is already blocked by Qt; this is the matching visual cue).
    const bool enabled = isEnabled();

    // Current selection text (elided to fit before the chevron).
    const QRect textRect(kPadH, 0, width() - 2 * kPadH - kChevron - kGap, height());
    p.setPen(enabled ? Th::c().text.primary : Th::c().text.tertiary);
    const QString label =
        p.fontMetrics().elidedText(currentText(), Qt::ElideRight, textRect.width());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    // Chevron on the right.
    const qreal   dpr     = devicePixelRatioF();
    const QPixmap chevron = svgPixmapPhys(
        QStringLiteral(":/ui/chevron-down.svg"),
        QSize(kChevron, kChevron),
        enabled ? Th::c().text.secondary : Th::c().text.tertiary,
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
    for (int i = 0; i < _items.size(); ++i) {
        if (_isSep.at(i)) {
            menu->addSeparator();
            continue;
        }
        menu->addItem(
            _items.at(i),
            [this, i] { setCurrentIndex(i); },
            /*destructive=*/false,
            /*iconPath=*/{},
            /*selected=*/i == _current
        );
    }
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
