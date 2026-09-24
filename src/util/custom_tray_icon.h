// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

// The user's own tray icon (Settings → Appearance → Tray icon, GitHub issue #73).
// The picture is normalised into a square PNG the app owns under its data dir,
// so the original may move or vanish. The "use it" switch and the monochrome
// option are separate settings: turning the switch off keeps the file, so
// turning it back on restores the same picture without another pick.
//
// Monochrome (the default) turns the picture into a white silhouette, like the
// built-in icon and most tray icons. On macOS that silhouette is the template
// image the menu bar tints for its light/dark look. With it off a colour icon
// keeps its colours (and on macOS is not a template).
namespace CustomTrayIcon {

// Side of the stored square. Trays paint 16–32 logical px; 128 leaves room
// for any DPR and matches the pixmap updateTrayIcon() composes.
constexpr int kStoredSize = 128;

// Bytes → image. SVG renders straight at kStoredSize (a 24 px Lucide icon
// would otherwise be upscaled blurry); rasters decode bounded, so a huge photo
// never lands in memory whole. Null when the bytes are no image we can read.
QImage decode(const QByteArray &bytes);

// Fit into a transparent kStoredSize square, aspect kept and centred — a logo
// must not lose its edges to a crop. Null in → null out. Pure.
QImage prepare(const QImage &src);

// White silhouette. The shape comes from the alpha channel when the picture
// has transparency inside its visible area (a logo cut out of its background);
// an opaque picture (a logo on a solid backdrop, a photo) instead keys on how
// far each pixel's colour is from the backdrop, sampled at the corners — so a
// dark-on-light and a light-on-dark logo both come out as their mark, solid,
// not as a white square. Pure.
QImage toMonochrome(const QImage &src);

// prepare() + the monochrome option, as the tray will show it. Pure.
QImage styled(const QImage &src, bool monochrome);

// Normalise and write the picture. False (and nothing changed) on failure.
bool install(const QImage &src);
// Delete the picture and switch the custom icon off.
void remove();

bool    hasImage();
QString path();

bool enabled();
void setEnabled(bool on);
bool monochrome();
void setMonochrome(bool on);

// The stored picture, normalised but without the monochrome option applied —
// what the picker dialog previews and restyles live. Null when there is none.
QImage stored();

// What the tray should show: styled stored picture, or null when the custom
// icon is off or its file is gone (the built-in icon shows then). Cached, so
// the per-unread-change tray repaint does not re-read the file.
QImage current();

} // namespace CustomTrayIcon
