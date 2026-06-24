// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QColor>
#include <QPushButton>
#include <QString>

// A flat, square icon button: a tinted SVG glyph on a transparent base with a
// circular `surface.highlight` hover pill. Re-tints/re-styles on themeChanged.
// Replaces the per-dialog "× close" buttons (some of which were text glyphs that
// render inconsistently across platforms) with a single SVG-backed widget.
//
//   auto *btn = new IconButton(":/ui/x.svg", 32, 14, parent);
//   connect(btn, &QPushButton::clicked, …);
class IconButton : public QPushButton {
    Q_OBJECT
public:
    explicit IconButton(
        const QString &svgPath, int side = 28, int iconPx = 14, QWidget *parent = nullptr
    );

    // Override the glyph tint (default: theme `icon.def`). Re-applied on theme change.
    void setIconColor(const QColor &color);

    // Swap the glyph (e.g. eye ↔ eye-off for a password reveal toggle).
    void setSvgPath(const QString &svgPath);

private:
    void applyTheme();

    QString _svgPath;
    int     _side   = 28;
    int     _iconPx = 14;
    QColor  _iconColor; // invalid → use theme icon.def
    bool    _hasColor = false;
};
