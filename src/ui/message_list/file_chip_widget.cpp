// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "file_chip_widget.h"
#include "message_render.h"

#include <QPainter>

FileChipWidget::FileChipWidget(const File &file, QWidget *parent) : QWidget(parent), _file(file) {
    setFixedHeight(MsgRender::fileChipHeight(_file));
    setMaximumWidth(MsgRender::fileChipMaxW(_file));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize FileChipWidget::sizeHint() const {
    return QSize(MsgRender::fileChipMaxW(_file), MsgRender::fileChipHeight(_file));
}

QSize FileChipWidget::minimumSizeHint() const {
    return QSize(120, MsgRender::fileChipHeight(_file));
}

void FileChipWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    MsgRender::paintFileChip(p, _file, QRect(0, 0, width(), MsgRender::fileChipHeight(_file)));
}
