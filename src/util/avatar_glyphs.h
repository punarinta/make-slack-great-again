// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Glyph avatars: a white line glyph (Lucide, ISC licence — https://lucide.dev/)
// on a rounded colour tile, like the Claude Code teammates' pictures. The
// glyphs and colours on offer, and the SVG for one pick — rendered by the image
// cache from a file, and by pickers directly.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QString>
#include <vector>

namespace AvatarGlyphs {

struct Glyph {
    QString id;       // Lucide's name: "code-xml"
    QString elements; // its SVG elements, in Lucide's 24-unit box
};

const std::vector<Glyph>  &glyphs();
const std::vector<QColor> &colors();
bool                       hasGlyph(const QString &id);

// A 128-unit tile of `color` with glyph `id` on it; the first glyph for an
// unknown id.
QByteArray svg(const QString &id, const QColor &color);

} // namespace AvatarGlyphs
