// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "custom_theme_editor.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

CustomThemeEditor::CustomThemeEditor(QWidget *parent) : QWidget(parent) {
    const auto &sp  = Th::c().spacing;
    auto       *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(sp.md);

    // Import: a pasted legacy share string or ia_theme JSON.
    {
        auto *h = new QHBoxLayout;
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(sp.md);
        _import = new StyledLineEdit(this);
        _import->setSize(StyledLineEdit::Size::Small);
        _import->setPlaceholderText(tr("Paste a Slack theme (colour list or JSON)"));
        _importBtn = new StyledButton(tr("Import"), StyledButton::Variant::Secondary, this);
        _importBtn->setSize(StyledButton::Size::Small);
        h->addWidget(_import, 1);
        h->addWidget(_importBtn);
        lay->addLayout(h);
        connect(_importBtn, &QPushButton::clicked, this, &CustomThemeEditor::importText);
        connect(_import, &StyledLineEdit::returnPressed, this, &CustomThemeEditor::importText);
    }

    _inverted = new QCheckBox(tr("Darker sidebar"), this);
    _gradient = new QCheckBox(tr("Window gradient"), this);
    lay->addWidget(_inverted);
    lay->addWidget(_gradient);
    connect(_inverted, &QCheckBox::toggled, this, [this](bool on) {
        if (_syncing)
            return;
        _theme.sidebarInverted = on;
        emitEdited();
    });
    connect(_gradient, &QCheckBox::toggled, this, [this](bool on) {
        if (_syncing)
            return;
        _theme.gradient = on;
        emitEdited();
    });

    _contrast = new QLabel(this);
    _contrast->setWordWrap(true);
    _contrast->hide();
    lay->addWidget(_contrast);

    {
        auto *h = new QHBoxLayout;
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(sp.md);
        _copyBtn = new StyledButton(tr("Copy theme"), StyledButton::Variant::Secondary, this);
        _copyBtn->setSize(StyledButton::Size::Small);
        _slackBtn =
            new StyledButton(tr("Use my Slack theme"), StyledButton::Variant::Secondary, this);
        _slackBtn->setSize(StyledButton::Size::Small);
        _slackBtn->hide();
        h->addWidget(_copyBtn);
        h->addWidget(_slackBtn);
        h->addStretch();
        lay->addLayout(h);
        connect(_copyBtn, &QPushButton::clicked, this, &CustomThemeEditor::copyTheme);
        connect(_slackBtn, &QPushButton::clicked, this, [this] {
            showStatus(tr("Reading your Slack theme…"), false);
            emit slackThemeRequested();
        });
    }

    _status = new QLabel(this);
    _status->setWordWrap(true);
    _status->hide();
    lay->addWidget(_status);

    connect(&ThemeManager::instance(), &ThemeManager::customThemeChanged, this, [this] {
        refreshContrast();
    });
    connect(&ThemeManager::instance(), &ThemeManager::modeChanged, this, [this] {
        refreshContrast();
    });

    setTheme(ThemeManager::instance().customTheme());
    applyTheme();
}

void CustomThemeEditor::setTheme(const Th::CustomTheme &t) {
    _theme = t;
    syncControls();
    refreshContrast();
}

void CustomThemeEditor::syncControls() {
    _syncing = true;
    _inverted->setChecked(_theme.sidebarInverted);
    _gradient->setChecked(_theme.gradient);
    _syncing = false;
}

void CustomThemeEditor::emitEdited() {
    _status->hide();
    emit themeEdited(_theme);
}

void CustomThemeEditor::refreshContrast() {
    // Advisory, like Slack's: sidebar text against the sidebar it sits on, for
    // the mode currently rendered. Ratio 3 is the WCAG floor for large text.
    auto       &mgr   = ThemeManager::instance();
    const auto &built = mgr.customVariant(mgr.effectiveDark());
    const bool  low   = Th::contrastRatio(built.nav.itemText, built.nav.primary) < 3.0;
    _contrast->setText(tr("Low contrast: sidebar text may be hard to read"));
    _contrast->setVisible(low);
}

void CustomThemeEditor::importText() {
    const auto parsed = Th::parseCustomTheme(_import->text());
    if (!parsed) {
        showStatus(
            tr("Not a Slack theme. Paste 8 or 10 colours separated by commas, or theme JSON."), true
        );
        return;
    }
    _import->clear();
    setTheme(*parsed);
    emit themeEdited(_theme);
    showStatus(tr("Theme imported"), false);
}

void CustomThemeEditor::copyTheme() {
    auto &mgr = ThemeManager::instance();
    QGuiApplication::clipboard()->setText(
        Th::legacyShareString(_theme, mgr.customVariant(mgr.effectiveDark()))
    );
    showStatus(tr("Theme copied: paste it into Slack's Import theme field"), false);
}

void CustomThemeEditor::setSlackThemeAvailable(bool on) {
    _slackBtn->setVisible(on);
}

void CustomThemeEditor::showStatus(const QString &text, bool error) {
    _statusError = error;
    _status->setText(text);
    _status->show();
    applyTheme(); // colour follows error/ok
}

void CustomThemeEditor::applyTheme() {
    const auto   &th       = Th::c();
    const QString checkQss = Th::checkBoxQss(th.fonts.md);
    Th::setStyleSheetIfChanged(_inverted, checkQss);
    Th::setStyleSheetIfChanged(_gradient, checkQss);
    Th::setStyleSheetIfChanged(
        _contrast,
        QString("font-size: %1px; color: %2;").arg(th.fonts.caption).arg(Th::qss(th.text.warning))
    );
    Th::setStyleSheetIfChanged(
        _status,
        QString("font-size: %1px; color: %2;")
            .arg(th.fonts.caption)
            .arg(Th::qss(_statusError ? th.text.danger : th.text.secondary))
    );
}
