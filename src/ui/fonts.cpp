// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "fonts.h"
#include "theme.h"

#include <QApplication>

namespace Ui {

namespace {

bool    g_valid      = false;
quint32 g_generation = 1; // items start at 0, so their first paint always shapes

QFont scaled(QFont f, qreal k) {
    f.setPointSizeF(f.pointSizeF() * k);
    return f;
}

QFont bold(QFont f) {
    f.setBold(true);
    return f;
}

QFont weighted(QFont f, QFont::Weight w) {
    f.setWeight(w);
    return f;
}

// Drops the set when the app font changes: the font-size setting
// (QApplication::setFont) or a system font change. QGuiApplication sends
// ApplicationFontChange to the application object itself before any widget gets
// its copy, so the widgets' font-change handling already sees the new set. Only
// the app's own copy counts — the per-widget ones would re-invalidate a set a
// widget's handler just rebuilt. (QGuiApplication::fontChanged would spare the
// filter, but Qt deprecates it in favour of this event.)
class AppFontWatcher : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::ApplicationFontChange && watched == qApp)
            Fonts::invalidate();
        return false;
    }
};

} // namespace

Fonts::Fonts()
    : normalFm(normal), demiBoldFm(demiBold), msgNameFm(msgName), msgTsFm(msgTs),
      tagBadgeFm(tagBadge), reactionCountFm(reactionCount), countBadgeFm(countBadge),
      youLabelFm(youLabel) {}

void Fonts::rebuild() {
    // Every derivation here is the exact recipe its paint path used inline, so
    // the default-size rendering doesn't move by a pixel.
    const QFont app = QApplication::font();
    generation      = g_generation;

    normal   = weighted(app, QFont::Normal);
    demiBold = weighted(app, QFont::DemiBold);

    msgName       = bold(app);
    msgTs         = scaled(app, 0.85);
    tagBadge      = bold(scaled(app, 0.62));
    reactionCount = scaled(app, 0.82);
    countBadge    = bold(scaled(app, 0.78));
    youLabel      = scaled(normal, 0.88);
    cardTs        = scaled(app, Th::c().fontScales.timestamp);

    normalFm        = QFontMetrics(normal);
    demiBoldFm      = QFontMetrics(demiBold);
    msgNameFm       = QFontMetrics(msgName);
    msgTsFm         = QFontMetrics(msgTs);
    tagBadgeFm      = QFontMetrics(tagBadge);
    reactionCountFm = QFontMetrics(reactionCount);
    countBadgeFm    = QFontMetrics(countBadge);
    youLabelFm      = QFontMetrics(youLabel);
}

const Fonts &Fonts::get() {
    static Fonts fonts;
    if (!g_valid) {
        static const bool hooked = [] {
            if (qApp)
                qApp->installEventFilter(new AppFontWatcher(qApp));
            return true;
        }();
        Q_UNUSED(hooked);
        fonts.rebuild();
        g_valid = true;
    }
    return fonts;
}

void Fonts::invalidate() {
    if (!g_valid)
        return; // nothing built since the last invalidation: keep the generation
    g_valid = false;
    ++g_generation;
}

} // namespace Ui
