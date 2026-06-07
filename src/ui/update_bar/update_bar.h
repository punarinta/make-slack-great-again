// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

// Thin bar shown immediately below the title bar when an update has been
// downloaded. Hidden by default; call showUpdateReady() to display it.
class UpdateBar : public QWidget {
    Q_OBJECT
public:
    explicit UpdateBar(QWidget *parent = nullptr);

    void showUpdateReady(const QString &stagedPath);

signals:
    void restartRequested(const QString &stagedPath);

private:
    QLabel      *_label  = nullptr;
    QPushButton *_btn    = nullptr;
    QString      _staged;
};
