// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme_manager.h"
#include "fonts.h"

#include <QApplication>
#include <QCursor>
#include <QFontInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QStyleHints>

#include <algorithm>

namespace {

constexpr auto kModeKey      = "appearance/mode";
constexpr auto kLightKey     = "appearance/theme"; // pre-mode key: old installs keep their pick
constexpr auto kDarkKey      = "appearance/themeDark";
constexpr auto kDefaultLight = "purple";
constexpr auto kDefaultDark  = "charcoal";
constexpr auto kCustomKey    = "appearance/customTheme";

// The app's settings store. Tests point MSGA_THEME_SETTINGS_FILE at a
// temp INI file: QSettings::setPath() cannot redirect the two-argument
// constructor on macOS/Windows (CFPreferences / registry), so without this
// hook a test run would read and write the developer's real preferences.
QSettings openSettings() {
    const QString file = qEnvironmentVariable("MSGA_THEME_SETTINGS_FILE");
    if (!file.isEmpty())
        return QSettings(file, QSettings::IniFormat);
    return QSettings(QStringLiteral("msga"), QStringLiteral("msga"));
}

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

// A slot holds a registry preset id or "custom"; anything else (stale id from
// another version, hand-edited config) falls back to the slot's default.
QString validSlotId(const QString &id, bool dark) {
    if (id == QLatin1String(ThemeManager::kCustomId) || Th::themeById(id, dark))
        return id;
    return QLatin1String(dark ? kDefaultDark : kDefaultLight);
}

bool systemPrefersDark() {
    // Dev/test override, and the escape hatch for desktops where Qt can't read
    // the scheme (no xdg-desktop-portal): force what "System" resolves to.
    const QByteArray forced = qgetenv("MSGA_SYSTEM_COLOR_SCHEME").toLower();
    if (forced == "dark")
        return true;
    if (forced == "light")
        return false;
    if (!qApp)
        return false;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

} // namespace

ThemeManager &ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

QString ThemeManager::modeId(ColorMode mode) {
    switch (mode) {
    case ColorMode::Light:
        return QStringLiteral("light");
    case ColorMode::Dark:
        return QStringLiteral("dark");
    case ColorMode::System:
        break;
    }
    return QStringLiteral("system");
}

ThemeManager::ColorMode ThemeManager::modeFromId(const QString &id) {
    if (id == QLatin1String("light"))
        return ColorMode::Light;
    if (id == QLatin1String("dark"))
        return ColorMode::Dark;
    return ColorMode::System;
}

ThemeManager::ThemeManager(QObject *parent) : QObject(parent) {
    // The singleton is first touched (via Th::c()) before any widget paints,
    // so the persisted theme is active from the very first frame.
    QSettings settings = openSettings();
    _fontSizeId        = settings.value("appearance/fontSize", QStringLiteral("medium")).toString();
    _custom            = Th::parseCustomTheme(settings.value(QLatin1String(kCustomKey)).toString())
                             .value_or(Th::defaultCustomTheme());
    rebuildCustom();

    if (settings.contains(QLatin1String(kModeKey))) {
        _mode    = modeFromId(settings.value(QLatin1String(kModeKey)).toString());
        _lightId = validSlotId(settings.value(QLatin1String(kLightKey)).toString(), false);
        _darkId  = validSlotId(settings.value(QLatin1String(kDarkKey)).toString(), true);
    } else {
        // First launch with colour modes. Before them there was one theme key
        // and charcoal was the only theme with dark content, so a charcoal pick
        // was a dark-mode choice — keep it dark instead of flipping that user
        // to the System default (light content on a light desktop). Everyone
        // else keeps their light pick and gets the System default.
        const QString legacy = settings.value(QLatin1String(kLightKey)).toString();
        if (legacy == QLatin1String(kDefaultDark)) {
            _mode    = ColorMode::Dark;
            _lightId = QLatin1String(kDefaultLight);
            _darkId  = QLatin1String(kDefaultDark);
            settings.setValue(QLatin1String(kModeKey), modeId(_mode));
            settings.setValue(QLatin1String(kLightKey), _lightId);
            settings.setValue(QLatin1String(kDarkKey), _darkId);
        } else {
            _mode    = ColorMode::System;
            _lightId = validSlotId(legacy, false);
            _darkId  = QLatin1String(kDefaultDark);
        }
    }

    _themeDark = effectiveDark();
    _themeId   = _themeDark ? _darkId : _lightId;
    _theme     = resolvedTheme();
    applyFontScale(_theme, fontFactorFor(_fontSizeId));
    // The singleton is created from widget code, so main() has already set the
    // app font (detectSystemFont) — capture it as the scaling base. Guarded:
    // theme-token-only users (unit tests) may have no QApplication.
    if (qApp) {
        _baseAppFont = QApplication::font();
        applyAppFontScale();
        connect(
            QGuiApplication::styleHints(),
            &QStyleHints::colorSchemeChanged,
            this,
            &ThemeManager::refreshSystemScheme
        );
    }
}

void ThemeManager::refreshSystemScheme() {
    if (_mode == ColorMode::System)
        reapply();
}

bool ThemeManager::effectiveDark() const {
    switch (_mode) {
    case ColorMode::Light:
        return false;
    case ColorMode::Dark:
        return true;
    case ColorMode::System:
        break;
    }
    return systemPrefersDark();
}

const Th::Theme *ThemeManager::themeFor(const QString &id, bool dark) const {
    if (id == QLatin1String(kCustomId))
        return &customVariant(dark);
    return Th::themeById(id, dark);
}

const Th::Theme &ThemeManager::resolvedTheme() const {
    const bool       dark = effectiveDark();
    const Th::Theme *t    = themeFor(dark ? _darkId : _lightId, dark);
    if (t)
        return *t;
    return dark ? Th::defaultDarkTheme() : Th::defaultTheme();
}

void ThemeManager::rebuildCustom() {
    _customLight = Th::buildTheme(Th::chromeFromCustom(_custom, false), false);
    _customDark  = Th::buildTheme(Th::chromeFromCustom(_custom, true), true);
}

void ThemeManager::setCustomTheme(const Th::CustomTheme &t) {
    if (t == _custom)
        return;
    _custom = t;
    openSettings().setValue(QLatin1String(kCustomKey), Th::serializeCustomTheme(t));
    rebuildCustom();
    emit customThemeChanged();
    // reapply() would see the same (slot, mode) and skip; the definition behind
    // the slot changed, so re-render outright when it is what's on screen.
    if (_themeId == QLatin1String(kCustomId))
        setTheme(resolvedTheme());
}

void ThemeManager::reapply() {
    const bool     dark = effectiveDark();
    const QString &id   = dark ? _darkId : _lightId;
    // Same preset AND same content mode: nothing new to render (both slots may
    // hold the same preset, in which case a mode flip still changes the content).
    if (id == _themeId && dark == _themeDark) {
        emit modeChanged(); // same theme on screen, but the mode/OS state moved
        return;
    }
    _themeId   = id;
    _themeDark = dark;
    setTheme(resolvedTheme());
    emit modeChanged();
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
    // Before the signal, so its handlers (layout rebuilds) see the new fonts.
    Ui::Fonts::invalidate();
    emit themeChanged();
    QGuiApplication::restoreOverrideCursor();
}

void ThemeManager::setThemeById(const QString &id) {
    setThemeIdFor(effectiveDark(), id);
}

void ThemeManager::setThemeIdFor(bool dark, const QString &id) {
    if (validSlotId(id, dark) != id)
        return; // unknown preset
    QString &slot = dark ? _darkId : _lightId;
    if (slot == id)
        return;
    slot = id;
    openSettings().setValue(QLatin1String(dark ? kDarkKey : kLightKey), id);
    if (dark == effectiveDark())
        reapply();
}

void ThemeManager::setMode(ColorMode mode) {
    if (mode == _mode)
        return;
    _mode = mode;
    openSettings().setValue(QLatin1String(kModeKey), modeId(mode));
    reapply();
}

double ThemeManager::fontFactor() const {
    return fontFactorFor(_fontSizeId);
}

void ThemeManager::setFontSizeId(const QString &id) {
    if (id == _fontSizeId)
        return;
    _fontSizeId = id;
    openSettings().setValue("appearance/fontSize", id);
    // App font FIRST: the themeChanged handlers below rebuild message docs,
    // which size their text from QApplication::font().
    applyAppFontScale();
    // Re-derive from the pristine registry theme so the new factor applies to
    // the base sizes, not to the previously scaled ones.
    setTheme(resolvedTheme());
}
