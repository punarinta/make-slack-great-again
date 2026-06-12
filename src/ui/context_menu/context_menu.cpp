// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "context_menu.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QFontMetrics>

ContextMenu::ContextMenu(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
}

// NOTE (V1 limitation): icon QPixmaps stored in Item::icon are rasterised with
// the theme color at addItem() time. They will NOT live-update when the theme
// changes — the menu is recreated on each popup() call so this is acceptable.
static QPixmap loadMenuIcon(const QString &path) {
    if (path.isEmpty())
        return {};
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    return svgPixmapPhys(
        path, QSize(ContextMenu::kIconSize, ContextMenu::kIconSize), Th::c().icon.strong, dpr
    );
}

void ContextMenu::addHeader(const QString &text) {
    Item it;
    it.text   = text;
    it.header = true;
    _items.push_back(std::move(it));
}

void ContextMenu::addItem(
    const QString        &text,
    std::function<void()> action,
    bool                  destructive,
    const QString        &iconPath,
    bool                  selected
) {
    Item it;
    it.text        = text;
    it.action      = std::move(action);
    it.destructive = destructive;
    it.icon        = loadMenuIcon(iconPath);
    it.selected    = selected;
    if (selected)
        _hasChecked = true;
    _items.push_back(std::move(it));
}

void ContextMenu::addItem(
    const QString        &text,
    const QString        &shortcut,
    std::function<void()> action,
    bool                  destructive,
    bool                  submenu,
    const QString        &iconPath
) {
    Item it;
    it.text        = text;
    it.shortcut    = shortcut;
    it.submenu     = submenu;
    it.action      = std::move(action);
    it.destructive = destructive;
    it.icon        = loadMenuIcon(iconPath);
    _items.push_back(std::move(it));
}

void ContextMenu::addSeparator() {
    _items.push_back({{}, {}, false, nullptr, false, true});
}

// ── Geometry ──────────────────────────────────────────────────────────────────

int ContextMenu::itemH(int i) const {
    if (_items[i].separator)
        return kSepH;
    if (_items[i].header)
        return kHeaderH;
    return kItemH;
}

int ContextMenu::itemTop(int i) const {
    const QRect mr = menuRect();
    int         y  = mr.top() + kPadV;
    for (int j = 0; j < i; ++j)
        y += itemH(j);
    return y;
}

int ContextMenu::totalItemsH() const {
    int h = 0;
    for (int i = 0; i < (int)_items.size(); ++i)
        h += itemH(i);
    return h;
}

QRect ContextMenu::menuRect() const {
    return QRect(kShadow, kShadow, width() - 2 * kShadow, height() - 2 * kShadow);
}

QRect ContextMenu::itemRect(int i) const {
    const QRect mr = menuRect();
    return QRect(mr.left(), itemTop(i), mr.width(), itemH(i));
}

void ContextMenu::updateGeometry(const QPoint &globalPos) {
    const QFontMetrics fm(QApplication::font());
    QFont              shortcutFont = QApplication::font();
    shortcutFont.setPointSizeF(shortcutFont.pointSizeF() * 0.85);
    const QFontMetrics sfm(shortcutFont);

    const int checkInset = _hasChecked ? kCheckW : 0;
    int       w          = kMinW;
    for (const auto &it : _items) {
        if (it.separator || it.header)
            continue;
        const int iconW = it.icon.isNull() ? 0 : (kIconSize + kIconGap);
        int       itemW = kPadH + checkInset + iconW + fm.horizontalAdvance(it.text) + kPadH;
        if (!it.shortcut.isEmpty())
            itemW += kShortcutGap + sfm.horizontalAdvance(it.shortcut) + kPadH;
        if (it.submenu)
            itemW += kShortcutGap + sfm.horizontalAdvance("›") + kPadH;
        w = std::max(w, itemW);
    }

    const int h      = 2 * kPadV + totalItemsH();
    const int totalW = w + 2 * kShadow;
    const int totalH = h + 2 * kShadow;

    // Anchor: prefer below-right of the click point; flip if near screen edge.
    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect avail = screen->availableGeometry();

    int x = globalPos.x();
    int y = globalPos.y();
    if (x + totalW > avail.right())
        x = globalPos.x() - totalW;
    if (y + totalH > avail.bottom())
        y = globalPos.y() - totalH;
    x = std::max(avail.left(), x);
    y = std::max(avail.top(), y);

    setGeometry(x, y, totalW, totalH);
}

