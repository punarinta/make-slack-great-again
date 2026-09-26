// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include "ui/file_dialog_utils.h"
#include "ui/fonts.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

// Same store ThemeManager uses (see MSGA_THEME_SETTINGS_FILE in main()).
static QSettings testSettings() {
    return QSettings(qEnvironmentVariable("MSGA_THEME_SETTINGS_FILE"), QSettings::IniFormat);
}

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-theme");
    app.setOrganizationName("msga-test");
    // ThemeManager persists via testSettings(). setPath() cannot
    // redirect that on macOS (CFPreferences) or Windows (registry), so point
    // the manager at a per-process temp INI instead — this also keeps the two
    // ctest entries built from this binary from sharing one store.
    static QTemporaryDir settingsDir;
    qputenv("MSGA_THEME_SETTINGS_FILE", settingsDir.filePath("msga.ini").toUtf8());
    // The ctest entry test_theme_migration runs this binary with a pre-mode
    // config (a single "appearance/theme" key) so the ThemeManager constructor's
    // one-time migration can be exercised; it runs once per process.
    const QByteArray seed = qgetenv("MSGA_TEST_SEED_THEME");
    if (!seed.isEmpty())
        testSettings().setValue("appearance/theme", QString::fromUtf8(seed));
    return msga_test::runCatch(argc, argv);
}

namespace {

// Tokens the chrome step writes; every one must come out a real colour for
// every preset × mode (an unset ChromeSpec field would leave one invalid).
void checkChromeTokensValid(const Th::Theme &t) {
    for (const QColor *c :
         {&t.nav.bg,
          &t.nav.primary,
          &t.nav.workspaceBubble,
          &t.nav.itemSelected,
          &t.nav.itemSelectedText,
          &t.nav.itemHover,
          &t.nav.itemText,
          &t.nav.itemTextDim,
          &t.nav.scrollThumb,
          &t.nav.scrollThumbHover,
          &t.nav.extBadgeBg,
          &t.nav.extBadgeText,
          &t.nav.bgGradTop,
          &t.nav.bgGradBottom,
          &t.nav.primaryGradTop,
          &t.nav.primaryGradBottom,
          &t.accent.def,
          &t.accent.hover,
          &t.accent.pressed,
          &t.accent.dark,
          &t.accent.subtleBg,
          &t.accent.text,
          &t.icon.accent,
          &t.titleBar.bg,
          &t.titleBar.controlDefault,
          &t.titleBar.controlHover,
          &t.presence.online,
          &t.badge.mention})
        CHECK(c->isValid());
}

} // namespace

TEST_CASE("theme registry", "[theme]") {
    const auto &themes = Th::availableThemes();
    REQUIRE(themes.size() >= 2);
    CHECK(themes[0].id == "purple");   // default first
    CHECK(themes[1].id == "charcoal"); // second slot in the picker

    // Every preset renders over both content modes.
    for (const auto &info : themes) {
        INFO("theme: " << info.id.toStdString());
        REQUIRE(info.light != nullptr);
        REQUIRE(info.dark != nullptr);
        CHECK(info.variant(false) == info.light);
        CHECK(info.variant(true) == info.dark);
        CHECK(Th::themeById(info.id, false) == info.light);
        CHECK(Th::themeById(info.id, true) == info.dark);
        CHECK_FALSE(Th::isDarkTheme(*info.light));
        CHECK(Th::isDarkTheme(*info.dark));
        checkChromeTokensValid(*info.light);
        checkChromeTokensValid(*info.dark);
    }

    CHECK(Th::themeById("purple", false) == &Th::defaultTheme());
    CHECK(Th::themeById("charcoal", true) == &Th::defaultDarkTheme());
    CHECK(Th::themeById("does-not-exist", false) == nullptr);
    CHECK(Th::themeById("does-not-exist", true) == nullptr);
}

TEST_CASE("presets retint chrome only; content surfaces are shared per mode", "[theme]") {
    for (const bool dark : {false, true}) {
        const auto &purple = *Th::themeById("purple", dark);
        for (const auto &info : Th::availableThemes()) {
            INFO("theme: " << info.id.toStdString() << (dark ? " dark" : " light"));
            const auto &t = *info.variant(dark);

            // Chrome is coherent…
            CHECK(t.titleBar.bg == t.nav.bg);
            CHECK(t.titleBar.controlDefault == t.nav.itemTextDim);
            CHECK(t.titleBar.controlHover == t.nav.itemText);
            if (!dark)
                CHECK(t.icon.accent == t.accent.def);
            if (info.id != QLatin1String("purple")) {
                CHECK(t.nav.bg != purple.nav.bg);
                CHECK(t.accent.def != purple.accent.def);
            }

            // …content-side tokens are identical across presets (copy-and-patch).
            CHECK(t.text.primary == purple.text.primary);
            CHECK(t.surface.content == purple.surface.content);
            CHECK(t.message.codeBlockBg == purple.message.codeBlockBg);
            CHECK(t.composer.bg == purple.composer.bg);
            CHECK(t.badge.mention == purple.badge.mention);
            CHECK(t.fonts.base == purple.fonts.base);
        }
    }
}

