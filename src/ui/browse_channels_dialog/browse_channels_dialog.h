// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QDialog>
#include <vector>

class BrowseListView;
class ImageCache;
class QFrame;
class QPushButton;
class QStackedWidget;
class StyledButton;
class StyledLineEdit;

// Modal "Find a channel" dialog.
// Shows all known channels with search and a People tab.
// "Create Channel" button emits createChannelRequested() — caller should close
// this dialog and open CreateChannelDialog in response.
class BrowseChannelsDialog : public QDialog {
    Q_OBJECT
public:
    explicit BrowseChannelsDialog(
        const std::vector<Conversation> &conversations,
        const std::vector<User>         &users,
        ImageCache                      *imgCache,
        QWidget                         *parent = nullptr
    );

    // Switch to the People tab (e.g. when opened from the DM section "+").
    void showPeopleTab() { selectTab(1); }

signals:
    void createChannelRequested();
    void channelActivated(ConversationId id);
    void userActivated(UserId id);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    void buildChannelItems();
    void buildPeopleItems();
    void applyFilter(const QString &query);
    void selectTab(int tab);
    void updateCard();
    void applyTheme();

    QFrame         *_card        = nullptr;
    StyledLineEdit *_searchEdit  = nullptr;
    StyledButton   *_createBtn   = nullptr;
    QPushButton    *_closeBtn    = nullptr;
    QPushButton    *_channelsTab = nullptr;
    QPushButton    *_peopleTab   = nullptr;
    QStackedWidget *_stack       = nullptr;
    BrowseListView *_channelList = nullptr;
    BrowseListView *_peopleList  = nullptr;
    int             _activeTab   = 0;

    std::vector<Conversation> _conversations;
    std::vector<User>         _users;
    ImageCache               *_imgCache = nullptr;

    static constexpr int kCardW    = 720;
    static constexpr int kCardMinH = 520;
    static constexpr int kListMinH = 400;
};
