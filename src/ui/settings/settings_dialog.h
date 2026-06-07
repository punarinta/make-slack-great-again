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
class QPushButton;
class QSpinBox;
class UpdateChecker;

class SettingsDialog : public QWidget {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);

    void open();
    void setUpdateChecker(UpdateChecker *checker);

signals:
    // Emitted when appearance settings are saved; carries the new relevantDays value.
    void appearanceChanged(int relevantDays);
    // Emitted after conv/visitedAt is wiped so the conv list can re-seed from API data.
    void stateCleared();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    enum class Dir { None, N, NE, E, SE, S, SW, W, NW };

    void                   buildPanel();
    void                   saveNotifications();
    void                   loadNotifications();
    void                   saveAppearance();
    void                   loadAppearance();
    void                   refreshCacheSize();
    void                   clearCache();
    void                   clearState();
    void                   refreshLastChecked();
    void                   refreshUpdateStatus();
    void                   updatePanelGeometry();
    Dir                    edgeAt(const QPoint &pos) const;
    static Qt::CursorShape cursorFor(Dir d);

    QFrame         *_panel = nullptr;
    QListWidget    *_tabs  = nullptr;
    QStackedWidget *_stack = nullptr;

    // Notification controls
    QCheckBox    *_notifEnabled  = nullptr;
    QRadioButton *_notifAll      = nullptr;
    QRadioButton *_notifMentions = nullptr;
    QCheckBox    *_notifSound    = nullptr;

    // Appearance controls
    QSpinBox *_relevantDays = nullptr;

    // Storage controls
    QLabel *_cacheSize = nullptr;

    // System / update controls
    UpdateChecker *_updateChecker = nullptr;
    QLabel        *_updateStatus  = nullptr;
    QLabel        *_lastChecked   = nullptr;
    QPushButton   *_checkBtn      = nullptr;

    Dir    _resizeDir = Dir::None;
    QPoint _dragStart;
    QRect  _panelAtDrag;
};