TEST_CASE("a preset's chrome is the same over both content modes", "[theme]") {
    for (const auto &info : Th::availableThemes()) {
        INFO("theme: " << info.id.toStdString());
        const auto &l = *info.light;
        const auto &d = *info.dark;
        CHECK(l.nav.bg == d.nav.bg);
        CHECK(l.nav.itemSelected == d.nav.itemSelected);
        CHECK(l.nav.itemSelectedText == d.nav.itemSelectedText);
        CHECK(l.nav.workspaceBubble == d.nav.workspaceBubble);
        CHECK(l.nav.itemText == d.nav.itemText);
        CHECK(l.nav.itemTextDim == d.nav.itemTextDim);
        CHECK(l.titleBar.bg == d.titleBar.bg);
        // The list plate is thinner over dark content, but always lightens the rail.
        CHECK(l.nav.primary.lightnessF() > l.nav.bg.lightnessF());
        CHECK(d.nav.primary.lightnessF() > d.nav.bg.lightnessF());
        CHECK(d.nav.primary.lightnessF() < l.nav.primary.lightnessF());
    }
}

TEST_CASE("every dark variant is a coherent dark theme", "[theme]") {
    for (const auto &info : Th::availableThemes()) {
        INFO("theme: " << info.id.toStdString());
        const auto &t = *info.dark;

        // Dark surfaces, light text — and enough spread between them to read.
        CHECK(t.surface.content.lightnessF() < 0.2);
        CHECK(t.surface.raised.lightnessF() < 0.25);
        CHECK(t.surface.sunken.lightnessF() < t.surface.content.lightnessF());
        CHECK(t.text.primary.lightnessF() > 0.75);
        CHECK(t.text.secondary.lightnessF() > 0.5);
        CHECK(t.text.primary.lightnessF() - t.surface.content.lightnessF() > 0.5);

        // Alpha overlays must LIGHTEN on dark surfaces, not darken.
        CHECK(t.message.hover.lightnessF() > 0.9);
        CHECK(t.surface.highlight.lightnessF() > t.surface.content.lightnessF());

        // Filled controls stay visible: accent face vs the surfaces it sits on,
        // and its label vs the face. Brand accents tuned for white content are
        // lifted for dark content.
        CHECK(t.accent.def.lightnessF() - t.surface.raised.lightnessF() > 0.1);
        CHECK(t.accent.hover.lightnessF() > t.accent.def.lightnessF());
        CHECK(t.accent.pressed.lightnessF() < t.accent.def.lightnessF());
        CHECK(t.accent.text.lightnessF() - t.accent.def.lightnessF() > 0.4);
        CHECK(t.icon.accent.lightnessF() > 0.5);

        // Content chrome that borders text follows the dark surfaces.
        CHECK(t.composer.bg.lightnessF() < 0.25);
        CHECK(t.message.codeBlockBg.lightnessF() < 0.25);
        CHECK(t.contextMenu.bg.lightnessF() < 0.25);
        CHECK(t.divider.def.lightnessF() < 0.4);

        // Structure/type scales stay shared with the base theme.
        const auto &purple = *Th::themeById("purple", false);
        CHECK(t.fonts.base == purple.fonts.base);
        CHECK(t.spacing.md == purple.spacing.md);
        CHECK(t.badge.mention == purple.badge.mention);
    }

    // Graphite over dark content is the full dark mode: chats list darker than
    // the content area, rail darkest — the light themes' white-plate derivation
    // must not flip this on dark.
    const auto &charcoal = *Th::themeById("charcoal", true);
    CHECK(charcoal.nav.primary.lightnessF() < charcoal.surface.content.lightnessF());
    CHECK(charcoal.nav.bg.lightnessF() < charcoal.nav.primary.lightnessF());
}

TEST_CASE("light charcoal is light content under graphite chrome", "[theme]") {
    const auto &t      = *Th::themeById("charcoal", false);
    const auto &purple = *Th::themeById("purple", false);
    CHECK_FALSE(Th::isDarkTheme(t));
    CHECK(t.surface.content == purple.surface.content);
    CHECK(t.text.primary == purple.text.primary);
    CHECK(t.nav.bg.lightnessF() < 0.1);
    // A filled grey control reads on white and carries white text.
    CHECK(t.accent.def.lightnessF() < 0.5);
    CHECK(t.accent.text.lightnessF() - t.accent.def.lightnessF() > 0.4);
    // The subtle accent plate is light, not the dark-mode grey.
    CHECK(t.accent.subtleBg.lightnessF() > 0.8);
}

