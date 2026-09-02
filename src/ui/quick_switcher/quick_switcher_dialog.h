// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/app_dialog/app_dialog.h"
#include "ui/conv_list/named_conversation.h"

#include <algorithm>
#include <vector>

class BrowseListView;
class ImageCache;
class QLabel;
class StyledLineEdit;

// Ctrl/Cmd+K quick switcher: type a few letters, hit Enter, land in the
// conversation (issue #52). Lists only conversations of the ACTIVE workspace
// that the user is already in — channels, DMs and group DMs — most recently
// active first, so an empty query is a "recent chats" list.
//
// Deliberately not a second channel browser: BrowseChannelsDialog is for finding
// something you are *not* in (it can join, it has a People tab, it hits the
// network). This one is pure navigation over what the sidebar already holds, and
// takes its names straight from ConvListWidget so the two can't disagree.
class QuickSwitcherDialog : public AppDialog {
    Q_OBJECT
public:
    QuickSwitcherDialog(
        std::vector<NamedConversation> conversations,
        ImageCache                    *imgCache,
        QWidget                       *parent = nullptr
    );

signals:
    void conversationActivated(ConversationId id);

protected:
    void applyTheme() override;
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    int  cardWidth(int availOverlayWidth) const override {
        return std::min(availOverlayWidth, kCardW);
    }
    int minCardHeight() const override { return kCardMinH; }

private:
    void buildItems();
    void applyFilter(const QString &query);

    StyledLineEdit *_searchEdit = nullptr;
    BrowseListView *_list       = nullptr;
    QLabel         *_hint       = nullptr;
    QLabel         *_empty      = nullptr;

    std::vector<NamedConversation> _conversations;
    ImageCache                    *_imgCache = nullptr;

    static constexpr int kCardW    = 560;
    static constexpr int kCardMinH = 420;
    static constexpr int kListMinH = 300;
};
