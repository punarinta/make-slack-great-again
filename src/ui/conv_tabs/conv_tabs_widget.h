// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPixmap>
#include <QWidget>

// Slack-style tab strip under the conversation header: "Messages" plus the
// channel canvas tab. With no canvas the second tab reads "Add canvas" with a
// sticky-note-plus icon; with one it shows the canvas title. The active tab
// gets a 2px underline sitting on the strip's bottom divider.
class ConvTabsWidget : public QWidget {
    Q_OBJECT
public:
    enum class Tab { Messages, Canvas };

    explicit ConvTabsWidget(QWidget *parent = nullptr);

    Tab  activeTab() const { return _active; }
    void setActiveTab(Tab tab); // visual only; does not emit tabSelected

    // hasCanvas=false → "Add canvas"; true → title ("Untitled" when empty).
    void setCanvasInfo(bool hasCanvas, const QString &title = {});

signals:
    void tabSelected(Tab tab);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    struct TabSpec {
        QString text;
        QPixmap icon;    // colored for the inactive state
        QPixmap iconHot; // colored for the active state
        QRect   rect;    // computed in relayout()
    };

    void relayout();
    void rebuildIcons();
    int  tabAt(const QPoint &pos) const;

    Tab     _active    = Tab::Messages;
    bool    _hasCanvas = false;
    QString _canvasTitle;
    int     _hovered = -1;

    TabSpec _tabs[2];

    static constexpr int kStripH   = 38; // widget height incl. bottom divider
    static constexpr int kPadLeft  = 16; // left inset, aligns with header text
    static constexpr int kTabPadH  = 10; // horizontal padding inside a tab
    static constexpr int kTabGap   = 4;  // gap between tabs
    static constexpr int kIconSz   = 15;
    static constexpr int kIconGap  = 6;
    static constexpr int kUnderlnH = 2;
};
