// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Centralized semantic theming system.
// All colors, font sizes and spacing are accessed via Th::c() — the active theme.
// Use Th::qss(color) to embed a QColor inside a Qt stylesheet string.
#pragma once

#include <QColor>
#include <QString>

namespace Th {

// ── Sub-structs ───────────────────────────────────────────────────────────────

struct NavColors {
    QColor bg;              // workspace sidebar column
    QColor primary;         // conversation list panel
    QColor workspaceBubble; // workspace icon chip
    QColor itemHover;       // hovered conversation row
    QColor itemSelected;    // active/selected conversation row
    QColor itemText;        // primary text on nav panel
    QColor itemTextDim;     // subdued text (channel names, inactive)
    QColor scrollThumb;     // scrollbar thumb on dark panel
    QColor scrollThumbHover;
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
    QColor primary;   // main body copy
    QColor secondary; // subdued (timestamps, captions)
    QColor tertiary;  // placeholder, hints, very dimmed
    QColor onDark;    // text on dark backgrounds
    QColor onDarkDim; // subdued text on dark backgrounds
    QColor link;      // hyperlinks
    QColor danger;    // error / destructive text
    QColor warning;   // warning-context text
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
    QColor pinnedBg;           // pinned message row tint
    QColor fileChipBg;         // non-image file chip background
    QColor fileChipBorder;     // file chip border
    QColor fileNameDim;        // filename label in image section
    QColor imagePlaceholderBg; // loading-image placeholder fill
    QColor imagePlaceholderBorder;
    QColor replyBarHover; // reply bar hover background
    QColor replyBarHoverBorder;
    QColor replyLink;           // "N replies" link color
    QColor appBadgeBg;          // "APP" tag background next to bot names
    QColor appBadgeText;        // "APP" tag text
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
    FontSizes         fonts;
    FontScales        fontScales;
    Spacing           spacing;

    // Workspace-switcher derived colors (computed per workspace from teamId hash)
    int workspaceHslSaturation; // 65
    int workspaceHslLightness;  // 42
};

// ── Access ────────────────────────────────────────────────────────────────────

// Returns the currently active theme. Call via the Th::c() shorthand below.
const Theme &current();

// Shorthand: Th::c().text.primary
inline const Theme &c() {
    return current();
}

// Formats a QColor for embedding in a Qt stylesheet string.
// Opaque → "#RRGGBB"; with alpha → "rgba(r,g,b,A)" (A is 0–255, not 0.0–1.0).
QString qss(const QColor &color);

// Global application stylesheet built from the active theme.
// Apply via qApp->setStyleSheet(Th::globalQss()) and re-apply on themeChanged.
// Covers only things that can't be done per-widget (QToolTip, etc.).
QString globalQss();

} // namespace Th
