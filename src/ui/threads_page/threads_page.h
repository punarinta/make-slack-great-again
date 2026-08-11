// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QWidget>
#include <vector>

class ImageCache;
class Session;
class StyledButton;
class ThreadCard;
class QLabel;
class QScrollArea;
class QVBoxLayout;

// Workspace-wide "Threads" overview page (the official client's Threads view):
// every thread the user is subscribed to, newest activity first, as cards with
// the channel name, the root message, the latest replies and an inline reply
// box. Lives in MainWindow's content stack like CanvasPage; data comes from
// Backend::loadThreadsView, so the whole page is gated on
// Capabilities::threadsView (the roster entry never shows otherwise).
class ThreadsPage : public QWidget {
    Q_OBJECT
public:
    explicit ThreadsPage(ImageCache *imgCache, QWidget *parent = nullptr);

    void setSession(Session *session);

    // (Re)load the first page. Call every time the page is brought to front —
    // the feed has no realtime push, so showing it is the refresh trigger.
    void open();

    // Drop cards and in-flight loads (workspace switch / logout).
    void clear();

signals:
    // "Show all replies" / a message row click — open the thread for real
    // (channel + thread panel).
    void openThreadRequested(ConversationId conv, Ts root);
    // Channel name on a card header.
    void openChannelRequested(ConversationId conv);

private:
    void applyTheme();
    void loadPage(const QString &cursor);
    void setStatus(const QString &text); // centered helper label; {} hides it

    Session    *_session  = nullptr;
    ImageCache *_imgCache = nullptr;

    QLabel       *_titleLabel  = nullptr;
    QWidget      *_headerRow   = nullptr;
    QScrollArea  *_scroll      = nullptr;
    QWidget      *_listHost    = nullptr;
    QVBoxLayout  *_listLayout  = nullptr; // cards, then "Show more", then stretch
    QLabel       *_statusLabel = nullptr;
    StyledButton *_moreBtn     = nullptr;

    std::vector<ThreadCard *> _cards;
    QString                   _nextCursor;
    bool                      _hasMore = false;
    bool                      _loading = false;

    rpl::lifetime _loadLifetime;   // active loadThreadsView subscription
    rpl::lifetime _eventsLifetime; // session events stream (reset per session)
};
