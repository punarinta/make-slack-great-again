// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Centralized semantic theming system.
// All colors, font sizes and spacing are accessed via Th::c() — the active theme.
// Use Th::qss(color) to embed a QColor inside a Qt stylesheet string.
#pragma once

#include "theme_custom.h"

#include <QColor>
#include <QLinearGradient>
#include <QString>

#include <vector>

class QWidget;

namespace Th {

// ── Sub-structs ───────────────────────────────────────────────────────────────

struct NavColors {
    QColor bg;               // workspace sidebar column (solid mid-point; borders/dots/seams)
    QColor primary;          // conversation list panel (solid mid-point; row base, borders)
    QColor workspaceBubble;  // workspace icon chip
    QColor itemHover;        // hovered conversation row
    QColor itemSelected;     // active/selected conversation row (near-white pill)
    QColor itemSelectedText; // dark ink (text/icons/away-ring) on the selected pill
    QColor itemText;         // primary text on nav panel
    QColor itemTextDim;      // subdued text (channel names, inactive)
    QColor scrollThumb;      // scrollbar thumb on dark panel
    QColor scrollThumbHover;
    QColor extBadgeBg;   // "EXT" tag background on the dark sidebar
    QColor extBadgeText; // "EXT" tag text on the dark sidebar
    // Slack-style sidebar gradient endpoints (vertical, lighter at top). The
    // workspace rail and the conversation list share one continuous gradient
    // anchored to the window; see Th::navGradient(). Derived from bg/primary.
    QColor bgGradTop;
    QColor bgGradBottom;
    QColor primaryGradTop;
    QColor primaryGradBottom;
};

struct SurfaceColors {
    QColor content;         // main message list background
    QColor raised;          // popups, tooltips, dropdowns
    QColor sunken;          // code blocks, inset areas
    QColor overlay;         // semi-transparent modal backdrop
    QColor viewerBackdrop;  // near-opaque backdrop of the full-window image viewer
    QColor viewerBtnHover;  // hovered action button on the viewer backdrop
    QColor highlight;       // hover on light background
    QColor highlightStrong; // pressed / stronger highlight
};

struct TextColors {
    QColor primary;      // main body copy
    QColor documentBody; // long-form document copy (canvas) — softer than primary
    QColor secondary;    // subdued (timestamps, captions)
    QColor tertiary;     // placeholder, hints, very dimmed
    QColor onDark;       // text on dark backgrounds
    QColor onDarkDim;    // subdued text on dark backgrounds
    QColor link;         // hyperlinks
    QColor danger;       // error / destructive text
    QColor warning;      // warning-context text
};

struct AccentColors {
    QColor def;      // primary button / brand accent
    QColor hover;    // hovered primary button
    QColor pressed;  // pressed primary button
    QColor dark;     // darker accent (secondary use)
    QColor text;     // text on accent-coloured surface
    QColor subtleBg; // very light accent-tinted background
};

struct BadgeColors {
    QColor unread;   // unread message count badge
    QColor mention;  // @mention badge (important: DMs + mentions — red)
    QColor activity; // non-important unread activity dot (blue)
};

struct PresenceColors {
    QColor online;
    QColor away;
    QColor phantom; // self-only: would be active, but no official client is connected
};

struct MessageColors {
    QColor hover;              // message row hover
    QColor mentionBg;          // @mention chip background — someone else
    QColor mentionSelfBg;      // @mention chip background — the authed user (yellow)
    QColor mentionText;        // @mention chip text (both variants)
    QColor codeBlockBg;        // inline/block code background
    QColor codeBlockBorder;    // code block border / blockquote bar
    QColor codeText;           // code font colour
    QColor quoteBorder;        // blockquote left bar
    QColor attachmentBg;       // file/link preview card background
    QColor attachmentBorder;   // attachment card border
    QColor attachmentDismiss;  // dismiss "×" button color
    QColor attachmentBar;      // attachment left bar when the payload sets no color
    QColor namedBarGood;       // attachment bar for the legacy named color "good"
    QColor namedBarWarning;    // … "warning"
    QColor namedBarDanger;     // … "danger"
    QColor botButtonBg;        // Block Kit / attachment button (outlined default style)
    QColor botButtonHoverBg;   // hovered default button
    QColor botButtonBorder;    // default button outline
    QColor botButtonFill;      // "primary" button fill ("danger" uses danger.def/hover)
    QColor botButtonFillHover; // hovered "primary" button
    QColor pinnedBg;           // pinned message row tint
    QColor reminderBg;         // "reminder set" message row tint (light blue)
    QColor reminderText;       // reminder banner text/icon ("Due …")
    QColor fileChipBg;         // non-image file chip background
    QColor fileChipBorder;     // file chip border
    QColor fileNameDim;        // filename label in image section
    QColor imagePlaceholderBg; // loading-image placeholder fill
    QColor imagePlaceholderBorder;
    QColor replyBarHover; // reply bar hover background
    QColor replyBarHoverBorder;
    QColor replyLink;     // "N replies" link color
    QColor appBadgeBg;    // "APP" tag background next to bot names
    QColor appBadgeText;  // "APP" tag text
    QColor extBadgeBg;    // "EXT" tag background next to external (Slack Connect) users
    QColor extBadgeText;  // "EXT" tag text
    QColor canvasTile;    // canvas preview card's icon tile — Slack's canvas blue, every theme
    QColor tableBorder;   // data table rounded frame + the rule under its header row
    QColor tableHeaderBg; // data table header row tint
    QColor tableRowRule;  // hairline between data table body rows
    int    avatarHslSaturation; // generated avatar HSL saturation
    int    avatarHslLightness;
};

struct ComposerColors {
    QColor bg;
    QColor border;
    QColor borderFocus;
    QColor toolbarBg;
    QColor toolbarBorder;
    QColor toolbarIcon;
    QColor toolbarIconActive;
    QColor attachmentChipBg;
    QColor attachmentChipBorder;
    QColor attachmentOverlayBg;   // semitransparent plate behind name/size on image chips
    QColor attachmentOverlayText; // text on attachmentOverlayBg
    QColor dropArrow;             // drop-menu chevron (empty composer)
    QColor dropArrowActive;
};

struct EditBannerColors {
    QColor bg;
    QColor border;
    QColor accent;
    QColor text;
};

struct DangerColors {
    QColor def;
    QColor hover;
    QColor icon;
    QColor text;
};

struct DividerColors {
    QColor def;    // standard section divider
    QColor strong; // popup/card outer border
    QColor subtle; // very subtle separation
};

struct IconColors {
    QColor def;     // standard icon
    QColor strong;  // dark icon (context menu items)
    QColor accent;  // accent-coloured icon
    QColor danger;  // destructive icon
    QColor onDark;  // icon on dark background
    QColor warning; // icon in warning context
    QColor starred; // starred-state icon (golden amber)
    QColor dim;     // very subdued icon
};

struct TitleBarColors {
    QColor bg;
    QColor controlDefault;
    QColor controlHover;
    QColor controlClose;
};

struct LoaderColors {
    QColor a, b, c, d;
};

struct ContextMenuColors {
    QColor bg;
    QColor border;
    QColor itemHover;
    QColor itemText;
    QColor itemTextDim;
    QColor dangerText;
};

struct TooltipColors {
    // Bubble fill (text on it is text.onDark). Deliberately near-black in EVERY
    // theme, including dark ones — a tooltip is a floating dark chip, not a
    // content surface, so it must not follow surface.* when the content darkens.
    QColor bg;
};

struct FontSizes {
    int xs;      // 10 — attachment labels
    int sm;      // 11 — small labels
    int caption; // 12 — captions, groupbox titles, banner labels
    int md;      // 13 — composer, mention popup
    int base;    // 14 — search, conv list
    int lg;      // 15 — thread header, welcome
    int xl;      // 16 — section headers
    int xxl;     // 18 — workspace icon label
    int xxxl;    // 24 — dialog titles
};

struct FontScales {
    double messageBold;  // 1.15 — bold spans
    double messageSmall; // 0.88 — blockquote body
    double timestamp;    // 0.85 — message timestamps
    double secondary;    // 0.82 — secondary text
    double badge;        // 0.78 — unread badge numerals
    double micro;        // 0.75 — smallest painted labels
};

struct Spacing {
    int xs;  // 2
    int sm;  // 4
    int md;  // 8
    int lg;  // 12
    int xl;  // 16
    int xxl; // 24
};

// ── Main theme struct ─────────────────────────────────────────────────────────

struct Theme {
    NavColors         nav;
    SurfaceColors     surface;
    TextColors        text;
    AccentColors      accent;
    BadgeColors       badge;
    PresenceColors    presence;
    MessageColors     message;
    ComposerColors    composer;
    EditBannerColors  editBanner;
    EditBannerColors  updateBanner;
    DangerColors      danger;
    DividerColors     divider;
    IconColors        icon;
    TitleBarColors    titleBar;
    LoaderColors      loader;
    ContextMenuColors contextMenu;
    TooltipColors     tooltip;
    FontSizes         fonts;
    FontScales        fontScales;
    Spacing           spacing;

