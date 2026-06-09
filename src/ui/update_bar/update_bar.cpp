// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "update_bar.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QPainter>
#include <QStyleOption>

UpdateBar::UpdateBar(QWidget *parent) : QWidget(parent) {
    setObjectName("updateBar");
    setFixedHeight(32);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 0, 8, 0);
    lay->setSpacing(12);

    _label = new QLabel(this);
    lay->addWidget(_label, 1);

    _btn = new QPushButton(this);
    _btn->setFixedHeight(22);
    _btn->setCursor(Qt::PointingHandCursor);
    connect(_btn, &QPushButton::clicked, this, [this] { emit restartRequested(); });
    lay->addWidget(_btn);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &UpdateBar::applyTheme);

    hide();
}

void UpdateBar::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void UpdateBar::applyTheme() {
    const auto &th = Th::c();

    setStyleSheet(
        QString("QWidget#updateBar {"
                "  background: %1;"
                "  border-bottom: 1px solid %2;"
                "}"
                "QLabel { background: transparent; color: %3; font-size: %4px; font-weight: 600; }"
                "QPushButton {"
                "  background: %5; color: %6;"
                "  border: none; border-radius: 3px;"
                "  font-size: %4px; font-weight: 600; padding: 0 10px;"
                "}"
                "QPushButton:hover   { background: %7; }"
                "QPushButton:pressed { background: %7; }")
            .arg(
                Th::qss(th.updateBanner.bg),
                Th::qss(th.updateBanner.border),
                Th::qss(th.updateBanner.text)
            )
            .arg(th.fonts.caption)
            .arg(Th::qss(th.danger.def), Th::qss(th.text.onDark), Th::qss(th.danger.hover))
    );
}

void UpdateBar::showUpdateReady() {
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    _label->setText(tr("A new version of msga has been downloaded. Restart to apply."));
    _btn->setText(tr("Restart now"));
#else
    _label->setText(tr("A new version of msga is ready to install."));
    _btn->setText(tr("Open installer"));
#endif
    show();
}
