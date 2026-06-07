// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"
#include "backend/domain.h"

class Session;

// Confirmation dialog shown before deleting a message.
// Accepted → caller calls backend()->deleteMessage().
class DeleteMessageDialog : public AppDialog {
    Q_OBJECT
public:
    explicit DeleteMessageDialog(
        const Message &msg, Session *session = nullptr, QWidget *parent = nullptr
    );
};
