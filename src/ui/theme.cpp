// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "theme.h"
#include "theme_manager.h"

namespace Th {

const Theme &current() {
    return ThemeManager::instance().theme();
}

QString qss(const QColor &c) {
    if (c.alpha() == 255)
        return c.name(); // "#RRGGBB"
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// ── Default theme: Slack dark-sidebar ────────────────────────────────────────

const Theme kSlackDark = {
    .nav =
        {
            .bg               = QColor("#3F0E40"),
            .primary          = QColor("#350D36"),
            .workspaceBubble  = QColor("#4A154B"),
            .itemHover        = QColor("#522653"),
            .itemSelected     = QColor("#E1DBE1"), // white ~85% over nav.primary
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
            .primary   = QColor("#1D1C1D"),
            .secondary = QColor("#616061"),
            .tertiary  = QColor("#888888"),
            .onDark    = QColor("#FFFFFF"),
            .onDarkDim = QColor("#CFC3CF"),
            .link      = QColor("#1264A3"),
            .danger    = QColor("#C0392B"),
            .warning   = QColor("#7A5800"),
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

const Theme &defaultTheme() {
    return kSlackDark;
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
