// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "session_status_dialog.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

SessionStatusDialog::SessionStatusDialog(
    const std::vector<std::pair<QString, QString>> &rows, QWidget *parent
)
    : AppDialog(tr("Session status"), parent) {
    const auto &sp   = Th::c().spacing;
    auto       *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(sp.xl);
    grid->setVerticalSpacing(sp.sm);
    int row = 0;
    for (const auto &[label, value] : rows) {
        auto *l = new QLabel(label);
        auto *v = new QLabel(value);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->setToolTip(value); // in case a long path is cut off
        grid->addWidget(l, row, 0, Qt::AlignTop | Qt::AlignLeft);
        grid->addWidget(v, row, 1, Qt::AlignTop | Qt::AlignLeft);
        _labels << l;
        _values << v;
        ++row;
    }
    grid->setColumnStretch(1, 1);
    contentLayout()->addLayout(grid);

    _closeBtn = new StyledButton(tr("Close"), StyledButton::Variant::Primary);
    addButtonRow(_closeBtn);
    connect(_closeBtn, &QPushButton::clicked, this, &AppDialog::accept);

    applyTheme();
    updateCard();
}

int SessionStatusDialog::cardWidth(int availOverlayWidth) const {
    // Like the base: the floor gives way on a window too narrow to hold it.
    const int minW = std::min(480, std::max(availOverlayWidth, 1));
    return std::clamp(availOverlayWidth, minW, 760);
}

void SessionStatusDialog::applyTheme() {
    AppDialog::applyTheme();
    for (auto *l : _labels)
        l->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.secondary)));
    for (auto *v : _values)
        v->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
}
