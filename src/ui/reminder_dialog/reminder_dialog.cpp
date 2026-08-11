// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "reminder_dialog.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"
#include "util/time_format.h"

#include <QDateEdit>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QTimeEdit>
#include <QVBoxLayout>

ReminderDialog::ReminderDialog(QWidget *parent) : AppDialog(tr("Reminder"), parent) {
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;
    cl->setSpacing(sp.sm);

    // Default: one hour from now, rounded up to a 5-minute boundary so the
    // preset looks intentional rather than "17:43".
    QDateTime def = QDateTime::currentDateTime().addSecs(3600);
    def           = def.addSecs((5 - def.time().minute() % 5) % 5 * 60);
    def.setTime(QTime(def.time().hour(), def.time().minute()));

    _whenLabel = new QLabel(tr("When"));
    cl->addWidget(_whenLabel);
    _date = new QDateEdit(def.date());
    _date->setCalendarPopup(true);
    _date->setMinimumDate(QDate::currentDate());
    _date->setMinimumWidth(240);
    _date->setLocale(TimeFmt::locale());
    cl->addWidget(_date);

    cl->addSpacing(sp.sm);
    _timeLabel = new QLabel(tr("Time"));
    cl->addWidget(_timeLabel);
    _time = new QTimeEdit(def.time());
    _time->setMinimumWidth(240);
    _time->setLocale(TimeFmt::locale());
    // Follow the app's 12/24-hour preference, not the C locale.
    _time->setDisplayFormat(TimeFmt::use24h() ? QStringLiteral("H:mm") : QStringLiteral("h:mm AP"));
    cl->addWidget(_time);

    cl->addSpacing(sp.md);
    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _saveBtn   = new StyledButton(tr("Save"), StyledButton::Variant::Primary);
    addButtonRow(_saveBtn, _cancelBtn); // Cancel → reject() wired by base
    connect(_saveBtn, &QPushButton::clicked, this, &AppDialog::accept);

    applyTheme();
    updateCard();
}

qint64 ReminderDialog::dueAt() const {
    const qint64 picked = QDateTime(_date->date(), _time->time()).toSecsSinceEpoch();
    return std::max(picked, QDateTime::currentSecsSinceEpoch() + 60);
}

void ReminderDialog::applyTheme() {
    AppDialog::applyTheme();
    const QString labelQss =
        QString("color: %1; font-weight: 600;").arg(Th::qss(Th::c().text.secondary));
    _whenLabel->setStyleSheet(labelQss);
    _timeLabel->setStyleSheet(labelQss);
    // QDateEdit/QTimeEdit are QAbstractSpinBoxes the app palette doesn't reach —
    // same hand styling as the composer's SchedulePopup.
    const QString editQss = QString(
                                "QDateEdit, QTimeEdit {"
                                "  border: 1px solid %1; border-radius: 4px;"
                                "  padding: 4px 8px; font-size: %4px; color: %2; background: %3;"
                                "}"
                                "QDateEdit:focus, QTimeEdit:focus { border-color: %5; }"
                                "QDateEdit::up-button, QDateEdit::down-button,"
                                "QTimeEdit::up-button, QTimeEdit::down-button { width: 14px; }"
    )
                                .arg(Th::qss(Th::c().divider.strong))
                                .arg(Th::qss(Th::c().text.primary))
                                .arg(Th::qss(Th::c().surface.raised))
                                .arg(Th::c().fonts.md)
                                .arg(Th::qss(Th::c().accent.def));
    _date->setStyleSheet(editQss);
    _time->setStyleSheet(editQss);
}