void ContextMenu::popup(const QPoint &globalPos) {
    updateGeometry(globalPos);
    show();
    raise();
    activateWindow();
}

// ── Outside-click dismissal ───────────────────────────────────────────────────
// Qt::Popup grab is unreliable on Wayland, so we watch every mouse press at
// the application level and close when the click falls outside our bounds.

void ContextMenu::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    qApp->installEventFilter(this);
}

void ContextMenu::hideEvent(QHideEvent *e) {
    qApp->removeEventFilter(this);
    QWidget::hideEvent(e);
}

bool ContextMenu::eventFilter(QObject *, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *me = static_cast<const QMouseEvent *>(event);
        if (!geometry().contains(me->globalPosition().toPoint()))
            close();
    }
    return false; // never consume — let the click reach its target
}

// ── Painting ──────────────────────────────────────────────────────────────────

void ContextMenu::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect  mr = menuRect();
    const QRectF mrf(mr);

    // ── Soft shadow (concentric translucent halos) ────────────────────────
    for (int i = kShadow; i >= 1; --i) {
        const int alpha = 2 + (kShadow - i) * 2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, alpha));
        const qreal r = kRadius + i;
        p.drawRoundedRect(mrf.adjusted(-i + 0.5, -i + 0.5, i - 0.5, i - 0.5), r, r);
    }

    // ── Menu card ─────────────────────────────────────────────────────────
    p.setBrush(Th::c().contextMenu.bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(mrf, kRadius, kRadius);

    // ── Items ─────────────────────────────────────────────────────────────
    const QFont baseFont   = QApplication::font();
    QFont       headerFont = baseFont;
    headerFont.setPointSizeF(headerFont.pointSizeF() * 0.82);
    QFont shortcutFont = baseFont;
    shortcutFont.setPointSizeF(shortcutFont.pointSizeF() * 0.85);
    const QFontMetrics fm(baseFont);
    const QFontMetrics sfm(shortcutFont);

    // When any item carries a checkmark, all content shifts right to make room.
    const int checkInset = _hasChecked ? kCheckW : 0;

    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        const QRect ir = itemRect(i);

        if (_items[i].separator) {
            const int lineY = ir.top() + ir.height() / 2;
            p.setPen(QColor(0, 0, 0, 18));
            p.drawLine(mr.left() + kPadH / 2, lineY, mr.right() - kPadH / 2, lineY);
            continue;
        }

        if (_items[i].header) {
            p.setFont(headerFont);
            p.setPen(Th::c().contextMenu.itemTextDim);
            p.drawText(
                ir.adjusted(kPadH + checkInset, 0, -kPadH, -2),
                Qt::AlignLeft | Qt::AlignBottom,
                _items[i].text
            );
            continue;
        }

        // Hover background
        if (i == _hovered) {
            QPainterPath clip;
            clip.addRoundedRect(mrf, kRadius, kRadius);
            p.setClipPath(clip);
            p.fillRect(ir, QColor(0x00, 0x00, 0x00, 12));
            p.setClipping(false);
        }

        const bool   isSelected = _items[i].selected;
        const QColor accentCol  = Th::c().accent.def;
        const QColor textColor  = _items[i].destructive ? Th::c().contextMenu.dangerText
                                  : isSelected          ? accentCol
                                                        : Th::c().contextMenu.itemText;

        // Checkmark for the selected item
        if (isSelected) {
            const int cx = ir.left() + kPadH / 2 + checkInset / 2 - 1;
            const int cy = ir.top() + ir.height() / 2;
            p.setPen(QPen(accentCol, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(cx - 4, cy + 1, cx - 1, cy + 4);
            p.drawLine(cx - 1, cy + 4, cx + 5, cy - 3);
        }

        // Right-side hint (shortcut or submenu arrow)
        if (!_items[i].shortcut.isEmpty()) {
            p.setFont(shortcutFont);
            p.setPen(Th::c().contextMenu.itemTextDim);
            p.drawText(
                QRect(ir.left(), ir.top(), ir.width() - kPadH, ir.height()),
                Qt::AlignVCenter | Qt::AlignRight,
                _items[i].shortcut
            );
        } else if (_items[i].submenu) {
            p.setFont(shortcutFont);
            p.setPen(Th::c().contextMenu.itemTextDim);
            p.drawText(
                QRect(ir.left(), ir.top(), ir.width() - kPadH, ir.height()),
                Qt::AlignVCenter | Qt::AlignRight,
                "›"
            );
        }

        // Icon — recolorised in accent when selected
        const int contentX = ir.left() + kPadH + checkInset;
        const int iconW    = _items[i].icon.isNull() ? 0 : (kIconSize + kIconGap);
        if (!_items[i].icon.isNull()) {
            const int iconY = ir.top() + (ir.height() - kIconSize) / 2;
            if (isSelected) {
                // Recolourise the icon pixmap to accent color at paint time.
                // icon.size() is physical (DPR-scaled) while tinted has DPR 1 —
                // draw into the full rect so the icon isn't shrunk by the DPR.
                QPixmap tinted(_items[i].icon.size());
                tinted.fill(Qt::transparent);
                QPainter tp(&tinted);
                tp.drawPixmap(tinted.rect(), _items[i].icon);
                tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tp.fillRect(tinted.rect(), accentCol);
                tp.end();
                p.drawPixmap(QRect(contentX, iconY, kIconSize, kIconSize), tinted);
            } else {
                p.drawPixmap(QRect(contentX, iconY, kIconSize, kIconSize), _items[i].icon);
            }
        }

        // Label
        p.setFont(baseFont);
        p.setPen(textColor);
        const int rightHintW =
            !_items[i].shortcut.isEmpty()
                ? kShortcutGap + sfm.horizontalAdvance(_items[i].shortcut)
                : (_items[i].submenu ? kShortcutGap + sfm.horizontalAdvance("›") : 0);
        const int availW = ir.width() - kPadH - checkInset - iconW - kPadH - rightHintW;
        p.drawText(
            QRect(contentX + iconW, ir.top(), availW, ir.height()),
            Qt::AlignVCenter | Qt::AlignLeft,
            fm.elidedText(_items[i].text, Qt::ElideRight, availW)
        );
    }
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

int ContextMenu::hoveredAt(const QPoint &pos) const {
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        if (_items[i].separator || _items[i].header)
            continue;
        if (itemRect(i).contains(pos))
            return i;
    }
    return -1;
}

void ContextMenu::mouseMoveEvent(QMouseEvent *e) {
    const int h = hoveredAt(e->pos());
    if (h != _hovered) {
        _hovered = h;
        update();
    }
}

void ContextMenu::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton)
        _pressed = hoveredAt(e->pos());
}

void ContextMenu::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const int i = hoveredAt(e->pos());
    if (i >= 0 && i == _pressed && _items[i].action) {
        close();
        _items[i].action(); // fire after close so the menu is gone first
    }
    _pressed = -1;
}

void ContextMenu::leaveEvent(QEvent *) {
    _hovered = -1;
    update();
}

void ContextMenu::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }

    for (const auto &item : _items) {
        if (item.separator || item.shortcut.isEmpty() || !item.action)
            continue;
        const QKeySequence seq(item.shortcut);
        if (seq.isEmpty())
            continue;
        const QKeyCombination       combo   = seq[0];
        const Qt::KeyboardModifiers reqMod  = combo.keyboardModifiers();
        const Qt::KeyboardModifiers presMod = e->modifiers();
        // Modifier-free shortcuts (e.g. "E", "Del") are case-insensitive — ignore Shift.
        const bool                  modOk   = reqMod == Qt::NoModifier
                                                  ? (presMod & ~Qt::ShiftModifier) == Qt::NoModifier
                                                  : presMod == reqMod;
        if (modOk && e->key() == combo.key()) {
            close();
            item.action();
            return;
        }
    }

    QWidget::keyPressEvent(e);
}
