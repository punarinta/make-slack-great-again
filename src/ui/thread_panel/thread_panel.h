// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QWidget>

class Session;
class MessageListWidget;
class ComposerWidget;
class ImageCache;
class QLabel;
class IconButton;

// Right-side panel showing a Slack thread: root message + replies + composer.
// Slides in when the user clicks a "N replies" bar in the main message list.
class ThreadPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThreadPanel(ImageCache *imgCache, QWidget *parent = nullptr);

    void setSession(Session *session);
    void openThread(ConversationId conv, Ts rootTs);
    void close();
    // Repaint the embedded message list, e.g. after the time-format setting changed.
    void refreshTimestamps();
    // Stop the embedded list's GIF decoding (host window minimized).
    void pauseGifPlayback();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

signals:
    void closeRequested();
    // Forwarded from the embedded message list's mention-hover profile card.
    void openDmRequested(UserId user);
    // Forwarded from the embedded message list (#channel mention click).
    void openChannelRequested(ConversationId conv);
    // Forwarded from the embedded message list (summarize no-provider notice).
    void aiSettingsRequested();

private:
    void applyTheme();
    // Keep the outward left-edge shadow positioned just left of the panel.
    void layoutShadow();

    Session       *_session = nullptr;
    ConversationId _conv;
    Ts             _rootTs;

    QWidget           *_headerWidget = nullptr;
    QWidget           *_leftShadow   = nullptr;
    QLabel            *_header       = nullptr;
    IconButton        *_closeBtn     = nullptr;
    MessageListWidget *_msgList      = nullptr;
    ComposerWidget    *_composer     = nullptr;

    rpl::lifetime _lifetime;
};
