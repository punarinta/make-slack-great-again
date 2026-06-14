// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "huddle_banner.h"
#include "ui/icon_utils.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyleOption>

HuddleBanner::HuddleBanner(QWidget *parent) : QWidget(parent) {
    setObjectName("huddleBanner");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(34);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(16, 0, 8, 0);
    lay->setSpacing(8);

    _icon = new QLabel(this);
    _icon->setFixedSize(16, 16);
    lay->addWidget(_icon);

    _label = new QLabel(tr("A huddle is happening"), this);
    lay->addWidget(_label, 1);

    _btn = new QPushButton(tr("Join"), this);
    _btn->setFixedHeight(22);
    _btn->setCursor(Qt::PointingHandCursor);
    connect(_btn, &QPushButton::clicked, this, [this] { emit joinClicked(); });
    lay->addWidget(_btn);

    // Tooltip clarifying that joining opens Slack's web client (we can't join the
    // huddle's media ourselves — see docs/HUDDLES_PLAN.md).
    _joinTooltip = new PopupTooltip(this);
    _btn->installEventFilter(this);

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &HuddleBanner::applyTheme
    );

    hide();
}

void HuddleBanner::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

bool HuddleBanner::eventFilter(QObject *obj, QEvent *e) {
    if (obj == _btn && _joinTooltip) {
        if (e->type() == QEvent::Enter)
            _joinTooltip->showAbove(
                tr("Opens the huddle in Slack for web"),
                QRect(_btn->mapToGlobal(QPoint(0, 0)), _btn->size())
            );
        else if (e->type() == QEvent::Leave)
            _joinTooltip->hide();
    }
    return QWidget::eventFilter(obj, e);
}

void HuddleBanner::applyTheme() {
    const auto &th = Th::c();

    // Solid green bar (Slack's huddle colour), white text/icon, white Join pill
    // with green label — high contrast and unmistakably actionable. Rebuilt on
    // every theme change so the icon tint never goes stale (see .rules).
    _icon->setPixmap(svgPixmap(":/ui/headphones.svg", QSize(16, 16), th.text.onDark));

    setStyleSheet(
        QString(
            "QWidget#huddleBanner { background: %1; }"
            "QLabel { background: transparent; color: %2; font-size: %3px; font-weight: 600; }"
            "QPushButton {"
            "  background: %2; color: %1;"
            "  border: none; border-radius: 3px;"
            "  font-size: %3px; font-weight: 600; padding: 0 12px;"
            "}"
            "QPushButton:hover   { background: %4; }"
            "QPushButton:pressed { background: %4; }"
        )
            .arg(Th::qss(th.presence.online), Th::qss(th.text.onDark))
            .arg(th.fonts.caption)
            .arg(Th::qss(th.surface.raised))
    );
}
