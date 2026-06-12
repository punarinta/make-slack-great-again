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
    const QString   &themeId() const { return _themeId; }

    // Replace the active theme and notify all subscribers.
    void setTheme(const Th::Theme &theme);

    // Switch to a registry theme by id ("purple"/"blue"), persist the choice
    // (QSettings "appearance/theme") and notify. Unknown ids are ignored.
    void setThemeById(const QString &id);

signals:
    void themeChanged();

private:
    explicit ThemeManager(QObject *parent = nullptr);

    Th::Theme _theme;
    QString   _themeId;
};
