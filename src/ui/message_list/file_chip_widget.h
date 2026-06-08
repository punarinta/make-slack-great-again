// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include <QWidget>

// Paints a single non-image file chip using the canonical message-list style.
// Fixed height (52px); respects parent width up to 380px.
class FileChipWidget : public QWidget {
    Q_OBJECT
public:
    explicit FileChipWidget(const File &file, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    File _file;
};
