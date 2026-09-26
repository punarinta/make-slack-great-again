// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/overview_card/overview_card.h"
#include "rpl/lifetime.h"

#include <QWidget>
#include <vector>

class ImageCache;
class Session;
class SavedCard;

// "Saved messages" page: every saved message (Slack's "Later": reminders
// soonest due first, then plain "Save for later" bookmarks newest first) as
// cards with the conversation name, a chat-style message row and the due time
// or "Saved for later". Data is the Session's local saved-item mirror — no API
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

    OverviewCard::Page _page; // list: status, cards, then stretch

    std::vector<SavedCard *> _cards;

    rpl::lifetime _remindersLifetime; // remindersChanged subscription (per session)
};
