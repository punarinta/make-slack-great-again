// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFont>
#include <QFontMetrics>

namespace Ui {

// Fonts the per-frame paint paths derive from the app font (and the theme's font
// scales), with their metrics, so a row paint doesn't copy + re-derive + measure
// a QFont every time.
//
// Only the CURRENT app font / theme is cached, built lazily on the first get()
// after an invalidation — nothing is precomputed for other themes or sizes. The
// set is invalidated when the app font changes (the font-size setting, system
// font changes: QEvent::ApplicationFontChange on qApp) and when a theme is
// applied (ThemeManager::setTheme, before themeChanged, so its handlers already
// see the new set).
// Screen / DPR changes need nothing: QFont sizes are in points (logical pixels),
// and the metrics use the same default DPI a per-paint QFontMetrics(font) would.
//
// The reference get() returns stays valid for the run: the set is rebuilt in
// place by the first get() after an invalidation, so call get() at the start of
// each paint rather than holding on to it. Caches that derive from these fonts
// (shaped text, measured widths) key on `generation`.
struct Fonts {
    quint32 generation = 0; // bumped by every invalidate()

    QFont        normal; // app font, explicit Normal weight (list rows, labels)
    QFontMetrics normalFm;
    QFont        demiBold; // app font, DemiBold (unread rows, overview card names)
    QFontMetrics demiBoldFm;

    QFont        msgName; // message header: bold author name
    QFontMetrics msgNameFm;
    QFont        msgTs; // message header: timestamp (0.85×)
    QFontMetrics msgTsFm;
    QFont        tagBadge; // "APP" / "EXT" pill (0.62× bold)
    QFontMetrics tagBadgeFm;
    QFont        reactionCount; // reaction chip count (0.82×)
    QFontMetrics reactionCountFm;
    QFont        countBadge; // chats list: unread badge / huddle count (0.78× bold)
    QFontMetrics countBadgeFm;
    QFont        youLabel; // chats list: "you" after your own DM (0.88×)
    QFontMetrics youLabelFm;
    QFont        cardTs; // overview card timestamp (theme fontScales.timestamp)

    // The current set; built on first use after an invalidation.
    static const Fonts &get();
    // Drop the cached set (next get() rebuilds). Cheap; safe to call often.
    static void         invalidate();

private:
    Fonts();
    void rebuild();
};

} // namespace Ui
