// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "icon_button.h"

#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

IconButton::IconButton(const QString &svgPath, int side, int iconPx, QWidget *parent)
    : QPushButton(parent), _svgPath(svgPath), _side(side), _iconPx(iconPx) {
    setFixedSize(_side, _side);
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setIconSize(QSize(_iconPx, _iconPx));
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void IconButton::setIconColor(const QColor &color) {
    _iconColor = color;
    _hasColor  = true;
    applyTheme();
}

void IconButton::applyTheme() {
    const QColor tint = _hasColor ? _iconColor : Th::c().icon.def;
    setIcon(svgIcon(_svgPath, QSize(_iconPx, _iconPx), tint));
    setStyleSheet(QString(
                      "IconButton { border: none; background: transparent; border-radius: %1px; }"
                      "IconButton:hover { background: %2; }"
    )
                      .arg(_side / 2)
                      .arg(Th::qss(Th::c().surface.highlight)));
}
