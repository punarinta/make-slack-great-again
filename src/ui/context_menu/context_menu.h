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
        QString               text;
        QString               shortcut;        // right-aligned hint (e.g. "E", "Ctrl+C")
        bool                  submenu = false; // show "›" indicator on the right
        std::function<void()> action;
        bool                  destructive = false; // draws label in red
        bool                  separator   = false; // thin divider; other fields ignored
        bool                  header      = false; // non-clickable section label
        bool                  selected    = false; // checkmark + accent color
        QPixmap               icon; // optional 16×16 icon rendered to the left of text
    };

    // Controls how the menu width is chosen.
    //  - Fit:      width is purely content-driven (default). The menu is exactly
    //              as wide as its longest row needs; never artificially limited.
    //  - MinWidth: as Fit, but never narrower than kMinW — used for the workspace
    //              switcher ("tray bar") menus whose rows are short, so they don't
    //              render as a cramped sliver.
    enum class WidthMode { Fit, MinWidth };

    explicit ContextMenu(QWidget *parent = nullptr);

    void setWidthMode(WidthMode mode) { _widthMode = mode; }

    // Non-clickable section label (e.g. "Notify you about…").
    void addHeader(const QString &text);

    // icon: optional SVG path (e.g. ":/ui/edit-3.svg"); pass empty string for no icon.
    // selected: draws a checkmark to the left and renders text+icon in accent color.
    void addItem(
        const QString        &text,
        std::function<void()> action,
        bool                  destructive = false,
        const QString        &iconPath    = {},
        bool                  selected    = false
    );
    void addItem(
        const QString        &text,
        const QString        &shortcut,
        std::function<void()> action,
        bool                  destructive = false,
        bool                  submenu     = false,
        const QString        &iconPath    = {}
    );
    void addSeparator();

    // Shows the menu anchored below-right of globalPos.
    // Flips direction automatically if near screen edge.
    void popup(const QPoint &globalPos);

    static constexpr int kIconSize = 16; // icon square size (public for use in .cpp)
    static constexpr int kCheckW = 16; // checkmark zone width (reserved when any item is selected)

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
    int   hoveredAt(const QPoint &widgetPos) const; // index or -1
    QRect itemRect(int i) const;                    // in widget coords (inside shadow padding)
    QRect menuRect() const;                         // white menu rect in widget coords
    void  updateGeometry(const QPoint &globalPos);

    std::vector<Item> _items;
    int               _hovered    = -1;
    int               _pressed    = -1;
    bool              _hasChecked = false; // true if any item has selected=true
    WidthMode         _widthMode  = WidthMode::Fit;

    // Layout
    static constexpr int kItemH       = 36;  // row height for a normal item
    static constexpr int kHeaderH     = 26;  // height of a non-clickable section label
    static constexpr int kSepH        = 9;   // height of a separator row
    static constexpr int kPadH        = 12;  // horizontal text padding
    static constexpr int kIconGap     = 8;   // gap between icon and text
    static constexpr int kPadV        = 6;   // top/bottom inner padding
    static constexpr int kRadius      = 8;   // corner radius
    static constexpr int kShadow      = 8;   // shadow halo width (transparent padding)
    static constexpr int kMinW        = 140; // floor applied only in WidthMode::MinWidth
    static constexpr int kLabelSlack  = 4;   // headroom so the widest label never elides
    static constexpr int kShortcutGap = 24;  // min space between label and shortcut

    int itemH(int i) const;   // height of item i (kItemH, kHeaderH, or kSepH)
    int itemTop(int i) const; // cumulative top in menu-card coords
    int totalItemsH() const;  // sum of all item heights
};
