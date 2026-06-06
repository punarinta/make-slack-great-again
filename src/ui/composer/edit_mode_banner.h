// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QLabel;

// Amber "Editing message" strip shown inside the composer during edit mode.
class EditModeBanner : public QWidget {
    Q_OBJECT
public:
    explicit EditModeBanner(QWidget *parent = nullptr);

signals:
    void cancelClicked();
};