TEST_CASE("buildTheme: pins are honoured, a light rail flips the ink", "[theme]") {
    Th::ChromeSpec spec;
    spec.rail            = QColor("#F5F0EB"); // Hoth-like light rail
    spec.pill            = QColor("#3F0E40");
    spec.pillInk         = QColor("#FFFFFF");
    spec.workspaceBubble = QColor("#DDD6CF");
    spec.accent          = {
        QColor("#4A154B"),
        QColor("#611F69"),
        QColor("#350D36"),
        QColor("#350D36"),
        QColor("#F4E5F5")
    };

    const Th::Theme light = Th::buildTheme(spec, false);
    // Derived ink on a light rail is dark.
    CHECK(light.nav.itemText.lightnessF() < 0.2);
    CHECK(light.nav.itemTextDim.lightnessF() < 0.5);
    CHECK(light.nav.scrollThumb.red() == 0);
    CHECK(light.nav.extBadgeText.lightnessF() < 0.5);
    CHECK(light.titleBar.controlHover == light.nav.itemText);
    // …and the content side is untouched.
    CHECK(light.surface.content == Th::defaultTheme().surface.content);
    // Dark content under the same light chrome keeps the chrome identical.
    const Th::Theme dark = Th::buildTheme(spec, true);
    CHECK(Th::isDarkTheme(dark));
    CHECK(dark.nav.itemText == light.nav.itemText);
    CHECK(dark.nav.bg == light.nav.bg);
    // The dark-content accent was lifted from the (dark) brand accent.
    CHECK(dark.accent.def.lightnessF() > light.accent.def.lightnessF());
    CHECK(qAbs(dark.accent.def.hslHueF() - light.accent.def.hslHueF()) < 0.02);

    // Pinned values survive derivation exactly.
    spec.itemHover       = QColor("#112233");
    spec.itemText        = QColor("#445566");
    spec.itemTextDim     = QColor("#778899");
    spec.presenceOnline  = QColor("#00FF00");
    spec.badgeMention    = QColor("#FF0000");
    spec.titleBarControl = QColor("#ABCDEF");
    spec.accentDark      = {
        QColor("#101010"),
        QColor("#202020"),
        QColor("#303030"),
        QColor("#404040"),
        QColor("#505050")
    };
    spec.iconAccentDark    = QColor("#606060");
    const Th::Theme pinned = Th::buildTheme(spec, true);
    CHECK(pinned.nav.itemHover == QColor("#112233"));
    CHECK(pinned.nav.itemText == QColor("#445566"));
    CHECK(pinned.nav.itemTextDim == QColor("#778899"));
    CHECK(pinned.presence.online == QColor("#00FF00"));
    CHECK(pinned.badge.mention == QColor("#FF0000"));
    CHECK(pinned.titleBar.controlDefault == QColor("#ABCDEF"));
    CHECK(pinned.accent.def == QColor("#101010"));
    CHECK(pinned.accent.subtleBg == QColor("#505050"));
    CHECK(pinned.icon.accent == QColor("#606060"));
    // Pins that only concern dark content don't leak into the light variant.
    const Th::Theme pinnedLight = Th::buildTheme(spec, false);
    CHECK(pinnedLight.accent.def == QColor("#4A154B"));
    CHECK(pinnedLight.icon.accent == QColor("#4A154B"));
}

TEST_CASE("ThemeManager switches, persists and ignores unknown ids", "[theme]") {
    auto &mgr = ThemeManager::instance();
    CHECK(mgr.themeId() == "purple"); // fresh settings → default

    QSignalSpy spy(&mgr, &ThemeManager::themeChanged);

    mgr.setThemeById("blue");
    CHECK(mgr.themeId() == "blue");
    CHECK(mgr.theme().nav.bg == Th::themeById("blue", false)->nav.bg);
    CHECK(spy.count() == 1);
    CHECK(testSettings().value("appearance/theme").toString() == QStringLiteral("blue"));

    mgr.setThemeById("blue"); // no-op: already active
    CHECK(spy.count() == 1);

    mgr.setThemeById("does-not-exist"); // ignored
    CHECK(mgr.themeId() == "blue");
    CHECK(spy.count() == 1);

    mgr.setThemeById("purple");
    CHECK(mgr.themeId() == "purple");
    CHECK(spy.count() == 2);
    CHECK(testSettings().value("appearance/theme").toString() == QStringLiteral("purple"));
}

TEST_CASE("registry classifies content darkness", "[theme]") {
    CHECK_FALSE(Th::isDarkTheme(*Th::themeById("purple", false)));
    CHECK(Th::isDarkTheme(*Th::themeById("purple", true)));
    CHECK_FALSE(Th::isDarkTheme(*Th::themeById("charcoal", false)));
    CHECK(Th::isDarkTheme(*Th::themeById("charcoal", true)));
    CHECK(Th::isDarkTheme(Th::defaultDarkTheme()));
    CHECK_FALSE(Th::isDarkTheme(Th::defaultTheme()));
}

