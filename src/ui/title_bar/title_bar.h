// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QWidget>

class QLabel;
class QPushButton;
class PopupTooltip;

// Custom title bar for the frameless main window.
// Handles drag-to-move (startSystemMove), double-click maximize/restore,
// and window state changes (updates the max/restore button icon).
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget *parent = nullptr);

    void setTitle(const QString &title);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void applyTheme();
    void updateMaxButton();
    void togglePin();
    void updatePinButton();
    void refreshHoverState();

    QLabel       *_titleLabel        = nullptr;
    QPushButton  *_minBtn            = nullptr;
    QPushButton  *_maxBtn            = nullptr;
    QPushButton  *_closeBtn          = nullptr;
    QPushButton  *_pinBtn            = nullptr;
    PopupTooltip *_tooltip           = nullptr;
    bool          _pinned            = false;
    bool          _dragging          = false; // manual drag (non-Wayland)
    bool          _systemMovePending = false; // startSystemMove() in flight (Wayland)
    QPoint        _dragOffset;
};
