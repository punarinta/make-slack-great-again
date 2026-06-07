// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"
#include "backend/domain.h"

class Session;
class ConvSelectorWidget;
class ComposerWidget;
class QPushButton;

// Dialog for forwarding a message to another conversation.
// User picks a target conversation, optionally adds a comment via the composer,
// and confirms. Accepted → caller reads targetConv() and comment().
class ForwardDialog : public AppDialog {
    Q_OBJECT
public:
    explicit ForwardDialog(const Message &msg, Session *session, QWidget *parent = nullptr);

    ConversationId targetConv() const;
    QString        comment() const;

private:
    ConvSelectorWidget *_selector = nullptr;
    ComposerWidget     *_composer = nullptr;
    QPushButton        *_fwdBtn   = nullptr;
};
