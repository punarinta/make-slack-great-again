// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme_manager.h"

ThemeManager &ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent), _theme(Th::defaultTheme()) {}

void ThemeManager::setTheme(const Th::Theme &theme) {
    _theme = theme;
    emit themeChanged();
}
