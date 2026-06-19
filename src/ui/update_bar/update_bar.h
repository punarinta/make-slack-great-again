// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QLabel;
class StyledButton;

// Thin bar shown immediately below the title bar when an update has been
// downloaded. Hidden by default; call showUpdateReady() to display it.
class UpdateBar : public QWidget {
    Q_OBJECT
public:
    explicit UpdateBar(QWidget *parent = nullptr);

    void showUpdateReady();

signals:
    void restartRequested();

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void applyTheme();

    QLabel       *_label = nullptr;
    StyledButton *_btn   = nullptr;
};
