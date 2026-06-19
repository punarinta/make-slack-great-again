// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "edit_mode_banner.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

EditModeBanner::EditModeBanner(QWidget *parent) : QWidget(parent) {
    setObjectName("editBanner");
    setFixedHeight(30);

    auto       *layout = new QHBoxLayout(this);
    const auto &sp     = Th::c().spacing;
    layout->setContentsMargins(sp.md, 0, sp.sm, 0);
    layout->setSpacing(sp.sm);

    auto *label = new QLabel(tr("Editing message"), this);
    layout->addWidget(label, 1);

    _cancelBtn = new QToolButton(this);
    _cancelBtn->setFixedSize(20, 20);
    _cancelBtn->setIconSize(QSize(12, 12));
    _cancelBtn->setFocusPolicy(Qt::NoFocus);
    _cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(_cancelBtn, &QToolButton::clicked, this, &EditModeBanner::cancelClicked);
    layout->addWidget(_cancelBtn);

    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void EditModeBanner::applyTheme() {
    setStyleSheet(QString(
                      "QWidget#editBanner {"
                      "  background: %1;"
                      "  border-left: 3px solid %2;"
                      "  border-bottom: 1px solid %3;"
                      "}"
                      "QLabel { border: none; background: transparent;"
                      "  font-size: %4px; color: %5; font-weight: 600; }"
                      "QToolButton { border: none; border-radius: 3px; background: transparent; }"
                      "QToolButton:hover { background: %3; }"
    )
                      .arg(
                          Th::qss(Th::c().editBanner.bg),
                          Th::qss(Th::c().editBanner.border),
                          Th::qss(Th::c().editBanner.accent)
                      )
                      .arg(Th::c().fonts.caption)
                      .arg(Th::qss(Th::c().editBanner.text)));
    _cancelBtn->setIcon(svgIcon(":/ui/x.svg", QSize(12, 12), Th::c().editBanner.text));
}
