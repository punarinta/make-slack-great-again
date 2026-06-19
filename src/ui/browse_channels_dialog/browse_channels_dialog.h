// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/app_dialog/app_dialog.h"

#include <algorithm>
#include <vector>

class BrowseListView;
class IconButton;
class ImageCache;
class QPushButton;
class QStackedWidget;
class StyledButton;
class StyledLineEdit;

// Modal "Find a channel" dialog (custom-chrome AppDialog: a tab bar instead of a
// title header). Shows all known channels with search and a People tab.
// "Create Channel" button emits createChannelRequested() — caller should close
// this dialog and open CreateChannelDialog in response.
class BrowseChannelsDialog : public AppDialog {
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
    void applyTheme() override;
    int  cardWidth(int availOverlayWidth) const override {
        return std::min(availOverlayWidth, kCardW);
    }
    int minCardHeight() const override { return kCardMinH; }

private:
    void buildChannelItems();
    void buildPeopleItems();
    void applyFilter(const QString &query);
    void selectTab(int tab);

    StyledLineEdit *_searchEdit  = nullptr;
    StyledButton   *_createBtn   = nullptr;
    IconButton     *_closeBtn    = nullptr;
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
