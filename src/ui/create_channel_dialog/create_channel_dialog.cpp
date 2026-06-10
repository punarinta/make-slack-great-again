// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "create_channel_dialog.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

static constexpr int kMaxNameLen = 80;

CreateChannelDialog::CreateChannelDialog(const QString &workspaceName, QWidget *parent)
    : AppDialog(tr("Create a channel"), parent), _workspaceName(workspaceName) {
    auto *cl = contentLayout();

    _pages = new QStackedWidget;
    cl->addWidget(_pages);

    // ── Step 1: Name ──────────────────────────────────────────────────────────
    auto *page1 = new QWidget;
    {
        auto *lay = new QVBoxLayout(page1);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);

        _nameSectionLabel = new QLabel(tr("Name"), page1);
        QFont lf          = _nameSectionLabel->font();
        lf.setBold(true);
        _nameSectionLabel->setFont(lf);
        lay->addWidget(_nameSectionLabel);

        _nameEdit = new StyledLineEdit(page1);
        _nameEdit->setPrefix("#");
        _nameEdit->setMaxLength(kMaxNameLen);
        _nameEdit->setPlaceholderText(tr("e.g. plan-budget"));
        lay->addWidget(_nameEdit);

        _helperLabel = new QLabel(
            tr("Channels are where conversations happen around a topic. Use a name that is easy "
               "to find and understand."),
            page1
        );
        _helperLabel->setWordWrap(true);
        lay->addWidget(_helperLabel);

        lay->addSpacing(8);

        auto *btnRow = new QHBoxLayout;
        btnRow->addStretch();
        _nextBtn = new QPushButton(tr("Next"), page1);
        _nextBtn->setEnabled(false);
        _nextBtn->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(_nextBtn);
        lay->addLayout(btnRow);
    }
    _pages->addWidget(page1);

    // ── Step 2: Visibility ────────────────────────────────────────────────────
    auto *page2 = new QWidget;
    {
        auto *lay = new QVBoxLayout(page2);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);

        _channelSubtitle = new QLabel(page2);
        lay->addWidget(_channelSubtitle);

        lay->addSpacing(4);

        _visSectionLabel = new QLabel(tr("Visibility"), page2);
        QFont lf         = _visSectionLabel->font();
        lf.setBold(true);
        _visSectionLabel->setFont(lf);
        lay->addWidget(_visSectionLabel);

        _publicRadio = new QRadioButton(page2);
        _publicRadio->setChecked(true);
        lay->addWidget(_publicRadio);

        _privateRadio = new QRadioButton(tr("Private — only specific people"), page2);
        lay->addWidget(_privateRadio);

        _privateDesc = new QLabel(tr("Can only be viewed or joined by invitation"), page2);
        _privateDesc->setContentsMargins(24, 0, 0, 0);
        lay->addWidget(_privateDesc);

        lay->addSpacing(8);

        auto *btnRow = new QHBoxLayout;
        _stepLabel   = new QLabel(tr("Step 2 of 2"), page2);
        _backBtn     = new QPushButton(tr("Back"), page2);
        _createBtn   = new QPushButton(tr("Create"), page2);
        _backBtn->setCursor(Qt::PointingHandCursor);
        _createBtn->setCursor(Qt::PointingHandCursor);

        btnRow->addWidget(_stepLabel);
        btnRow->addStretch();
        btnRow->addWidget(_backBtn);
        btnRow->addSpacing(8);
        btnRow->addWidget(_createBtn);
        lay->addLayout(btnRow);
    }
    _pages->addWidget(page2);

    // ── Connections ───────────────────────────────────────────────────────────
    connect(_nameEdit, &StyledLineEdit::textChanged, this, [this](const QString &) {
        updateNextEnabled();
    });
    connect(_nameEdit, &StyledLineEdit::returnPressed, this, [this] {
        if (_nextBtn->isEnabled())
            goNext();
    });
    connect(_nextBtn, &QPushButton::clicked, this, &CreateChannelDialog::goNext);
    connect(_backBtn, &QPushButton::clicked, this, &CreateChannelDialog::goBack);
    connect(_createBtn, &QPushButton::clicked, this, &QDialog::accept);

    applyTheme();
    updateCard();
}

