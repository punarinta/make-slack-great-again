// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"
#include "backend/domain.h"

#include <vector>

class Dropdown;
class QFrame;
class Session;
class ConvSelectorWidget;
class ComposerWidget;
class StyledButton;

// Dialog for forwarding a message to another conversation.
// User picks a target conversation, optionally adds a comment via the composer,
// and confirms. Accepted → caller reads targetSession(), targetConv() and
// comment().
class ForwardDialog : public AppDialog {
    Q_OBJECT
public:
    // A workspace the message may be forwarded into.
    struct Workspace {
        Session *session = nullptr;
        QString  name;
    };

    // `session` owns the message (its preview renders against it). With two or
    // more `workspaces`, a picker above the conversation selector chooses where
    // the message goes, starting on `session`; otherwise it stays in `session`.
    explicit ForwardDialog(
        const Message         &msg,
        Session               *session,
        std::vector<Workspace> workspaces = {},
        QWidget               *parent     = nullptr
    );

    Session       *targetSession() const { return _target; }
    // True if the dialog holds `session` (as target or picker choice): the host
    // must close it before that session is destroyed.
    bool           usesSession(const Session *session) const;
    ConversationId targetConv() const;
    QString        comment() const;

protected:
    void applyTheme() override;

private:
    void setTargetSession(Session *session);

    std::vector<Workspace> _workspaces;
    Session               *_target      = nullptr;
    Dropdown              *_wsPicker    = nullptr;
    ConvSelectorWidget    *_selector    = nullptr;
    ComposerWidget        *_composer    = nullptr;
    QFrame                *_previewCard = nullptr;
    StyledButton          *_copyLinkBtn = nullptr;
    StyledButton          *_cancelBtn   = nullptr;
    StyledButton          *_fwdBtn      = nullptr;
};
