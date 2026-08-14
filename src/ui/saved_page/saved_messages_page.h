// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QWidget>
#include <vector>

class ImageCache;
class Session;
class SavedCard;
class QLabel;
class QScrollArea;
class QVBoxLayout;

// "Saved messages" page: every message reminder ("Later" item with a due date),
// soonest due first, as cards with the conversation name, a chat-style message
// row and the due time. Data is the Session's local reminder mirror — no API
// call — so the page opens instantly and follows remindersChanged() live.
// Lives in MainWindow's content stack like ThreadsPage; the roster entry that
// opens it only shows while the list is non-empty.
class SavedMessagesPage : public QWidget {
    Q_OBJECT
public:
    explicit SavedMessagesPage(ImageCache *imgCache, QWidget *parent = nullptr);

    void setSession(Session *session);

    // (Re)build the cards. Call every time the page is brought to front.
    void open();

    // Drop cards (workspace switch / logout).
    void clear();

signals:
    // A message row click — jump to the message in its channel (and thread).
    void openMessageRequested(ConversationId conv, Ts ts, Ts threadRoot);
    // Conversation name on a card header.
    void openChannelRequested(ConversationId conv);

private:
    void applyTheme();
    void rebuild();
    void setStatus(const QString &text); // centered helper label; {} hides it

    Session    *_session  = nullptr;
    ImageCache *_imgCache = nullptr;

    QLabel      *_titleLabel  = nullptr;
    QWidget     *_headerRow   = nullptr;
    QScrollArea *_scroll      = nullptr;
    QWidget     *_listHost    = nullptr;
    QVBoxLayout *_listLayout  = nullptr; // cards, then stretch
    QLabel      *_statusLabel = nullptr;

    std::vector<SavedCard *> _cards;

    rpl::lifetime _remindersLifetime; // remindersChanged subscription (per session)
};