    // Workspace-switcher derived colors (computed per workspace from teamId hash)
    int workspaceHslSaturation; // 65
    int workspaceHslLightness;  // 42
};

// ── Construction: content mode × chrome ─────────────────────────────────────
// Every theme is `light base → (dark content) → chrome`. Slack colours only
// the chrome; whether the content area is light or dark is a separate mode.

// The five accent tokens as one unit.
struct AccentSet {
    QColor def, hover, pressed, dark, subtleBg;
};

// A sidebar palette. The required fields are what the built-in presets set;
// every optional (default-constructed, invalid) colour is derived from the
// rail and content mode by applyChrome — an imported Slack theme pins the ones
// it names, and derivation never overwrites a pinned value.
struct ChromeSpec {
    QColor    rail;            // nav.bg, titleBar.bg; list surface/hover/gradients derive from it
    QColor    pill;            // nav.itemSelected
    QColor    pillInk;         // nav.itemSelectedText
    QColor    workspaceBubble; // nav.workspaceBubble
    QColor    itemTextDim;     // nav.itemTextDim (optional: derived from the rail)
    AccentSet accent;          // over light content
    AccentSet accentDark; // over dark content (optional: `accent` lifted to a readable lightness)
    QColor    iconAccentDark;  // icon.accent over dark content (optional: lifted from accent.def)
    bool      gradient = true; // false: flat sidebar (gradient endpoints = the solid tones)
    // Pins for imported themes (optional).
    QColor    itemHover;       // nav.itemHover
    QColor    itemText;        // nav.itemText
    QColor    presenceOnline;  // presence.online
    QColor    badgeMention;    // badge.mention
    QColor    titleBarBg;      // titleBar.bg (default: the rail)
    QColor    titleBarControl; // titleBar.controlDefault
};

// Build a complete theme: the light base, the dark content set when
// `darkContent`, then `chrome` laid over it (with a light rail flipping every
// ink drawn on the chrome to dark).
Theme buildTheme(const ChromeSpec &chrome, bool darkContent);

// ── Custom themes ────────────────────────────────────────────────────────────

// A named colour the custom-theme editor offers per slot, and what a `palette`
// name in an imported Slack theme resolves against. Our table, Slack-shaped:
// the names Slack uses that we can identify (aubergine, jade, …) carry our
// approximation of Slack's swatch until its table is captured (see
// docs/theming-plan.md, phase 4).
struct Swatch {
    QString name; // lower-case id, as stored in JSON
    QColor  color;
};
const std::vector<Swatch> &swatches();
const Swatch              *swatchByName(const QString &name); // nullptr when unknown
// Name of the swatch equal to `c`, or empty.
QString                    swatchNameFor(const QColor &c);

// The chrome a custom theme describes over one content mode: the rail from
// `primary` (shifted by brightness; a light tint of it when the sidebar is not
// inverted over light content), pill + accent set from `highlight1`, presence
// from `highlight2`, mention badge from `important`, pins as pinned.
ChromeSpec chromeFromCustom(const CustomTheme &t, bool darkContent);

// What the editor starts from (Slack's classic aubergine look).
CustomTheme defaultCustomTheme();

// ── Theme registry ────────────────────────────────────────────────────────────

// A selectable chrome preset, rendered over both content modes. `id` is the
// persisted QSettings value ("appearance/theme" / "appearance/themeDark"); the
// display name is translated at the UI site.
struct ThemeInfo {
    QString      id;
    const Theme *light;
    const Theme *dark;

