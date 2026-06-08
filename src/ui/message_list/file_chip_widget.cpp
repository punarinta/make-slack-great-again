// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "file_chip_widget.h"
#include "message_render.h"

#include <QPainter>

FileChipWidget::FileChipWidget(const File &file, QWidget *parent) : QWidget(parent), _file(file) {
    setFixedHeight(MsgRender::kFileChipH);
    setMaximumWidth(MsgRender::kFileChipMaxW);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize FileChipWidget::sizeHint() const {
    return QSize(MsgRender::kFileChipMaxW, MsgRender::kFileChipH);
}

QSize FileChipWidget::minimumSizeHint() const {
    return QSize(120, MsgRender::kFileChipH);
}

void FileChipWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    MsgRender::paintFileChip(p, _file, QRect(0, 0, width(), MsgRender::kFileChipH));
}
