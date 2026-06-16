// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QString>

namespace Clipboard {

inline void setText(const QString &text) {
    QApplication::clipboard()->setText(text);
}

inline void setImage(const QImage &image) {
    QApplication::clipboard()->setImage(image);
}

} // namespace Clipboard
