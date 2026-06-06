// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPixmap>
#include <QWidget>
#include <functional>
#include <vector>

// Telegram-styled floating context menu.
// Frameless, translucent, with soft painted shadow and 8px rounded corners.
// Dismisses on click-outside (Qt::Popup behaviour).
class ContextMenu : public QWidget {
    Q_OBJECT
public:
    struct Item {
        QString text;
        QString shortcut;               // right-aligned hint (e.g. "E", "Ctrl+C")
        bool submenu    = false;        // show "›" indicator on the right
        std::function<void()> action;
        bool destructive = false;       // draws label in red
        bool separator   = false;       // thin divider; other fields ignored
        QPixmap icon;                   // optional 16×16 icon rendered to the left of text
    };

    explicit ContextMenu(QWidget *parent = nullptr);

    // icon: optional SVG path (e.g. ":/ui/edit-3.svg"); pass empty string for no icon.
    void addItem(const QString &text, std::function<void()> action,
                 bool destructive = false, const QString &iconPath = {});
    void addItem(const QString &text, const QString &shortcut,
                 std::function<void()> action, bool destructive = false,
                 bool submenu = false, const QString &iconPath = {});
    void addSeparator();

    // Shows the menu anchored below-right of globalPos.
    // Flips direction automatically if near screen edge.
    void popup(const QPoint &globalPos);

    static constexpr int kIconSize   = 16;   // icon square size (public for use in .cpp)

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    int  hoveredAt(const QPoint &widgetPos) const; // index or -1
    QRect itemRect(int i) const;    // in widget coords (inside shadow padding)
    QRect menuRect() const;         // white menu rect in widget coords
    void  updateGeometry(const QPoint &globalPos);

    std::vector<Item> _items;
    int _hovered  = -1;
    int _pressed  = -1;

    // Layout
    static constexpr int kItemH      = 36;   // row height for a normal item
    static constexpr int kSepH       =  9;   // height of a separator row
    static constexpr int kPadH       = 12;   // horizontal text padding
    static constexpr int kIconGap    =  8;   // gap between icon and text
    static constexpr int kPadV       =  6;   // top/bottom inner padding
    static constexpr int kRadius     =  8;   // corner radius
    static constexpr int kShadow     =  8;   // shadow halo width (transparent padding)
    static constexpr int kMinW       = 200;  // minimum menu width
    static constexpr int kShortcutGap = 24;  // min space between label and shortcut

    int itemH(int i) const;        // height of item i (kItemH or kSepH)
    int itemTop(int i) const;      // cumulative top in menu-card coords
    int totalItemsH() const;       // sum of all item heights
};