TEST_CASE("colour mode: default is System, slots are per mode, persisted", "[theme][mode]") {
    auto &mgr = ThemeManager::instance();
    // Reset to a known state (earlier cases may have moved the light slot).
    mgr.setMode(ThemeManager::ColorMode::System);
    mgr.setThemeIdFor(false, "purple");
    mgr.setThemeIdFor(true, "charcoal");

    // Default mode is System — the same as the official Slack desktop app.
    CHECK(ThemeManager::modeFromId("") == ThemeManager::ColorMode::System);
    CHECK(ThemeManager::modeFromId("bogus") == ThemeManager::ColorMode::System);
    CHECK(ThemeManager::modeId(ThemeManager::ColorMode::System) == "system");
    CHECK(ThemeManager::modeFromId("light") == ThemeManager::ColorMode::Light);
    CHECK(ThemeManager::modeFromId("dark") == ThemeManager::ColorMode::Dark);

    // The offscreen platform reports no scheme → System resolves to light.
    CHECK(mgr.mode() == ThemeManager::ColorMode::System);
    CHECK_FALSE(mgr.effectiveDark());
    CHECK(mgr.themeId() == "purple");

    QSignalSpy themeSpy(&mgr, &ThemeManager::themeChanged);
    QSignalSpy modeSpy(&mgr, &ThemeManager::modeChanged);

    // Fixed dark: renders the dark slot.
    mgr.setMode(ThemeManager::ColorMode::Dark);
    CHECK(mgr.effectiveDark());
    CHECK(mgr.themeId() == "charcoal");
    CHECK(Th::isDarkTheme(mgr.theme()));
    CHECK(themeSpy.count() == 1);
    CHECK(modeSpy.count() == 1);
    CHECK(testSettings().value("appearance/mode").toString() == "dark");

    // Editing the light slot while dark is shown changes nothing on screen…
    mgr.setThemeIdFor(false, "blue");
    CHECK(mgr.themeId() == "charcoal");
    CHECK(themeSpy.count() == 1);
    CHECK(mgr.themeIdFor(false) == "blue");
    CHECK(testSettings().value("appearance/theme").toString() == "blue");
    CHECK(mgr.themeIdFor(true) == "charcoal"); // untouched default: not written until changed

    // …until the mode flips to it.
    mgr.setMode(ThemeManager::ColorMode::Light);
    CHECK(mgr.themeId() == "blue");
    CHECK(themeSpy.count() == 2);

    // Any preset fits either slot: the slot picks the chrome, the mode the
    // content. Editing the off-screen dark slot changes nothing on screen.
    mgr.setThemeIdFor(true, "blue");
    CHECK(mgr.themeIdFor(true) == "blue");
    CHECK(mgr.themeId() == "blue"); // the light slot, still on screen
    CHECK_FALSE(Th::isDarkTheme(mgr.theme()));
    CHECK(themeSpy.count() == 2);
    CHECK(testSettings().value("appearance/themeDark").toString() == "blue");
    mgr.setThemeIdFor(true, "nope"); // unknown → ignored
    CHECK(mgr.themeIdFor(true) == "blue");
    mgr.setThemeIdFor(false, "charcoal"); // graphite chrome over light content
    CHECK(mgr.themeId() == "charcoal");
    CHECK_FALSE(Th::isDarkTheme(mgr.theme()));
    CHECK(mgr.theme().nav.bg == Th::themeById("charcoal", false)->nav.bg);
    CHECK(themeSpy.count() == 3);

    // Dark mode with the blue slot renders blue chrome over dark content.
    mgr.setMode(ThemeManager::ColorMode::Dark);
    CHECK(mgr.themeId() == "blue");
    CHECK(Th::isDarkTheme(mgr.theme()));
    CHECK(mgr.theme().nav.bg == Th::themeById("blue", true)->nav.bg);
    CHECK(themeSpy.count() == 4);
    mgr.setThemeIdFor(true, "charcoal");
    CHECK(themeSpy.count() == 5);
    mgr.setMode(ThemeManager::ColorMode::Light);
    CHECK(themeSpy.count() == 6);

    // setThemeById edits the slot on screen — the old single-slot API keeps
    // working for callers that don't know about modes.
    mgr.setThemeById("charcoal"); // light slot already charcoal → no-op
    CHECK(themeSpy.count() == 6);
    mgr.setThemeById("green");
    CHECK(mgr.themeId() == "green");
    CHECK(mgr.themeIdFor(false) == "green");
    CHECK(mgr.themeIdFor(true) == "charcoal");
    CHECK(themeSpy.count() == 7);

    // Same mode again is a no-op.
    mgr.setMode(ThemeManager::ColorMode::Light);
    CHECK(themeSpy.count() == 7);

    // Back to System (→ light here): nothing new on screen, but the mode
    // observers still hear about it.
    const int modeBefore = modeSpy.count();
    mgr.setMode(ThemeManager::ColorMode::System);
    CHECK(mgr.themeId() == "green");
    CHECK(themeSpy.count() == 7);
    CHECK(modeSpy.count() == modeBefore + 1);

    mgr.setThemeIdFor(false, "purple");
}

TEST_CASE("colour mode: System follows the OS scheme live", "[theme][mode]") {
    auto &mgr = ThemeManager::instance();
    mgr.setMode(ThemeManager::ColorMode::System);
    mgr.setThemeIdFor(false, "purple");
    mgr.setThemeIdFor(true, "charcoal");
    REQUIRE(mgr.themeId() == "purple");

    QSignalSpy themeSpy(&mgr, &ThemeManager::themeChanged);

    // The offscreen platform theme reports no scheme (and ignores
    // QStyleHints::setColorScheme), so drive the resolver through the override
    // the way a portal-less desktop would; refreshSystemScheme() is what the
    // colorSchemeChanged signal is wired to.
    qputenv("MSGA_SYSTEM_COLOR_SCHEME", "dark");
    mgr.refreshSystemScheme();
    CHECK(mgr.effectiveDark());
    CHECK(mgr.themeId() == "charcoal");
    CHECK(themeSpy.count() == 1);

    // A fixed mode ignores the OS…
    mgr.setMode(ThemeManager::ColorMode::Light);
    CHECK(mgr.themeId() == "purple");
    qputenv("MSGA_SYSTEM_COLOR_SCHEME", "light");
    mgr.refreshSystemScheme();
    qputenv("MSGA_SYSTEM_COLOR_SCHEME", "dark");
    mgr.refreshSystemScheme();
    CHECK(mgr.themeId() == "purple");

    // …and System picks it back up on re-entry.
    mgr.setMode(ThemeManager::ColorMode::System);
    CHECK(mgr.themeId() == "charcoal");

    qunsetenv("MSGA_SYSTEM_COLOR_SCHEME");
    mgr.refreshSystemScheme();
    CHECK(mgr.themeId() == "purple");
}

TEST_CASE("legacy charcoal pick migrates to a fixed dark mode", "[theme][migration]") {
    // Only meaningful when main() seeded the pre-mode config (see there).
    if (qgetenv("MSGA_TEST_SEED_THEME") != "charcoal")
        SKIP("run via test_theme_migration");
    auto &mgr = ThemeManager::instance();
    // Charcoal was the only dark-content theme, so the pick meant "dark mode":
    // keep it dark rather than flipping the user to System on upgrade.
    CHECK(mgr.mode() == ThemeManager::ColorMode::Dark);
    CHECK(mgr.themeId() == "charcoal");
    CHECK(mgr.themeIdFor(true) == "charcoal");
    CHECK(mgr.themeIdFor(false) == "purple");
    QSettings s = testSettings();
    CHECK(s.value("appearance/mode").toString() == "dark");
    CHECK(s.value("appearance/theme").toString() == "purple");
    CHECK(s.value("appearance/themeDark").toString() == "charcoal");
}

