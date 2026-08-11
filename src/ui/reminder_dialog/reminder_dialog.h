// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

class QDateEdit;
class QLabel;
class QTimeEdit;
class StyledButton;

// Custom date/time picker for a message reminder ("Remind me → Custom…").
// Accepted → caller reads dueAt() and calls Session::setMessageReminder().
class ReminderDialog : public AppDialog {
    Q_OBJECT
public:
    explicit ReminderDialog(QWidget *parent = nullptr);

    // The chosen due time (Unix seconds), floored to one minute from now so an
    // in-the-past pick can't produce a reminder that instantly fires.
    qint64 dueAt() const;

protected:
    void applyTheme() override;

private:
    QLabel       *_whenLabel = nullptr;
    QLabel       *_timeLabel = nullptr;
    QDateEdit    *_date      = nullptr;
    QTimeEdit    *_time      = nullptr;
    StyledButton *_cancelBtn = nullptr;
    StyledButton *_saveBtn   = nullptr;
};
