// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "update_bar.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

UpdateBar::UpdateBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(32);
    setStyleSheet("background: #007A5A;");

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 0, 8, 0);
    lay->setSpacing(12);

    _label = new QLabel(this);
    _label->setStyleSheet("color: white; font-size: 12px; background: transparent;");
    lay->addWidget(_label, 1);

    _btn = new QPushButton(this);
    _btn->setFixedHeight(22);
    _btn->setCursor(Qt::PointingHandCursor);
    _btn->setStyleSheet("QPushButton {"
                        "  background: rgba(255,255,255,0.20); color: white;"
                        "  border: 1px solid rgba(255,255,255,0.35); border-radius: 3px;"
                        "  font-size: 12px; font-weight: 600; padding: 0 10px;"
                        "}"
                        "QPushButton:hover   { background: rgba(255,255,255,0.30); }"
                        "QPushButton:pressed { background: rgba(255,255,255,0.15); }");
    connect(_btn, &QPushButton::clicked, this, [this] { emit restartRequested(_staged); });
    lay->addWidget(_btn);

    hide();
}

void UpdateBar::showUpdateReady(const QString &stagedPath) {
    _staged = stagedPath;
#if defined(Q_OS_LINUX)
    _label->setText(tr("A new version of msga has been downloaded. Restart to apply."));
    _btn->setText(tr("Restart now"));
#else
    _label->setText(tr("A new version of msga is ready to install."));
    _btn->setText(tr("Open installer"));
#endif
    show();
}
