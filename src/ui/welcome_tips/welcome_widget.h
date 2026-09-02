// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QWidget>
#include <QList>

class QLabel;
class QFrame;

class WelcomeWidget : public QWidget {
    Q_OBJECT
public:
    explicit WelcomeWidget(QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void applyTheme();
    void repositionContent();

    QWidget         *_content = nullptr;
    QLabel          *_title   = nullptr;
    QFrame          *_rule    = nullptr;
    QList<QLabel *>  _chipLabels;
    QList<QLabel *>  _plusLabels;
    QList<QLabel *>  _actionLabels;
    // One widget per shortcut row, in registry order — repositionContent() hides
    // trailing ones when the panel is taller than the space it has.
    QList<QWidget *> _rows;
};
