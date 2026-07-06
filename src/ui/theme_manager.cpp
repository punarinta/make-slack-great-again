// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme_manager.h"

#include <QCursor>
#include <QGuiApplication>
#include <QSettings>

ThemeManager &ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent) {
    // The singleton is first touched (via Th::c()) before any widget paints,
    // so the persisted theme is active from the very first frame.
    _themeId =
        QSettings("msga", "msga").value("appearance/theme", QStringLiteral("purple")).toString();
    const Th::Theme *theme = Th::themeById(_themeId);
    if (!theme) { // unknown/stale id from a future or older version
        _themeId = QStringLiteral("purple");
        theme    = &Th::defaultTheme();
    }
    _theme = *theme;
}

void ThemeManager::setTheme(const Th::Theme &theme) {
    // Applying a theme re-polishes the whole widget tree synchronously (rebuilt
    // stylesheets, re-baked pixmaps, reset message docs) and blocks the main
    // thread for a noticeable beat. Show the wait cursor for the duration so the
    // switch reads as "working" rather than frozen.
    QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    _theme = theme;
    emit themeChanged();
    QGuiApplication::restoreOverrideCursor();
}

void ThemeManager::setThemeById(const QString &id) {
    const Th::Theme *theme = Th::themeById(id);
    if (!theme || id == _themeId)
        return;
    _themeId = id;
    QSettings("msga", "msga").setValue("appearance/theme", id);
    setTheme(*theme);
}
