// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme.h"
#include "theme_manager.h"

#include <QPoint>
#include <QWidget>

#include <algorithm>

namespace Th {

const Theme &current() {
    return ThemeManager::instance().theme();
}

QString qss(const QColor &c) {
    if (c.alpha() == 255)
        return c.name(); // "#RRGGBB"
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

namespace {

// Scale each RGB channel of `c` by `f`, clamped — a perceptually fine way to
// lighten/darken the dark sidebar tones for the gradient endpoints.
QColor scaleRgb(const QColor &c, double f) {
    return QColor(
        std::clamp(static_cast<int>(c.red() * f), 0, 255),
        std::clamp(static_cast<int>(c.green() * f), 0, 255),
        std::clamp(static_cast<int>(c.blue() * f), 0, 255),
        c.alpha()
    );
}

// Composite opaque white at alpha `a` (0..1) over `base` — Slack's "translucent
// plate over the backdrop" trick, evaluated once at theme-build time.
QColor overlayWhite(const QColor &base, double a) {
    return QColor(
        std::clamp(static_cast<int>(base.red() * (1 - a) + 255 * a), 0, 255),
        std::clamp(static_cast<int>(base.green() * (1 - a) + 255 * a), 0, 255),
        std::clamp(static_cast<int>(base.blue() * (1 - a) + 255 * a), 0, 255),
        base.alpha()
    );
}

// Subtle vertical sidebar gradient, matching the official Slack reference
// (~12% lighter at the top, ~10% darker at the bottom). Derived from the solid
// nav.bg / nav.primary tones so every theme gets a consistent gradient for free.
constexpr double kGradTopFactor    = 1.12;
constexpr double kGradBottomFactor = 0.90;

// Slack lightens the conversation list relative to the workspace rail by laying a
// translucent white plate over the same backdrop. We bake that in: the chats-bar
// surface (nav.primary) and its hover are derived from the rail tone (nav.bg) plus
// a white overlay, so the list reads *lighter* than the rail on every theme. The
// rail-dark tone the theme declared as nav.primary becomes the ink for text/icons
// on the near-white selected pill (nav.itemSelectedText). Then both columns get
// the shared vertical gradient.
//
// Dark themes need a much thinner plate: even 12% white over a near-black rail
// lands around #404346 — *lighter* than the dark content surface, flipping the
// sidebar/content depth. With dark content the whole sidebar must stay darker
// than the content area, so the plate only nudges the list above the rail.
void finalizeNav(Theme &t) {
    const bool   darkContent = t.surface.content.lightnessF() < 0.5;
    const double plate       = darkContent ? 0.03 : 0.12;
    const double hoverPlate  = darkContent ? 0.10 : 0.24;

    t.nav.itemSelectedText = t.nav.primary;                 // old dark chats tone → pill ink
    t.nav.primary          = overlayWhite(t.nav.bg, plate); // chats surface = rail + plate
    t.nav.itemHover = overlayWhite(t.nav.bg, hoverPlate);   // hover sits lighter than the surface

    t.nav.bgGradTop         = scaleRgb(t.nav.bg, kGradTopFactor);
    t.nav.bgGradBottom      = scaleRgb(t.nav.bg, kGradBottomFactor);
    t.nav.primaryGradTop    = scaleRgb(t.nav.primary, kGradTopFactor);
    t.nav.primaryGradBottom = scaleRgb(t.nav.primary, kGradBottomFactor);
}

} // namespace

// ── Default theme: aubergine (Slack's classic purple sidebar) ────────────────

const Theme kAubergineBase = {
    .nav =
        {
            .bg      = QColor("#3F0E40"),
            .primary = QColor("#350D36"), // → itemSelectedText; chats surface derived from bg
            .workspaceBubble  = QColor("#4A154B"),
            .itemSelected     = QColor("#E1DBE1"), // near-white selection pill
            .itemText         = QColor("#FFFFFF"),
            .itemTextDim      = QColor("#CFC3CF"),
            .scrollThumb      = QColor(255, 255, 255, 100),
            .scrollThumbHover = QColor(255, 255, 255, 160),
            .extBadgeBg       = QColor(230, 201, 138, 38),
            .extBadgeText     = QColor("#E6C98A"),
        },
    .surface =
        {
            .content         = QColor("#FFFFFF"),
            .raised          = QColor("#FFFFFF"),
            .sunken          = QColor("#F4F4F4"),
            .overlay         = QColor(0, 0, 0, 70),
            .viewerBackdrop  = QColor(12, 12, 14, 238),
            .viewerBtnHover  = QColor(255, 255, 255, 38),
            .highlight       = QColor("#F0F0F0"),
            .highlightStrong = QColor("#E8E8E8"),
        },
    .text =
        {
            .primary      = QColor("#1D1C1D"),
            .documentBody = QColor("#333333"),
            .secondary    = QColor("#616061"),
            .tertiary     = QColor("#888888"),
            .onDark       = QColor("#FFFFFF"),
            .onDarkDim    = QColor("#CFC3CF"),
            .link         = QColor("#1264A3"),
            .danger       = QColor("#C0392B"),
            .warning      = QColor("#7A5800"),
        },
    .accent =
        {
            .def      = QColor("#4A154B"),
            .hover    = QColor("#611F69"),
            .pressed  = QColor("#350D36"),
            .dark     = QColor("#350D36"),
            .text     = QColor("#FFFFFF"),
            .subtleBg = QColor("#F4E5F5"),
        },
    .badge =
        {
            .unread   = QColor("#E01E5A"),
            .mention  = QColor("#CD2553"),
            .activity = QColor("#1D9BD1"),
        },
    .presence =
        {
            .online  = QColor("#2BAC76"),
            .away    = QColor("#8B8B8B"),
            .phantom = QColor("#E8A33D"),
        },
    .message =
        {
            .hover                  = QColor(0, 0, 0, 10),
            .mentionBg              = QColor("#E5F6FD"),
            .mentionSelfBg          = QColor("#FFF5D1"),
            .mentionText            = QColor("#1264A3"),
            .codeBlockBg            = QColor("#F4F4F4"),
            .codeBlockBorder        = QColor("#CCCCCC"),
            .codeText               = QColor("#555555"),
            .quoteBorder            = QColor("#CCCCCC"),
            .attachmentBg           = QColor("#FAFAFA"),
            .attachmentBorder       = QColor("#DDDDDD"),
            .attachmentDismiss      = QColor("#888888"),
            .pinnedBg               = QColor(0xFF, 0xEB, 0x3B, 60),
            .reminderBg             = QColor(0x1D, 0x9B, 0xD1, 26),
            .reminderText           = QColor("#1264A3"),
            .fileChipBg             = QColor("#FAFAFA"),
            .fileChipBorder         = QColor("#DDDDDD"),
            .fileNameDim            = QColor("#666666"),
            .imagePlaceholderBg     = QColor("#F5F5F5"),
            .imagePlaceholderBorder = QColor("#CCCCCC"),
            .replyBarHover          = QColor("#F8F8F8"),
            .replyBarHoverBorder    = QColor("#D1D5DB"),
            .replyLink              = QColor("#1164A3"),
            .appBadgeBg             = QColor(29, 28, 29, 33),
            .appBadgeText           = QColor("#616061"),
            .extBadgeBg             = QColor(198, 146, 10, 38),
            .extBadgeText           = QColor("#8A6508"),
            .avatarHslSaturation    = 130,
            .avatarHslLightness     = 100,
        },
    .composer =
        {
            .bg                    = QColor("#FFFFFF"),
            .border                = QColor("#DDDDDD"),
            .borderFocus           = QColor("#999999"),
            .toolbarBg             = QColor("#F5F5F5"),
            .toolbarBorder         = QColor("#DDDDDD"),
            .toolbarIcon           = QColor("#888888"),
            .toolbarIconActive     = QColor("#505050"),
            .attachmentChipBg      = QColor("#F8F8F8"),
            .attachmentChipBorder  = QColor("#E0E0E0"),
            .attachmentOverlayBg   = QColor(255, 255, 255, 210),
            .attachmentOverlayText = QColor("#1D1C1D"),
            .dropArrow             = QColor("#CCCCCC"),
            .dropArrowActive       = QColor("#FFFFFF"),
        },
    .editBanner =
        {
            .bg     = QColor("#FFF8EE"),
            .border = QColor("#E8A917"),
            .accent = QColor("#F0DFA0"),
            .text   = QColor("#7A5800"),
        },
    .updateBanner =
        {
            .bg     = QColor("#FFFDE7"),
            .border = QColor("#F9A825"),
            .accent = QColor("#FFF9C4"),
            .text   = QColor("#1D1C1D"),
        },
    .danger =
        {
            .def   = QColor("#E01E5A"),
            .hover = QColor("#C0184F"),
            .icon  = QColor("#C0392B"),
            .text  = QColor("#C0392B"),
        },
    .divider =
        {
            .def    = QColor("#E8E8E8"),
            .strong = QColor("#D1D1D1"),
            .subtle = QColor("#F0F0F0"),
        },
    .icon =
        {
            .def     = QColor("#888888"),
            .strong  = QColor("#454245"),
            .accent  = QColor("#4A154B"),
            .danger  = QColor("#C0392B"),
            .onDark  = QColor("#FFFFFF"),
            .warning = QColor("#7A5800"),
            .starred = QColor("#C6920A"),
            .dim     = QColor("#CCCCCC"),
        },
    .titleBar =
        {
            .bg             = QColor("#3F0E40"),
            .controlDefault = QColor("#CFC3CF"),
            .controlHover   = QColor("#FFFFFF"),
            .controlClose   = QColor("#C0392B"),
        },
    .loader =
        {
            .a = QColor(0xED, 0xAE, 0x2F),
            .b = QColor(0x2F, 0xB2, 0x7C),
            .c = QColor(0x38, 0xBC, 0xED),
            .d = QColor(0xDC, 0x1A, 0x59),
        },
    .contextMenu =
        {
            .bg          = QColor("#FFFFFF"),
            .border      = QColor("#D1D1D1"),
            .itemHover   = QColor("#F0F0F0"),
            .itemText    = QColor("#1D1C1D"),
            .itemTextDim = QColor("#888888"),
            .dangerText  = QColor("#E01E5A"),
        },
    .tooltip =
        {
            .bg = QColor("#1D1C1D"),
        },
    .fonts =
        {
            .xs      = 10,
            .sm      = 11,
            .caption = 12,
            .md      = 13,
            .base    = 14,
            .lg      = 15,
            .xl      = 16,
            .xxl     = 18,
            .xxxl    = 24,
        },
    .fontScales =
        {
            .messageBold  = 1.15,
            .messageSmall = 0.88,
            .timestamp    = 0.85,
            .secondary    = 0.82,
            .badge        = 0.78,
            .micro        = 0.75,
        },
    .spacing =
        {
            .xs  = 2,
            .sm  = 4,
            .md  = 8,
            .lg  = 12,
            .xl  = 16,
            .xxl = 24,
        },
    .workspaceHslSaturation = 65,
    .workspaceHslLightness  = 42,
};

static Theme makeAubergine() {
    Theme t = kAubergineBase;
    finalizeNav(t);
    return t;
}

const Theme kAubergine = makeAubergine();

// ── Charcoal theme: full dark mode ────────────────────────────────────────────
// Unlike blue/green (chrome-only retints), charcoal also retints every
// content-side token: dark surfaces, light text, alpha overlays that *lighten*
// instead of darken. Still copy-and-patch from the aubergine base so any token
// not repeated here inherits a deliberate value (badges, presence dots, loader
// and the type scales are shared by construction).
//
// Palette is pure neutral graphite (R=G=B on every grey, no blue cast),
// modelled on the sBlack dark Slack theme: near-black chrome (#131313),
// #222222 content, a mid-grey #545454 selection pill carrying light ink, and
// bright near-white primary text.

static Theme makeCharcoal() {
    Theme t = kAubergineBase;

    // Chrome — a neutral near-black rail. The chats-list plate lands just above
    // this but still *below* the content surface (see finalizeNav's dark-content
    // path): sidebar darker than content, rail darkest, like Slack's dark mode.
    t.nav.bg              = QColor("#131313");
    t.nav.primary         = QColor("#DEDEDE"); // → itemSelectedText: light ink on the grey pill
    t.nav.workspaceBubble = QColor("#333333");
    t.nav.itemSelected    = QColor("#545454"); // mid-grey selection pill (sBlack-style)
    t.nav.itemTextDim     = QColor("#C9C9C9");

    t.surface.content         = QColor("#222222");
    t.surface.raised          = QColor("#2A2A2A");
    t.surface.sunken          = QColor("#1A1A1A");
    t.surface.highlight       = QColor("#2E2E2E");
    t.surface.highlightStrong = QColor("#383838");

    t.text.primary      = QColor("#E6E6E6");
    t.text.documentBody = QColor("#D6D6D6");
    t.text.secondary    = QColor("#A8A8A8");
    t.text.tertiary     = QColor("#7B7B7B");
    t.text.onDarkDim    = QColor("#C9C9C9");
    t.text.link         = QColor("#53B4E5");
    t.text.danger       = QColor("#E57373");
    t.text.warning      = QColor("#D9A741");

    // Accent stays charcoal-neutral but sits mid-grey so a filled CTA is
    // clearly visible against both the dark content and raised surfaces.
    t.accent.def      = QColor("#5A5A5A");
    t.accent.hover    = QColor("#6A6A6A");
    t.accent.pressed  = QColor("#4A4A4A");
    t.accent.dark     = QColor("#3E3E3E");
    t.accent.subtleBg = QColor("#333333");

    t.message.hover                  = QColor(255, 255, 255, 12); // lighten, don't darken
    t.message.mentionBg              = QColor(29, 155, 209, 46);
    t.message.mentionSelfBg          = QColor(250, 200, 60, 42);
    t.message.mentionText            = QColor("#53B4E5");
    t.message.codeBlockBg            = QColor("#262626");
    t.message.codeBlockBorder        = QColor("#3E3E3E");
    t.message.codeText               = QColor("#BDBDBD");
    t.message.quoteBorder            = QColor("#4D4D4D");
    t.message.attachmentBg           = QColor("#202020");
    t.message.attachmentBorder       = QColor("#3A3A3A");
    t.message.attachmentDismiss      = QColor("#9C9C9C");
    t.message.pinnedBg               = QColor(0xFF, 0xEB, 0x3B, 28);
    t.message.reminderBg             = QColor(0x1D, 0x9B, 0xD1, 24);
    t.message.reminderText           = QColor("#53B4E5");
    t.message.fileChipBg             = QColor("#202020");
    t.message.fileChipBorder         = QColor("#3A3A3A");
    t.message.fileNameDim            = QColor("#A6A6A6");
    t.message.imagePlaceholderBg     = QColor("#262626");
    t.message.imagePlaceholderBorder = QColor("#3E3E3E");
    t.message.replyBarHover          = QColor("#282828");
    t.message.replyBarHoverBorder    = QColor("#3E3E3E");
    t.message.replyLink              = QColor("#53B4E5");
    t.message.appBadgeBg             = QColor(255, 255, 255, 30);
    t.message.appBadgeText           = QColor("#A8A8A8");
    t.message.extBadgeBg             = QColor(230, 201, 138, 30);
    t.message.extBadgeText           = QColor("#D9B45C");

    t.composer.bg                    = QColor("#222222");
    t.composer.border                = QColor("#3A3A3A");
    t.composer.borderFocus           = QColor("#6E6E6E");
    t.composer.toolbarBg             = QColor("#1B1B1B");
    t.composer.toolbarBorder         = QColor("#383838");
    t.composer.toolbarIcon           = QColor("#9C9C9C");
    t.composer.toolbarIconActive     = QColor("#E6E6E6");
    t.composer.attachmentChipBg      = QColor("#282828");
    t.composer.attachmentChipBorder  = QColor("#3E3E3E");
    t.composer.attachmentOverlayBg   = QColor(34, 34, 34, 210);
    t.composer.attachmentOverlayText = QColor("#E6E6E6");
    t.composer.dropArrow             = QColor("#4D4D4D");

    t.editBanner.bg     = QColor("#332B18");
    t.editBanner.border = QColor("#C99A2C");
    t.editBanner.accent = QColor("#4A3E1E");
    t.editBanner.text   = QColor("#E3C36B");

    t.updateBanner.bg     = QColor("#33301C");
    t.updateBanner.accent = QColor("#453E20");
    t.updateBanner.text   = QColor("#E6E6E6");

    t.danger.hover = QColor("#F02E6A"); // lighten on press-hover, not darken
    t.danger.icon  = QColor("#E57373");
    t.danger.text  = QColor("#E57373");

    t.divider.def    = QColor("#333333");
    t.divider.strong = QColor("#4D4D4D");
    t.divider.subtle = QColor("#2A2A2A");

    t.icon.def     = QColor("#9C9C9C");
    t.icon.strong  = QColor("#C9C9C9");
    t.icon.accent  = QColor("#A8A8A8"); // accent tint must stay visible on dark
    t.icon.danger  = QColor("#E57373");
    t.icon.warning = QColor("#D9A741");
    t.icon.starred = QColor("#E0B341");
    t.icon.dim     = QColor("#4D4D4D");

    t.contextMenu.bg          = QColor("#262626");
    t.contextMenu.border      = QColor("#4D4D4D");
    t.contextMenu.itemHover   = QColor("#333333");
    t.contextMenu.itemText    = QColor("#E6E6E6");
    t.contextMenu.itemTextDim = QColor("#9C9C9C");
    t.contextMenu.dangerText  = QColor("#F06A85");

    t.titleBar.bg             = t.nav.bg;
    t.titleBar.controlDefault = QColor("#C9C9C9"); // drop the aubergine tint

    // Tooltips stay a dark chip, but darker than the dark surfaces they float
    // over so they still read as a separate layer.
    t.tooltip.bg = QColor("#0A0A0A");

    finalizeNav(t);
    return t;
}

const Theme kCharcoal = makeCharcoal();

// ── Blue theme: same content surfaces, ocean-blue chrome ─────────────────────
// Copy-and-patch rather than a second 200-line literal: the delta below IS the
// definition of what "blue" changes, and content-side tokens can never drift.

static Theme makeOceanBlue() {
    Theme t = kAubergineBase;

    t.nav.bg              = QColor("#0E2A40");
    t.nav.primary         = QColor("#0B2335"); // → itemSelectedText; chats surface derived from bg
    t.nav.workspaceBubble = QColor("#15405E");
    t.nav.itemSelected    = QColor("#DBE0E5"); // near-white selection pill
    t.nav.itemTextDim     = QColor("#C3CCD4");

    t.accent.def      = QColor("#1264A3");
    t.accent.hover    = QColor("#1B7CC4");
    t.accent.pressed  = QColor("#0B4F82");
    t.accent.dark     = QColor("#0B4F82");
    t.accent.subtleBg = QColor("#E5F0F8");

    t.icon.accent = t.accent.def;
    t.titleBar.bg = t.nav.bg;

    finalizeNav(t);
    return t;
}

const Theme kOceanBlue = makeOceanBlue();

// ── Green theme: same content surfaces, forest-green chrome ───────────────────
// Same copy-and-patch approach as blue; only the chrome tokens differ.

static Theme makeForestGreen() {
    Theme t = kAubergineBase;

    t.nav.bg              = QColor("#0E3D2E");
    t.nav.primary         = QColor("#0A3124"); // → itemSelectedText; chats surface derived from bg
    t.nav.workspaceBubble = QColor("#15543E");
    t.nav.itemSelected    = QColor("#DBE5E0"); // near-white selection pill
    t.nav.itemTextDim     = QColor("#C3D4CC");

    t.accent.def      = QColor("#007A5A"); // Slack brand green
    t.accent.hover    = QColor("#148567");
    t.accent.pressed  = QColor("#055C42");
    t.accent.dark     = QColor("#055C42");
    t.accent.subtleBg = QColor("#E5F4EE");

    t.icon.accent = t.accent.def;
    t.titleBar.bg = t.nav.bg;

    finalizeNav(t);
    return t;
}

const Theme kForestGreen = makeForestGreen();

const std::vector<ThemeInfo> &availableThemes() {
    static const std::vector<ThemeInfo> kThemes = {
        {QStringLiteral("purple"), &kAubergine},
        {QStringLiteral("charcoal"), &kCharcoal},
        {QStringLiteral("blue"), &kOceanBlue},
        {QStringLiteral("green"), &kForestGreen},
    };
    return kThemes;
}

const Theme *themeById(const QString &id) {
    for (const auto &info : availableThemes())
        if (info.id == id)
            return info.theme;
    return nullptr;
}

const Theme &defaultTheme() {
    return kAubergine;
}

QLinearGradient navGradient(const QWidget *widget, const QColor &top, const QColor &bottom) {
    const QWidget  *win  = widget->window();
    const int       yTop = widget->mapTo(win, QPoint(0, 0)).y();
    const int       h    = std::max(1, win->height());
    // Local coordinates: the gradient runs from the window's top edge to its
    // bottom, regardless of where this widget sits — so sibling sidebar widgets
    // line up seamlessly and the fill stays put as list rows scroll.
    QLinearGradient g(0, -yTop, 0, h - yTop);
    g.setColorAt(0.0, top);
    g.setColorAt(1.0, bottom);
    return g;
}

QString globalQss() {
    const auto &th = c();
    return QString(
               "QToolTip {"
               "  background-color: %1;"
               "  color: %2;"
               "  border: none;"
               "  border-radius: 6px;"
               "  padding: 5px 10px;"
               "  font-weight: bold;"
               "  font-size: %3px;"
               "}"
    )
        .arg(qss(th.tooltip.bg))
        .arg(qss(th.text.onDark))
        .arg(th.fonts.caption);
}

QString scrollBarQss(int width, int radius) {
    const auto &th = c();
    return QString(
               "QScrollBar:vertical { background: transparent; width: %1px; margin: 0; }"
               "QScrollBar::handle:vertical { background: %3; border-radius: %2px;"
               " min-height: 28px; }"
               "QScrollBar::handle:vertical:hover { background: %4; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
               "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
               " background: transparent; }"
               "QScrollBar:horizontal { background: transparent; height: %1px; margin: 0; }"
               "QScrollBar::handle:horizontal { background: %3; border-radius: %2px;"
               " min-width: 28px; }"
               "QScrollBar::handle:horizontal:hover { background: %4; }"
               "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
               "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
               " background: transparent; }"
    )
        .arg(width)
        .arg(radius)
        .arg(qss(th.divider.strong), qss(th.text.secondary));
}

static QString fontRule(int fontPx) {
    return fontPx > 0 ? QString("font-size: %1px;").arg(fontPx) : QString();
}

QString radioQss(int fontPx) {
    const auto &th = c();
    // 16px well + 1px border; checked = accent ring + accent dot with a
    // well-colored gap (radial gradient keeps the indicator size constant).
    return QString(
               "QRadioButton { color: %1; %2 background: transparent; }"
               "QRadioButton::indicator { width: 16px; height: 16px; border-radius: 9px;"
               "  border: 1px solid %3; background: %4; }"
               "QRadioButton::indicator:hover { border-color: %5; }"
               "QRadioButton::indicator:checked { border-color: %6;"
               "  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,"
               "  stop:0 %6, stop:0.45 %6, stop:0.55 %4, stop:1 %4); }"
    )
        .arg(qss(th.text.primary), fontRule(fontPx), qss(th.divider.strong))
        .arg(qss(th.surface.content), qss(th.text.tertiary), qss(th.accent.def));
}

QString checkBoxQss(int fontPx) {
    const auto &th = c();
    return QString(
               "QCheckBox { color: %1; %2 background: transparent; }"
               "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px;"
               "  border: 1px solid %3; background: %4; }"
               "QCheckBox::indicator:hover { border-color: %5; }"
               "QCheckBox::indicator:checked { border-color: %6; background: %6;"
               "  image: url(:/ui/check-on-accent.svg); }"
    )
        .arg(qss(th.text.primary), fontRule(fontPx), qss(th.divider.strong))
        .arg(qss(th.surface.content), qss(th.text.tertiary), qss(th.accent.def));
}

QString spinBoxQss(int fontPx) {
    const auto &th = c();
    return QString(
               "QSpinBox { %1 color: %2; background: %3;"
               "  border: 1px solid %4; border-radius: 4px; padding: 3px 6px;"
               "  selection-background-color: %5; selection-color: %6; }"
               "QSpinBox:focus { border-color: %7; }"
               "QSpinBox::up-button, QSpinBox::down-button {"
               "  width: 16px; border: none; background: transparent; }"
               "QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: %8; }"
               "QSpinBox::up-arrow { image: url(:/ui/spin-up.svg); width: 10px; height: 10px; }"
               "QSpinBox::down-arrow { image: url(:/ui/spin-down.svg); width: 10px;"
               "  height: 10px; }"
    )
        .arg(fontRule(fontPx), qss(th.text.primary), qss(th.surface.content))
        .arg(qss(th.divider.strong), qss(th.accent.def), qss(th.accent.text))
        .arg(qss(th.text.link), qss(th.surface.highlight));
}

QString stockDialogQss() {
    const auto &th = c();
    // One flat rule is enough: with a stylesheet background on every widget the
    // style renders the dialog flat anyway, so pinning color/selection to the
    // same theme tokens guarantees contrast on every app-theme × OS-theme
    // combination. Scrollbars get our usual look instead of the stock boxes.
    return QString(
               "QWidget { color: %1; background: %2;"
               "  selection-background-color: %3; selection-color: %4; }"
               "QWidget:disabled { color: %5; }"
           )
               .arg(qss(th.text.primary), qss(th.surface.content), qss(th.accent.def))
               .arg(qss(th.accent.text), qss(th.text.tertiary)) +
           scrollBarQss();
}

} // namespace Th
