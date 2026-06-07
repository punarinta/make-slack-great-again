// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QWidget>

class Session;
class MessageListWidget;
class ComposerWidget;
class QLabel;

// Right-side panel showing a Slack thread: root message + replies + composer.
// Slides in when the user clicks a "N replies" bar in the main message list.
class ThreadPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThreadPanel(QWidget *parent = nullptr);

    void setSession(Session *session);
    void openThread(ConversationId conv, Ts rootTs);
    void close();

signals:
    void closeRequested();

private:
    Session       *_session = nullptr;
    ConversationId _conv;
    Ts             _rootTs;

    QLabel            *_header   = nullptr;
    MessageListWidget *_msgList  = nullptr;
    ComposerWidget    *_composer = nullptr;

    rpl::lifetime _lifetime;
};
