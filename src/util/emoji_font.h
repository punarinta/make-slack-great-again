// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFont>

// Returns a font suitable for rendering Unicode emoji at the given pixel size.
// Uses the platform-native color emoji font (Noto Color Emoji / Apple Color Emoji / Segoe UI
// Emoji).
inline QFont emojiFont(int pixelSize = 20) {
    QFont f;
    f.setPixelSize(pixelSize);
#if defined(Q_OS_WIN)
    f.setFamily("Segoe UI Emoji");
#elif defined(Q_OS_MAC)
    f.setFamily("Apple Color Emoji");
#else
    f.setFamilies({"Noto Color Emoji", "Noto Emoji"});
#endif
    return f;
}

// Returns a CSS font-family string for use in Qt HTML (QTextDocument inline styles).
inline QString emojiFontFamily() {
#if defined(Q_OS_WIN)
    return QStringLiteral("Segoe UI Emoji");
#elif defined(Q_OS_MAC)
    return QStringLiteral("Apple Color Emoji");
#else
    return QStringLiteral("Noto Color Emoji, Noto Emoji");
#endif
}
