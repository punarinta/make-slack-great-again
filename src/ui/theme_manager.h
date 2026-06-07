// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "theme.h"
#include <QObject>

namespace Th {
const Theme &defaultTheme();
}

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager &instance();

    const Th::Theme &theme() const { return _theme; }

    // Replace the active theme and notify all subscribers.
    void setTheme(const Th::Theme &theme);

signals:
    void themeChanged();

private:
    explicit ThemeManager(QObject *parent = nullptr);

    Th::Theme _theme;
};
