// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"
#include "ui/composer/composer_draft.h"

#include <QHash>
#include <QWidget>

class Session;
class MessageListWidget;
class ComposerWidget;
class ImageCache;
class QLabel;
class IconButton;
class PopupTooltip;

// Right-side panel showing a Slack thread: root message + replies + composer.
// Slides in when the user clicks a "N replies" bar in the main message list.
class ThreadPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThreadPanel(ImageCache *imgCache, QWidget *parent = nullptr);

    void setSession(Session *session);
    // Forget every stashed reply draft belonging to a workspace — called when
    // it logs out, so the same team re-added later starts clean.
    void purgeDrafts(const QString &teamId);
    void openThread(ConversationId conv, Ts rootTs);
    // Move the focus to one reply of the open thread (see MessageListWidget::jumpToTs).
    void jumpToTs(const Ts &ts);
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
    bool eventFilter(QObject *watched, QEvent *e) override;

signals:
    void closeRequested();
    // Forwarded from the embedded message list's mention-hover profile card.
    void openDmRequested(UserId user);
    // Forwarded from the embedded message list (#channel mention click).
    void openChannelRequested(ConversationId conv);
    // Forwarded from the embedded message list (summarize no-provider notice).
    void aiSettingsRequested();
    // Forwarded from the embedded message list (message-link chip click).
    void messageLinkRequested(ConversationId conv, Ts ts, Ts threadTs);
    // Forwarded from the embedded message list ("Forward message"). Carries the
    // conversation the reply lives in — the host can't assume it is the one on
    // screen, since a thread panel outlives a navigation away from its channel.
    void forwardMessageRequested(ConversationId conv, Message msg);

private:
    void applyTheme();
    // Empty the reply composer and file its unsent input under the open thread
    // (workspace + conversation + root ts). Runs on every way out of a thread —
    // opening another one, close(), a session switch — so a reply staged for
    // one thread can never be sent into another; input held while no thread is
    // open is discarded.
    void stashDraft();
    // Keep the outward left-edge shadow positioned just left of the panel.
    void layoutShadow();
    // Ask for a save path and hand off to a ThreadExportJob, which re-fetches
    // the whole thread from the API (the open view may only hold its head).
    void downloadThread();

    Session                      *_session = nullptr;
    ConversationId                _conv;
    Ts                            _rootTs;
    // "teamId\x1fconvId\x1frootTs" → stashed reply input, so switching threads
    // (or workspaces and back) resumes where the user left off. See stashDraft().
    QHash<QString, ComposerDraft> _drafts;

    QWidget           *_headerWidget = nullptr;
    QWidget           *_leftShadow   = nullptr;
    QLabel            *_header       = nullptr;
    IconButton        *_downloadBtn  = nullptr;
    IconButton        *_closeBtn     = nullptr;
    PopupTooltip      *_tooltip      = nullptr;
    MessageListWidget *_msgList      = nullptr;
    ComposerWidget    *_composer     = nullptr;

    rpl::lifetime _lifetime;
};