    const Theme *variant(bool darkContent) const { return darkContent ? dark : light; }
};

// All built-in presets, in display order. First entry is the default (purple).
const std::vector<ThemeInfo> &availableThemes();

// The preset's variant for one content mode; nullptr for unknown ids (callers
// fall back to defaultTheme() / defaultDarkTheme()).
const Theme *themeById(const QString &id, bool dark);

// Whether the theme darkens the CONTENT area (dark mode), as opposed to only
// tinting the chrome.
bool isDarkTheme(const Theme &t);

// ── Access ────────────────────────────────────────────────────────────────────

// Returns the currently active theme. Call via the Th::c() shorthand below.
const Theme &current();

// Shorthand: Th::c().text.primary
inline const Theme &c() {
    return current();
}

// A vertical gradient spanning the full height of the sidebar column, mapped
// into `widget`'s local coordinates. Because it's anchored to the top-level
// window, all sidebar widgets (workspace rail, conversation list, footer) share
// one continuous gradient — and it stays fixed while list rows scroll.
QLinearGradient navGradient(const QWidget *widget, const QColor &top, const QColor &bottom);

// Formats a QColor for embedding in a Qt stylesheet string.
// Opaque → "#RRGGBB"; with alpha → "rgba(r,g,b,A)" (A is 0–255, not 0.0–1.0).
QString qss(const QColor &color);

// Global application stylesheet built from the active theme.
// Apply via qApp->setStyleSheet(Th::globalQss()) and re-apply on themeChanged.
// Covers only things that can't be done per-widget (QToolTip, etc.).
QString globalQss();

// Our scrollbar look: thin rounded handle (`divider.strong`, hover
// `text.secondary`), transparent track, no arrows — vertical + horizontal.
// Shared by every content scroll area (canvas, settings, search). Re-emit on
// themeChanged where it's applied. `width` is the bar thickness, `radius` the
// handle corner radius.
QString scrollBarQss(int width = 8, int radius = 4);

// The knobs behind scrollBarQss(). `margin` insets the handle inside the bar
// (a 2px margin on an 8px bar leaves a 4px handle), `minHandle` is the handle's
// minimum length and `hoverTint` adds the `text.secondary` hover colour.
struct ScrollBarStyle {
    int  width     = 8;
    int  radius    = 4;
    int  margin    = 0;
    int  minHandle = 28;
    bool hoverTint = true;
};
QString scrollBarQss(const ScrollBarStyle &style);

// The slimmer bar of the floating pick lists (mention popup, composer
// completer, conversation selector): 4px handle inset in an 8px track, radius
// 3, no hover tint.
QString popupScrollBarQss();

// Themed stock form controls. Native QRadioButton/QCheckBox/QSpinBox draw their
// indicator/field from the OS palette — light-mode white regardless of the
// theme — so any dialog using them must apply these instead of hand-rolling a
// color/font-only stylesheet. `fontPx` <= 0 keeps the widget's inherited font
// size; an invalid `textColor` means text.primary. Re-apply on themeChanged.
QString radioQss(int fontPx = 0);
QString checkBoxQss(int fontPx = 0, const QColor &textColor = {});
QString spinBoxQss(int fontPx = 0);

// Stylesheet for stock Qt dialogs we don't custom-paint (the widget-based
// QFileDialog fallback). Their text/selection colors come from the OS palette,
// which is unreadable whenever the OS theme's lightness differs from the app
// theme's. Must be a stylesheet, not a QPalette: any ancestor `QWidget {
// background: … }` rule makes QStyleSheetStyle::polish assign each child its
// own palette rebuilt from the app palette, silently discarding one set with
// setPalette().
QString stockDialogQss();

} // namespace Th
