// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "update_bar.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

UpdateBar::UpdateBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(32);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 0, 8, 0);
    lay->setSpacing(12);

    _label = new QLabel(this);
    lay->addWidget(_label, 1);

    _btn = new QPushButton(this);
    _btn->setFixedHeight(22);
    _btn->setCursor(Qt::PointingHandCursor);
    connect(_btn, &QPushButton::clicked, this, [this] { emit restartRequested(_staged); });
    lay->addWidget(_btn);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &UpdateBar::applyTheme);

    hide();
}

void UpdateBar::applyTheme() {
    const auto &th = Th::c();

    setStyleSheet(QString("background: %1;").arg(Th::qss(th.accent.def)));

    _label->setStyleSheet(QString("color: %1; font-size: %2px; background: transparent;")
                              .arg(Th::qss(th.accent.text))
                              .arg(th.fonts.caption));

    _btn->setStyleSheet(QString("QPushButton {"
                                "  background: rgba(255,255,255,0.20); color: %1;"
                                "  border: 1px solid rgba(255,255,255,0.35); border-radius: 3px;"
                                "  font-size: %2px; font-weight: 600; padding: 0 10px;"
                                "}"
                                "QPushButton:hover   { background: rgba(255,255,255,0.30); }"
                                "QPushButton:pressed { background: rgba(255,255,255,0.15); }")
                            .arg(Th::qss(th.accent.text))
                            .arg(th.fonts.caption));
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
