// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Modal settings overlay — full-window child widget that dims the background.
// No compositor required; uses Qt's backing-store alpha blending.
#pragma once

#include <QWidget>
#include <QPoint>
#include <QRect>

class QFrame;
class QLabel;
class QListWidget;
class QStackedWidget;
class QCheckBox;
class QRadioButton;

class SettingsDialog : public QWidget {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);

    void open();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    enum class Dir { None, N, NE, E, SE, S, SW, W, NW };

    void buildPanel();
    void saveNotifications();
    void loadNotifications();
    void refreshCacheSize();
    void clearCache();
    void updatePanelGeometry();
    Dir  edgeAt(const QPoint &pos) const;
    static Qt::CursorShape cursorFor(Dir d);

    QFrame         *_panel   = nullptr;
    QListWidget    *_tabs    = nullptr;
    QStackedWidget *_stack   = nullptr;

    // Notification controls
    QCheckBox      *_notifEnabled  = nullptr;
    QRadioButton   *_notifAll      = nullptr;
    QRadioButton   *_notifMentions = nullptr;
    QCheckBox      *_notifSound    = nullptr;

    // Storage controls
    QLabel         *_cacheSize     = nullptr;

    Dir    _resizeDir  = Dir::None;
    QPoint _dragStart;
    QRect  _panelAtDrag;
};
