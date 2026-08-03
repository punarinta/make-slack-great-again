// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme_manager.h"

#include <QApplication>
#include <QCursor>
#include <QFontInfo>
#include <QGuiApplication>
#include <QSettings>

#include <algorithm>

namespace {

double fontFactorFor(const QString &id) {
    if (id == QLatin1String("small"))
        return 0.9;
    if (id == QLatin1String("large"))
        return 1.15;
    return 1.0; // "medium" (and any unknown/stale id)
}

// Scale every px size of the theme's font scale. Only ever applied to a fresh
// copy of a pristine registry theme, so factors never compound.
void applyFontScale(Th::Theme &t, double k) {
    if (k == 1.0)
        return;
    const auto s = [k](int px) { return std::max(1, qRound(px * k)); };
    auto      &f = t.fonts;
    f.xs         = s(f.xs);
    f.sm         = s(f.sm);
    f.caption    = s(f.caption);
    f.md         = s(f.md);
    f.base       = s(f.base);
    f.lg         = s(f.lg);
    f.xl         = s(f.xl);
    f.xxl        = s(f.xxl);
    f.xxxl       = s(f.xxxl);
}

} // namespace

ThemeManager &ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent) {
    // The singleton is first touched (via Th::c()) before any widget paints,
    // so the persisted theme is active from the very first frame.
    const QSettings settings("msga", "msga");
    _themeId    = settings.value("appearance/theme", QStringLiteral("purple")).toString();
    _fontSizeId = settings.value("appearance/fontSize", QStringLiteral("medium")).toString();
    const Th::Theme *theme = Th::themeById(_themeId);
    if (!theme) { // unknown/stale id from a future or older version
        _themeId = QStringLiteral("purple");
        theme    = &Th::defaultTheme();
    }
    _theme = *theme;
    applyFontScale(_theme, fontFactorFor(_fontSizeId));
    // The singleton is created from widget code, so main() has already set the
    // app font (detectSystemFont) — capture it as the scaling base. Guarded:
    // theme-token-only users (unit tests) may have no QApplication.
    if (qApp) {
        _baseAppFont = QApplication::font();
        applyAppFontScale();
    }
}

void ThemeManager::applyAppFontScale() {
    if (!qApp)
        return;
    const double k = fontFactorFor(_fontSizeId);
    QFont        f = _baseAppFont;
    if (k != 1.0) {
        if (f.pointSizeF() > 0)
            f.setPointSizeF(f.pointSizeF() * k);
        else if (f.pixelSize() > 0)
            f.setPixelSize(std::max(1, qRound(f.pixelSize() * k)));
        else
            f.setPointSizeF(QFontInfo(f).pointSizeF() * k);
    }
    // A general setFont() clears the per-class font hash the platform theme
    // installs at startup (QMenu, QSmallFont/QMiniFont on mac, …). On Linux
    // main() already sets an app font so that ship has sailed, but on Windows
    // and macOS it hasn't — so don't call it just to re-assign the font we are
    // already using. Reverting to "medium" still goes through: there the active
    // font is the scaled one, not _baseAppFont.
    if (k == 1.0 && QApplication::font() == f)
        return;
    QApplication::setFont(f);
}

void ThemeManager::setTheme(const Th::Theme &theme) {
    // Applying a theme re-polishes the whole widget tree synchronously (rebuilt
    // stylesheets, re-baked pixmaps, reset message docs) and blocks the main
    // thread for a noticeable beat. Show the wait cursor for the duration so the
    // switch reads as "working" rather than frozen.
    QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    _theme = theme;
    applyFontScale(_theme, fontFactorFor(_fontSizeId));
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

double ThemeManager::fontFactor() const {
    return fontFactorFor(_fontSizeId);
}

void ThemeManager::setFontSizeId(const QString &id) {
    if (id == _fontSizeId)
        return;
    _fontSizeId = id;
    QSettings("msga", "msga").setValue("appearance/fontSize", id);
    // App font FIRST: the themeChanged handlers below rebuild message docs,
    // which size their text from QApplication::font().
    applyAppFontScale();
    // Re-derive from the pristine registry theme so the new factor applies to
    // the base sizes, not to the previously scaled ones.
    const Th::Theme *theme = Th::themeById(_themeId);
    setTheme(theme ? *theme : Th::defaultTheme());
}
