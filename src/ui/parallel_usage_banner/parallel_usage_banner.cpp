// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "parallel_usage_banner.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyleOption>

namespace {
// Setup docs explaining that every device needs its own Slack app keys.
constexpr auto kSetupDocsUrl =
    "https://github.com/punarinta/make-slack-great-again/blob/master/docs/SETUP_SLACK.md";
} // namespace

ParallelUsageBanner::ParallelUsageBanner(QWidget *parent) : QWidget(parent) {
    setObjectName("parallelUsageBanner");
    setAttribute(Qt::WA_StyledBackground);

    auto       *lay = new QHBoxLayout(this);
    const auto &sp  = Th::c().spacing;
    lay->setContentsMargins(sp.xl, sp.xs, sp.sm, sp.xs);
    lay->setSpacing(sp.md);

    _label = new QLabel(this);
    _label->setTextFormat(Qt::RichText);
    _label->setWordWrap(true);
    _label->setOpenExternalLinks(true);
    _label->setText(
        tr("The same app keys are running on another device and keep interrupting your Slack "
           "connection.") +
        QStringLiteral(" <a href=\"%1\">%2</a>")
            .arg(QString::fromLatin1(kSetupDocsUrl), tr("How to solve this?"))
    );
    lay->addWidget(_label, 1);

    _closeBtn = new QPushButton(this);
    _closeBtn->setObjectName("parallelUsageClose");
    _closeBtn->setFixedSize(20, 20);
    _closeBtn->setCursor(Qt::PointingHandCursor);
    connect(_closeBtn, &QPushButton::clicked, this, &QWidget::hide);
    lay->addWidget(_closeBtn, 0, Qt::AlignTop);

    applyTheme();
    connect(
        &ThemeManager::instance(),
        &ThemeManager::themeChanged,
        this,
        &ParallelUsageBanner::applyTheme
    );

    hide();
}

void ParallelUsageBanner::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void ParallelUsageBanner::applyTheme() {
    const auto &th = Th::c();

    // Solid danger-red bar with light text, matching the transient error banner.
    // The link colour comes from the label palette (not inline HTML) so it never
    // bakes a stale theme — the text is only set once, in the constructor.
    QPalette pal = _label->palette();
    pal.setColor(QPalette::Link, th.text.onDark);
    _label->setPalette(pal);

    // Re-tint the close glyph on every theme change (see .rules).
    _closeBtn->setIcon(svgIcon(":/ui/x.svg", QSize(14, 14), th.text.onDark));

    setStyleSheet(QString(
                      "QWidget#parallelUsageBanner { background: %1; }"
                      "QLabel { background: transparent; color: %2; font-size: %3px; }"
                      "QPushButton#parallelUsageClose {"
                      "  background: transparent; border: none; border-radius: 3px;"
                      "}"
                      "QPushButton#parallelUsageClose:hover { background: %4; }"
    )
                      .arg(Th::qss(th.danger.icon), Th::qss(th.text.onDark))
                      .arg(th.fonts.md)
                      .arg(Th::qss(th.danger.hover)));
}