TEST_CASE("stock file dialog readable whatever the OS palette", "[theme]") {
    // Issue #18: with no native dialog helper Qt shows its widget-based
    // QFileDialog, which inherits an ancestor `QWidget { background: … }`
    // stylesheet (theme surface) while QStyleSheetStyle rebuilds each child's
    // text colors from the OS palette — white-on-white when the OS theme is
    // dark and the app theme is light (and inverted for charcoal). The dialog
    // must pin its text to the same theme its backgrounds come from.
    const QPalette appPalette = qApp->palette();
    QPalette       osDark     = appPalette;
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        osDark.setColor(role, QColor("#FCFCFC"));
    qApp->setPalette(osDark);

    for (const auto &info : Th::availableThemes())
        for (const auto *variant : {info.light, info.dark}) {
            INFO(
                "theme: " << info.id.toStdString()
                          << (Th::isDarkTheme(*variant) ? " dark" : " light")
            );
            ThemeManager::instance().setTheme(*variant);

            // Stand-in for MainWindow's right panel, whose stylesheet cascades
            // into every parented dialog.
            QWidget panel;
            panel.setStyleSheet(
                QString("QWidget { background: %1; }").arg(Th::qss(Th::c().surface.content))
            );

            QFileDialog dlg(&panel);
            dlg.setOption(QFileDialog::DontUseNativeDialog);
            Ui::applyFileDialogTheme(&dlg);
            dlg.show();

            auto *label = dlg.findChild<QLabel *>("fileNameLabel");
            REQUIRE(label != nullptr);
            CHECK(label->palette().color(QPalette::WindowText) == Th::c().text.primary);
        }

    qApp->setPalette(appPalette);
    ThemeManager::instance().setTheme(Th::defaultTheme());
}

// ── Custom themes (phase 3) ──────────────────────────────────────────────────

TEST_CASE("custom theme: legacy share strings parse in every spelling", "[theme][custom]") {
    // Slack's own aubergine example, 8 values with '#'.
    const auto eight =
        Th::parseCustomTheme("#4D394B,#3E313C,#4C9689,#FFFFFF,#3E313C,#FFFFFF,#38978D,#EB4D5C");
    REQUIRE(eight);
    CHECK(eight->primary.color == QColor("#4D394B"));
    CHECK(eight->highlight1.color == QColor("#4C9689"));
    CHECK(eight->pins.itemSelText == QColor("#FFFFFF"));
    CHECK(eight->pins.itemHover == QColor("#3E313C"));
    CHECK(eight->pins.itemText == QColor("#FFFFFF"));
    CHECK(eight->highlight2.color == QColor("#38978D"));
    CHECK(eight->important.color == QColor("#EB4D5C"));
    CHECK_FALSE(eight->pins.titleBarBg.isValid());
    CHECK(eight->brightness == Th::CustomTheme::kBrightnessNeutral);
    CHECK(eight->sidebarInverted);
    CHECK(eight->gradient);

    // 10 values, no '#', lower case, spaces after the commas, 3-digit shorthand.
    const auto ten = Th::parseCustomTheme(
        "1a1d21, 222529, 1164a3, fff, 350d36, d1d2d3, 2bac76, cd2553, 121016, fff"
    );
    REQUIRE(ten);
    CHECK(ten->primary.color == QColor("#1A1D21"));
    CHECK(ten->pins.itemSelText == QColor("#FFFFFF"));
    CHECK(ten->pins.titleBarBg == QColor("#121016"));
    CHECK(ten->pins.titleBarText == QColor("#FFFFFF"));
    // A value equal to a swatch remembers the swatch's name.
    CHECK(ten->highlight2.palette == "jade");
    CHECK(ten->important.palette == "cherry");
    CHECK(ten->primary.palette == "nocturne");

    // Whitespace-separated is accepted too (what a copy from a rendered page gives).
    CHECK(Th::parseCustomTheme("#4D394B #3E313C #4C9689 #FFFFFF #3E313C #FFFFFF #38978D #EB4D5C"));
    // Surrounding whitespace / newline.
    CHECK(
        Th::parseCustomTheme("  #4D394B,#3E313C,#4C9689,#FFFFFF,#3E313C,#FFFFFF,#38978D,#EB4D5C\n")
    );

    // Garbage: wrong count, a non-hex token, empty, prose, unrelated JSON.
    CHECK_FALSE(Th::parseCustomTheme("#4D394B,#3E313C,#4C9689,#FFFFFF,#3E313C,#FFFFFF,#38978D"));
    CHECK_FALSE(
        Th::parseCustomTheme("#4D394B,#3E313C,#4C9689,#FFFFFF,#3E313C,#FFFFFF,#38978D,#GGGGGG")
    );
    CHECK_FALSE(
        Th::parseCustomTheme("#4D394B,#3E313C,#4C9689,#FFFF,#3E313C,#FFFFFF,#38978D,#EB4D5C")
    );
    CHECK_FALSE(Th::parseCustomTheme(""));
    CHECK_FALSE(Th::parseCustomTheme("hello world"));
    CHECK_FALSE(Th::parseCustomTheme(R"({"foo":1})"));
    CHECK_FALSE(Th::parseCustomTheme("{not json"));
}

