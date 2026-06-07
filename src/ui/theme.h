// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Named color constants for the Slack aubergine theme.
// Used by code that sets colors programmatically (QPainter, QPalette).
// Widget styling lives in resources/style.qss, loaded at startup.
#pragma once

#include <QColor>

namespace Theme {

// Sidebar / navigation panel
inline const QColor kSidebarBg{"#3F0E40"};       // workspace sidebar
inline const QColor kConvPanelBg{"#350D36"};     // conversation list panel
inline const QColor kWorkspaceIconBg{"#4A154B"}; // workspace icon chip
inline const QColor kHoverItem{"#522653"};       // hovered conv item

// Text
inline const QColor kTextOnDark{"#FFFFFF"};
inline const QColor kTextOnDarkDim{"#CFC3CF"}; // unread conv names
inline const QColor kTextPrimary{"#1D1C1D"};
inline const QColor kTextSecondary{"#616061"};

// Interactive
inline const QColor kSelectedItem{"#7D5D7E"}; // active conversation pill (~35% white over panel bg)
inline const QColor kUnreadBadge{"#E01E5A"};  // unread count badge
inline const QColor kMentionBadge{"#CD2553"}; // @mention badge

// Message area
inline const QColor kMessageBg{"#FFFFFF"};
inline const QColor kDivider{"#E8E8E8"};
inline const QColor kLink{"#1264A3"};

} // namespace Theme
