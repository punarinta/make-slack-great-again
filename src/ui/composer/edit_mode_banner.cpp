// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "edit_mode_banner.h"
#include "ui/icon_utils.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

EditModeBanner::EditModeBanner(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("editBanner");
    setFixedHeight(30);
    setStyleSheet(
        "QWidget#editBanner {"
        "  background: #FFF8EE;"
        "  border-left: 3px solid #E8A917;"
        "  border-bottom: 1px solid #F0DFA0;"
        "}"
        "QLabel { border: none; background: transparent;"
        "  font-size: 12px; color: #7A5800; font-weight: 600; }"
        "QToolButton { border: none; border-radius: 3px; background: transparent; }"
        "QToolButton:hover { background: #F5D98C; }"
    );

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 4, 0);
    layout->setSpacing(4);

    auto *label = new QLabel(tr("Editing message"), this);
    layout->addWidget(label, 1);

    auto *cancelBtn = new QToolButton(this);
    cancelBtn->setFixedSize(20, 20);
    cancelBtn->setIconSize(QSize(12, 12));
    cancelBtn->setIcon(svgIcon(":/ui/x.svg", QSize(12, 12), QColor("#7A5800")));
    cancelBtn->setFocusPolicy(Qt::NoFocus);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(cancelBtn, &QToolButton::clicked, this, &EditModeBanner::cancelClicked);
    layout->addWidget(cancelBtn);

    hide();
}
