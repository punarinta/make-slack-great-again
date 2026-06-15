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
void finalizeNav(Theme &t) {
    t.nav.itemSelectedText = t.nav.primary;                // old dark chats tone → pill ink
    t.nav.primary          = overlayWhite(t.nav.bg, 0.12); // chats surface = rail + 12% white
    t.nav.itemHover        = overlayWhite(t.nav.bg, 0.24); // hover sits lighter than the surface

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
        .arg(qss(th.text.primary))
        .arg(qss(th.text.onDark))
        .arg(th.fonts.caption);
}

} // namespace Th