QString CreateChannelDialog::channelName() const {
    return _nameEdit->text().trimmed().toLower().replace(' ', '-');
}

bool CreateChannelDialog::isPrivate() const {
    return _privateRadio->isChecked();
}

void CreateChannelDialog::goNext() {
    const QString raw     = _nameEdit->text().trimmed();
    const QString display = "# " + raw.toLower().replace(' ', '-');
    _channelSubtitle->setText(display);

    const QString wsName = _workspaceName.isEmpty() ? tr("this workspace") : _workspaceName;
    _publicRadio->setText(tr("Public — anyone in %1").arg(wsName));

    _pages->setCurrentIndex(1);
    updateCard();
}

void CreateChannelDialog::goBack() {
    _pages->setCurrentIndex(0);
    updateCard();
}

void CreateChannelDialog::updateNextEnabled() {
    _nextBtn->setEnabled(!_nameEdit->text().trimmed().isEmpty());
}

void CreateChannelDialog::applyTheme() {
    AppDialog::applyTheme();

    if (_nameSectionLabel)
        _nameSectionLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));

    if (_helperLabel)
        _helperLabel->setStyleSheet(QString("color: %1; font-size: %2px;")
                                        .arg(Th::qss(Th::c().text.secondary))
                                        .arg(Th::c().fonts.sm));

    if (_nextBtn)
        _nextBtn->setStyleSheet(QString(
                                    "QPushButton {"
                                    "  background: %1; color: %2; border: none;"
                                    "  border-radius: 6px; padding: 8px 20px; font-weight: bold;"
                                    "}"
                                    "QPushButton:hover { background: %3; }"
                                    "QPushButton:disabled {"
                                    "  background: %4; color: %5;"
                                    "}"
        )
                                    .arg(
                                        Th::qss(Th::c().accent.def),
                                        Th::qss(Th::c().accent.text),
                                        Th::qss(Th::c().accent.dark),
                                        Th::qss(Th::c().surface.highlightStrong),
                                        Th::qss(Th::c().text.tertiary)
                                    ));

    if (_channelSubtitle)
        _channelSubtitle->setStyleSheet(QString("color: %1; font-size: %2px;")
                                            .arg(Th::qss(Th::c().text.secondary))
                                            .arg(Th::c().fonts.sm));

    if (_visSectionLabel)
        _visSectionLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));

    if (_publicRadio)
        _publicRadio->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));

    if (_privateRadio)
        _privateRadio->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));

    if (_privateDesc)
        _privateDesc->setStyleSheet(QString("color: %1; font-size: %2px;")
                                        .arg(Th::qss(Th::c().text.secondary))
                                        .arg(Th::c().fonts.sm));

    if (_stepLabel)
        _stepLabel->setStyleSheet(QString("color: %1; font-size: %2px;")
                                      .arg(Th::qss(Th::c().text.secondary))
                                      .arg(Th::c().fonts.sm));

    if (_backBtn)
        _backBtn->setStyleSheet(QString(
                                    "QPushButton {"
                                    "  border: 1px solid %1; border-radius: 6px;"
                                    "  padding: 8px 20px; background: %2;"
                                    "}"
                                    "QPushButton:hover { background: %3; }"
        )
                                    .arg(
                                        Th::qss(Th::c().divider.strong),
                                        Th::qss(Th::c().surface.raised),
                                        Th::qss(Th::c().surface.sunken)
                                    ));

    if (_createBtn)
        _createBtn->setStyleSheet(QString(
                                      "QPushButton {"
                                      "  background: %1; color: %2; border: none;"
                                      "  border-radius: 6px; padding: 8px 20px; font-weight: bold;"
                                      "}"
                                      "QPushButton:hover { background: %3; }"
        )
                                      .arg(
                                          Th::qss(Th::c().accent.def),
                                          Th::qss(Th::c().accent.text),
                                          Th::qss(Th::c().accent.dark)
                                      ));
}
