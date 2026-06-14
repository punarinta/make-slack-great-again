// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Slack's canvas HTML download leaves emoji shortcodes (":tada:") as literal
// text instead of rendering them. These helpers expand the codes for display
// in the canvas editor while keeping the document round-trippable back to
// markdown (see CanvasDiff::normalizeMd, which reverses the custom-emoji form).
#pragma once

#include <QHash>
#include <QString>

namespace CanvasEmoji {

// Expand emoji shortcodes in canvas body HTML. Only text *outside* tags is
// touched — markup and attributes (section ids like "temp:C:…", inline styles)
// carry colons too and must never be mangled.
//
//  - built-in names      → their Unicode glyph;
//  - custom emoji (a key  → an inline <img src="emoji:<name>">. The custom
//    in customEmoji)        "emoji:" scheme lets CanvasEdit resolve the image
//                           at load time and lets the markdown serializer turn
//                           it back into ":<name>:" on save;
//  - unknown names       → left untouched.
//
// `customEmoji` maps a workspace emoji name to its image URL (or "alias:other").
QString expandInHtml(const QString &html, const QHash<QString, QString> &customEmoji);

} // namespace CanvasEmoji
