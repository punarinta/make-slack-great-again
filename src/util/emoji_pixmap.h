// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QColor>
#include <QPixmap>
#include <QRect>

class QPainter;

// Cached rasterization of Unicode emoji. Shaping emoji text through the color
// emoji font is expensive to repeat: FreeType decodes the glyph's embedded PNG
// on every load (even for advance metrics), so per-frame drawText/
// horizontalAdvance calls in paint paths turn into per-frame PNG decodes.
// These helpers render an emoji ONCE per (glyph, size, color, DPR) into a
// QPixmapCache-backed pixmap; painting it is a plain blit.
namespace EmojiPix {

// `emoji` rendered through emojiFont(px), tight-cropped to the glyph ink and
// downscaled if it overflows a px×px box, at device pixel ratio `dpr`.
// `color` only affects glyphs served by a monochrome fallback font; color-font
// glyphs ignore the pen. Returns a null pixmap for whitespace-only input.
QPixmap pixmap(const QString &emoji, int px, const QColor &color, qreal dpr);

// Draw `emoji` at size px centered inside `box` (logical coordinates).
void draw(QPainter &p, const QRect &box, const QString &emoji, int px, const QColor &color);

// Logical width the emoji will occupy when drawn at size px (its ink width,
// capped at px) — the replacement for QFontMetrics::horizontalAdvance, whose
// color-emoji answer is both wrong (~2× the ink) and costs a PNG decode.
int width(const QString &emoji, int px, qreal dpr);

} // namespace EmojiPix
