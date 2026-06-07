// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"
#include "backend/domain.h"

class QFrame;
class QLabel;
class QPushButton;
class Session;

// Confirmation dialog shown before deleting a message.
// Accepted → caller calls backend()->deleteMessage().
class DeleteMessageDialog : public AppDialog {
    Q_OBJECT
public:
    explicit DeleteMessageDialog(
        const Message &msg, Session *session = nullptr, QWidget *parent = nullptr
    );

protected:
    void applyTheme() override;

private:
    QLabel      *_warnLabel = nullptr;
    QFrame      *_msgCard   = nullptr;
    QLabel      *_tsLabel   = nullptr;
    QPushButton *_cancelBtn = nullptr;
    QPushButton *_deleteBtn = nullptr;
};
