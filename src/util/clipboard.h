// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QApplication>
#include <QClipboard>
#include <QString>

namespace Clipboard {

inline void setText(const QString &text) {
    QApplication::clipboard()->setText(text);
}

} // namespace Clipboard