TEST_CASE("custom theme: ia_theme JSON with palette names and hex", "[theme][custom]") {
    // Verbatim shape of what users.prefs.get returns.
    const auto slack = Th::parseCustomTheme(
        R"({"primary":{"palette":"aubergine"},"highlight1":{"palette":"aubergine"},)"
        R"("highlight2":{"palette":"jade"},"important":{"palette":"aubergine"},)"
        R"("brightness":6,"sidebarInverted":true,"useCustomHex":false})"
    );
    REQUIRE(slack);
    CHECK(slack->primary.color == Th::swatchByName("aubergine")->color);
    CHECK(slack->primary.palette == "aubergine");
    CHECK(slack->highlight2.color == Th::swatchByName("jade")->color);
    CHECK(slack->sidebarInverted);
    CHECK(slack->gradient); // not a Slack key → our default

    // A hex wins over the name; an unknown name uses the hex, or falls back.
    const auto mixed = Th::parseCustomTheme(
        R"({"primary":{"hex":"#112233","palette":"aubergine"},)"
        R"("highlight1":{"palette":"work hard","hex":"#445566"},)"
        R"("highlight2":{"palette":"no-such-swatch"},)"
        R"("important":"#778899","brightness":42,"sidebarInverted":false})"
    );
    REQUIRE(mixed);
    CHECK(mixed->primary.color == QColor("#112233"));
    CHECK(mixed->primary.palette.isEmpty()); // not a swatch value any more
    CHECK(mixed->highlight1.color == QColor("#445566"));
    CHECK(mixed->highlight2 == Th::defaultCustomTheme().highlight2);
    CHECK(mixed->important.color == QColor("#778899"));
    CHECK(mixed->brightness == Th::CustomTheme::kBrightnessMax); // clamped
    CHECK_FALSE(mixed->sidebarInverted);

    // Missing slots take the defaults, but at least one slot must be present.
    const auto partial = Th::parseCustomTheme(R"({"primary":{"hex":"#000000"}})");
    REQUIRE(partial);
    CHECK(partial->primary.color == QColor("#000000"));
    CHECK(partial->highlight1 == Th::defaultCustomTheme().highlight1);
}

TEST_CASE("custom theme: serialise → parse is the identity", "[theme][custom]") {
    Th::CustomTheme t   = Th::defaultCustomTheme();
    const auto      def = Th::parseCustomTheme(Th::serializeCustomTheme(t));
    REQUIRE(def);
    CHECK(*def == t);

    t.primary            = {QColor("#123456"), {}};
    t.highlight1.color   = QColor("#FEDCBA");
    t.highlight1.palette = {};
    t.brightness         = 2;
    t.sidebarInverted    = false;
    t.gradient           = false;
    t.pins.itemHover     = QColor("#0000FF");
    t.pins.itemSelText   = QColor("#00FF00");
    t.pins.itemText      = QColor("#FF0000");
    t.pins.titleBarBg    = QColor("#101010");
    t.pins.titleBarText  = QColor("#EEEEEE");
    const QString json   = Th::serializeCustomTheme(t);
    const auto    back   = Th::parseCustomTheme(json);
    REQUIRE(back);
    CHECK(*back == t);
    // The Slack-shaped keys are there for a paste into Slack.
    CHECK(json.contains(R"("primary":{"hex":"#123456"})"));
    CHECK(json.contains(R"("useCustomHex":true)"));
    CHECK(json.contains(R"("brightness":2)"));
    CHECK(json.contains(R"("sidebarInverted":false)"));
    // A swatch pick carries its name.
    CHECK(Th::serializeCustomTheme(Th::defaultCustomTheme()).contains(R"("palette":"jade")"));
}

TEST_CASE("custom theme: legacy export → import round-trips", "[theme][custom]") {
    // menu_bg and hover_item are one token on export (both are the hover), so
    // the sample uses the same value for both, as Slack's own themes do.
    const QString in =
        "#3F0E40,#5A2A5B,#1264A3,#FFFFFF,#5A2A5B,#E1D6E1,#2BAC76,#CD2553,#2B0A2C,#F0E4F0";
    const auto t = Th::parseCustomTheme(in);
    REQUIRE(t);
    const Th::Theme built = Th::buildTheme(Th::chromeFromCustom(*t, false), false);
    const QString   out   = Th::legacyShareString(*t, built);
    CHECK(out == in);
    // …and over dark content the chrome (what the string describes) is the same.
    const Th::Theme dark = Th::buildTheme(Th::chromeFromCustom(*t, true), true);
    CHECK(Th::legacyShareString(*t, dark) == in);
    CHECK(Th::isDarkTheme(dark));
}

TEST_CASE("custom theme: slots drive the chrome tokens", "[theme][custom]") {
    Th::CustomTheme t;
    t.primary    = {QColor("#3F0E40"), {}};
    t.highlight1 = {QColor("#1264A3"), {}};
    t.highlight2 = {QColor("#00FF00"), {}};
    t.important  = {QColor("#FF0000"), {}};

    const Th::Theme light = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(light.nav.bg == QColor("#3F0E40")); // neutral brightness: exact
    CHECK(light.titleBar.bg == light.nav.bg);
    CHECK(light.nav.itemSelected == QColor("#1264A3"));
    CHECK(light.nav.itemSelectedText == QColor("#FFFFFF")); // dark pill → white ink
    CHECK(light.accent.def == QColor("#1264A3"));
    CHECK(light.accent.hover.lightnessF() > light.accent.def.lightnessF());
    CHECK(light.accent.pressed.lightnessF() < light.accent.def.lightnessF());
    CHECK(light.icon.accent == QColor("#1264A3"));
    CHECK(light.presence.online == QColor("#00FF00"));
    CHECK(light.badge.mention == QColor("#FF0000"));
    CHECK(light.nav.itemText == QColor("#FFFFFF")); // derived: dark rail → white
    CHECK(light.nav.bgGradTop != light.nav.bgGradBottom);
    CHECK(light.surface.content == Th::defaultTheme().surface.content);
    checkChromeTokensValid(light);

    // Brightness shifts the rail's lightness both ways; hue stays.
    t.brightness           = Th::CustomTheme::kBrightnessMax;
    const Th::Theme bright = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(bright.nav.bg.lightnessF() > light.nav.bg.lightnessF());
    CHECK(qAbs(bright.nav.bg.hslHueF() - light.nav.bg.hslHueF()) < 0.02);
    t.brightness        = Th::CustomTheme::kBrightnessMin;
    const Th::Theme dim = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(dim.nav.bg.lightnessF() < light.nav.bg.lightnessF());
    t.brightness = Th::CustomTheme::kBrightnessNeutral;

    // Gradient off: flat endpoints.
    t.gradient           = false;
    const Th::Theme flat = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(flat.nav.bgGradTop == flat.nav.bg);
    CHECK(flat.nav.bgGradBottom == flat.nav.bg);
    CHECK(flat.nav.primaryGradTop == flat.nav.primary);
    t.gradient = true;

    // Darker sidebar off: over light content the rail becomes a pale tint of
    // the primary (a light rail, so the ink flips); over dark content the rail
    // stays the primary — a pale rail would sit above the content.
    t.sidebarInverted    = false;
    const Th::Theme pale = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(pale.nav.bg.lightnessF() > 0.85);
    CHECK(qAbs(pale.nav.bg.hslHueF() - light.nav.bg.hslHueF()) < 0.02);
    CHECK(pale.nav.itemText.lightnessF() < 0.2);
    CHECK(pale.nav.scrollThumb.red() == 0);
    const Th::Theme paleDark = Th::buildTheme(Th::chromeFromCustom(t, true), true);
    CHECK(paleDark.nav.bg == QColor("#3F0E40"));
    CHECK(paleDark.nav.itemText == QColor("#FFFFFF"));
    t.sidebarInverted = true;

    // A light primary (Slack's Hoth) is a light rail outright; a light pill
    // carries dark ink; dark content lifts nothing it can't read.
    t.primary            = {QColor("#F5F0EB"), {}};
    t.highlight1         = {QColor("#E8E0D8"), {}};
    const Th::Theme hoth = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(hoth.nav.itemText.lightnessF() < 0.2);
    CHECK(hoth.nav.itemSelectedText.lightnessF() < 0.2);
    CHECK(hoth.nav.extBadgeText.lightnessF() < 0.5);
    const Th::Theme hothDark = Th::buildTheme(Th::chromeFromCustom(t, true), true);
    CHECK(Th::isDarkTheme(hothDark));
    CHECK(hothDark.accent.def.lightnessF() < 0.6); // pale highlight dropped to a readable fill
    checkChromeTokensValid(hoth);
    checkChromeTokensValid(hothDark);

    // Pins from a legacy string survive derivation.
    t.pins.itemHover       = QColor("#112233");
    t.pins.itemSelText     = QColor("#445566");
    t.pins.itemText        = QColor("#778899");
    t.pins.titleBarBg      = QColor("#AABBCC");
    t.pins.titleBarText    = QColor("#DDEEFF");
    const Th::Theme pinned = Th::buildTheme(Th::chromeFromCustom(t, false), false);
    CHECK(pinned.nav.itemHover == QColor("#112233"));
    CHECK(pinned.nav.itemSelectedText == QColor("#445566"));
    CHECK(pinned.nav.itemText == QColor("#778899"));
    CHECK(pinned.titleBar.bg == QColor("#AABBCC"));
    CHECK(pinned.titleBar.controlDefault == QColor("#DDEEFF"));
    CHECK(pinned.nav.itemTextDim != QColor("#778899")); // derived from the pinned ink
    CHECK(pinned.nav.itemTextDim.isValid());
}

TEST_CASE("custom theme: contrast ratio and swatch lookups", "[theme][custom]") {
    CHECK(Th::contrastRatio(QColor("#000000"), QColor("#FFFFFF")) == Catch::Approx(21.0));
    CHECK(Th::contrastRatio(QColor("#FFFFFF"), QColor("#FFFFFF")) == Catch::Approx(1.0));
    CHECK(Th::contrastRatio(QColor("#FFFFFF"), QColor("#3F0E40")) > 10.0);
    CHECK(Th::contrastRatio(QColor("#3F0E40"), QColor("#FFFFFF")) > 10.0); // symmetric
    CHECK(Th::contrastRatio(QColor("#777777"), QColor("#888888")) < 1.5);

    REQUIRE_FALSE(Th::swatches().empty());
    for (const auto &s : Th::swatches()) {
        CHECK(s.color.isValid());
        CHECK(Th::swatchByName(s.name) == &s);
        CHECK(Th::swatchNameFor(s.color) == s.name);
    }
    CHECK(Th::swatchByName(" Aubergine ") != nullptr); // case/space tolerant
    CHECK(Th::swatchByName("nope") == nullptr);
    CHECK(Th::swatchNameFor(QColor("#010203")).isEmpty());
}

TEST_CASE("ThemeManager: the custom slot renders the user's theme live", "[theme][custom]") {
    auto &mgr = ThemeManager::instance();
    mgr.setMode(ThemeManager::ColorMode::Light);
    mgr.setThemeIdFor(false, "purple");
    REQUIRE(mgr.themeId() == "purple");
    // Fresh settings: the editor's default.
    CHECK(mgr.customTheme() == Th::defaultCustomTheme());

    QSignalSpy themeSpy(&mgr, &ThemeManager::themeChanged);
    QSignalSpy customSpy(&mgr, &ThemeManager::customThemeChanged);

    // Editing the definition while a preset is on screen persists but doesn't
    // re-render.
    Th::CustomTheme t = mgr.customTheme();
    t.primary         = {QColor("#102030"), {}};
    mgr.setCustomTheme(t);
    CHECK(customSpy.count() == 1);
    CHECK(themeSpy.count() == 0);
    CHECK(mgr.customVariant(false).nav.bg == QColor("#102030"));
    CHECK(mgr.customVariant(true).nav.bg == QColor("#102030"));
    CHECK(Th::isDarkTheme(mgr.customVariant(true)));
    const auto stored =
        Th::parseCustomTheme(testSettings().value("appearance/customTheme").toString());
    REQUIRE(stored);
    CHECK(*stored == t);
    mgr.setCustomTheme(t); // unchanged → nothing
    CHECK(customSpy.count() == 1);

    // Selecting the custom card for the mode on screen renders it…
    mgr.setThemeIdFor(false, ThemeManager::kCustomId);
    CHECK(mgr.themeId() == ThemeManager::kCustomId);
    CHECK(themeSpy.count() == 1);
    CHECK(mgr.theme().nav.bg == QColor("#102030"));
    CHECK(
        testSettings().value("appearance/theme").toString() ==
        QLatin1String(ThemeManager::kCustomId)
    );
    // …and now an edit re-renders at once.
    t.primary = {QColor("#405060"), {}};
    mgr.setCustomTheme(t);
    CHECK(themeSpy.count() == 2);
    CHECK(mgr.theme().nav.bg == QColor("#405060"));
    CHECK(mgr.themeFor(ThemeManager::kCustomId, false) == &mgr.customVariant(false));
    CHECK(mgr.themeFor("blue", true) == Th::themeById("blue", true));
    CHECK(mgr.themeFor("nope", true) == nullptr);

    // The dark slot can hold it too, and a mode flip lands on the dark variant.
    mgr.setThemeIdFor(true, ThemeManager::kCustomId);
    mgr.setMode(ThemeManager::ColorMode::Dark);
    CHECK(mgr.themeId() == ThemeManager::kCustomId);
    CHECK(Th::isDarkTheme(mgr.theme()));
    CHECK(mgr.theme().nav.bg == QColor("#405060"));

    // Back to a clean state for whatever runs next.
    mgr.setMode(ThemeManager::ColorMode::Light);
    mgr.setThemeIdFor(false, "purple");
    mgr.setThemeIdFor(true, "charcoal");
    mgr.setCustomTheme(Th::defaultCustomTheme());
}

// Regression: the message header fonts (sender name, timestamp, tag pill,
// reaction count) used to be function-local statics built from the app font on
// first paint, so a runtime font-size switch left them at the old size until a
// restart while the body text followed.
TEST_CASE("Ui::Fonts follow a runtime font-size change", "[theme][fonts]") {
    auto &mgr = ThemeManager::instance();
    mgr.setFontSizeId("medium");

    const Ui::Fonts &fonts   = Ui::Fonts::get();
    const qreal      basePt  = QApplication::font().pointSizeF();
    const quint32    baseGen = fonts.generation;
    const int        baseH   = fonts.msgNameFm.height();
    REQUIRE(basePt > 0);
    CHECK(fonts.msgName.pointSizeF() == Catch::Approx(basePt));
    CHECK(fonts.msgName.bold());
    CHECK(fonts.msgTs.pointSizeF() == Catch::Approx(basePt * 0.85));
    CHECK(fonts.tagBadge.pointSizeF() == Catch::Approx(basePt * 0.62));
    CHECK(fonts.demiBold.weight() == QFont::DemiBold);
    // Cached, not rebuilt, while nothing changes.
    CHECK(Ui::Fonts::get().generation == baseGen);
    CHECK(&Ui::Fonts::get() == &fonts);

    // The Appearance → Font size setting: every derived font follows at once.
    mgr.setFontSizeId("large");
    const qreal largePt = QApplication::font().pointSizeF();
    REQUIRE(largePt > basePt);
    // (The set rebuilds in place on the next get(); paint paths always call it.)
    const Ui::Fonts &large = Ui::Fonts::get();
    CHECK(&large == &fonts);
    CHECK(large.generation != baseGen);
    CHECK(large.msgName.pointSizeF() == Catch::Approx(largePt));
    CHECK(large.msgTs.pointSizeF() == Catch::Approx(largePt * 0.85));
    CHECK(large.tagBadge.pointSizeF() == Catch::Approx(largePt * 0.62));
    CHECK(large.reactionCount.pointSizeF() == Catch::Approx(largePt * 0.82));
    CHECK(large.countBadge.pointSizeF() == Catch::Approx(largePt * 0.78));
    CHECK(large.msgNameFm.height() > baseH);

    // A plain app-font change (system font, no theme re-apply) invalidates too.
    const quint32 largeGen = large.generation;
    QFont         f        = QApplication::font();
    f.setPointSizeF(largePt * 2);
    QApplication::setFont(f);
    CHECK(Ui::Fonts::get().generation != largeGen);
    CHECK(Ui::Fonts::get().msgName.pointSizeF() == Catch::Approx(largePt * 2));

    // Back to the default size: back to the original fonts.
    mgr.setFontSizeId("medium");
    CHECK(Ui::Fonts::get().msgName.pointSizeF() == Catch::Approx(basePt));
    CHECK(Ui::Fonts::get().msgNameFm.height() == baseH);
}
