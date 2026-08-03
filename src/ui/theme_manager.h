// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "theme.h"
#include <QFont>
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

    // UI font size variant ("small" | "medium" | "large"): a multiplier applied
    // over the active theme's px font scale, so every widget that sizes text
    // from Th::c().fonts follows. Persisted (QSettings "appearance/fontSize");
    // setting it re-derives the theme from its pristine registry entry (the
    // scale never compounds) and notifies via themeChanged.
    const QString &fontSizeId() const { return _fontSizeId; }
    void           setFontSizeId(const QString &id);
    // The active multiplier (0.9 / 1.0 / 1.15) for text-driven GEOMETRY that
    // isn't sized from a font (fixed row heights, etc.). Text itself should
    // derive from Th::c().fonts or QApplication::font(), not from this.
    double         fontFactor() const;

signals:
    void themeChanged();

private:
    explicit ThemeManager(QObject *parent = nullptr);

    // Scale the APPLICATION font from _baseAppFont by the active factor. The
    // message renderer (and every default-font widget) derives its text size
    // from QApplication::font(), not from Th::c().fonts — without this, the
    // font-size setting would only reach px-token'd chrome labels.
    void applyAppFontScale();

    Th::Theme _theme;
    QString   _themeId;
    QString   _fontSizeId;
    QFont     _baseAppFont; // app font as set by main() — scaling never compounds
};
