// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/virtual_list/virtual_list_widget.h"

#include <QHash>
#include <QPixmap>
#include <QVariantAnimation>
#include <vector>

class ImageCache;
class PopupTooltip;

// Per-user info cached from setUsers().
struct UserInfo {
    QString displayName;
    QString avatarUrl;
    QString name; // Slack username (used for MPDM name parsing)
    bool    isDeactivated = false;
    bool    isActive      = false;
    bool    dndEnabled    = false;
    bool    isBot         = false; // bot/app user (incl. Slackbot)
    QString statusEmoji;           // resolved emoji name without colons, e.g. "palm_tree"
};

// Visual row kinds in the conversation list.
enum class RowKind { SectionHeader, Conv, AddChannels, ShowMore };

// Maps a visual row index to its content.
struct RowItem {
    RowKind kind;
    int     convIdx   = -1; // index into _convs, valid when kind == Conv
    int     sectionId = -1; // 0 = Channels, 1 = Direct messages, 2 = Agents & apps;
                            // valid for SectionHeader/AddChannels/ShowMore
    int     count     = 0;  // for ShowMore: number of hidden items
};

// Virtual-painted conversation list with section grouping and collapse/expand.
// Zero QWidgets per row — scales to thousands of conversations.
class ConvListWidget : public VirtualListWidget {
    Q_OBJECT
public:
    explicit ConvListWidget(ImageCache *imgCache, QWidget *parent = nullptr);

    void setConversations(std::vector<Conversation> convs);
    // Call with the full user list so DM names, avatars, and status can be resolved.
    void setUsers(const std::vector<User> &users);
    // Set the current user's ID so the "you" label can be shown on self DMs.
    void setMe(UserId id) {
        _meUserId = std::move(id);
        viewport()->update();
    }
    // Self-only: the user appears away to others merely because no official
    // Slack client is connected (see Session::selfPresence()).
    void setSelfPhantomAway(bool phantom) {
        if (_selfPhantomAway == phantom)
            return;
        _selfPhantomAway = phantom;
        viewport()->update();
    }
    // Resolved display name for a visual row (DMs → user displayName, channels → conv.name).
    QString        resolvedName(int row) const;
    int            selectedIndex() const { return _selected; }
    // Resolved ConversationId for a visual row (-1 safe: returns empty id).
    ConversationId conversationId(int row) const;
    // Visual row for a given id; -1 if not found or section is collapsed.
    int            rowForId(ConversationId id) const;
    // Programmatically select a row; emits conversationSelected.
    void           selectRow(int row);
    // Select a conversation even if it is currently hidden by the relevance
    // filter: stamps it visited (making it relevant), expands its section,
    // then selects its row. Returns false if the id is not in the list at all.
    bool           selectConversation(ConversationId id);

    // Set how many days of activity qualify a conversation as "relevant" (shown inline).
    // Conversations outside this window appear under "N more..." until expanded.
    void setRelevantDays(int days);
    // Wipe the in-memory visit history and rebuild rows so auto-seed runs from scratch.
    // Call after the user clears state in Settings.
    void resetVisitedAt();

signals:
    void conversationSelected(int row);
    void findChannelRequested();
    // "+" on the Direct messages header — open the browse dialog on People.
    void browsePeopleRequested();
    void createChannelRequested();
    void starConversationRequested(ConversationId id, bool star);
    void setNotificationLevelRequested(ConversationId id, NotificationLevel level);
    void leaveConversationRequested(ConversationId id);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    void doPaint(QPaintEvent *e) override;
    void doMouseMove(QMouseEvent *e) override;
    void doMousePress(QMouseEvent *e) override;
    void doMouseRelease(QMouseEvent *e) override;
    void doMouseLeave() override;

    int   rowAt(int viewportY) const; // -1 if none
    void  setHovered(int row);
    void  setSelected(int row); // emits conversationSelected (no-op for non-Conv rows)
    void  showChannelContextMenu(int row, QPoint globalPos);
    void  showMpdmContextMenu(int row, QPoint globalPos);
    void  paintRow(QPainter &p, int row, int y) const;
    void  paintSectionHeader(QPainter &p, int row, int y, int sectionId) const;
    void  paintAddChannelsRow(QPainter &p, int row, int y) const;
    void  paintShowMoreRow(QPainter &p, int row, int y, int count) const;
    // Hit/paint rect of the "+" button on the Direct messages section header.
    QRect dmPlusRect(int rowY) const;
    void  updateScrollRange();
    // True for 1:1 IMs whose counterpart is a bot/app (incl. Slackbot) —
    // these are grouped under "Agents & apps" instead of "Direct messages".
    bool  isAppConv(const Conversation &c) const;
    // Rebuild _convs from _allConvs, filtering deactivated / raw-ID DM users.
    void  rebuildFilteredConvs();
    // Rebuild _rows from _convs according to current section collapse state.
    void  rebuildRows();

    // Icon pixmaps colorized with nav-side theme tokens. Rebuilt on
    // themeChanged — a static-local cache would keep the old theme's tint.
    struct IconPixmaps {
        QPixmap chevDown, chevRight, hash, msg, bot, plusDim; // section headers, onDarkDim
        QPixmap plusBright;                                   // add-channels hover, onDark
        QPixmap lockDim, lockBright, lockSelected;            // private channel prefix
        QPixmap hashSmDim, hashSmBright, hashSmSelected;      // public channel prefix
    };
    void rebuildIconPixmaps();

    // Avatar helpers — trigger is non-const (starts downloads), draw is const.
    void triggerMissingAvatarDownloads();
    void drawUserAvatar(
        QPainter &p, QRect rect, const QString &userId, QColor bgColor, bool isSelected = false
    ) const;

    std::vector<Conversation> _allConvs; // unfiltered; source of truth
    std::vector<Conversation> _convs;    // filtered convs
    std::vector<RowItem>      _rows;     // visual row list (includes headers/actions)
    // userId → {displayName, avatarUrl, ...}, rebuilt on setUsers().
    QHash<QString, UserInfo>  _userInfos;
    // Slack username (user.name) → userId.value, for MPDM name parsing.
    QHash<QString, QString>   _usernameToId;
    // convId.value → Unix epoch sec of last time the user opened that conversation in this app.
    // Persisted to QSettings so recency survives restarts.
    QHash<QString, qint64>    _visitedAt;

    void          loadVisitedAt();
    void          saveVisitedAt();
    IconPixmaps   _iconPx;
    ImageCache   *_imgCache = nullptr;
    PopupTooltip *_tooltip  = nullptr; // hover tooltip for the DM header "+"
    UserId        _meUserId;
    bool          _selfPhantomAway = false;

    bool _channelsCollapsed = false;
    bool _dmsCollapsed      = false;
    bool _appsCollapsed     = false;
    bool _showAllChannels   = false; // true after user clicks "N more channels"

    int            _hovered  = -1;
    int            _selected = -1;
    ConversationId _selectedId; // survives rebuildRows() calls

    // Selection slide animation: 0.0 = start of slide, 1.0 = settled
    QVariantAnimation _selAnim;
    int               _selFrom = -1;
    double            _selT    = 1.0;

    static constexpr int kRowH         = 30; // height of every row (uniform)
    static constexpr int kPadH         = 12; // horizontal left padding
    static constexpr int kPadV         = 8;  // vertical padding inside row
    static constexpr int kAvatarSize   = 20; // size of user avatar square
    static constexpr int kAvatarRadius = 5;  // corner radius
    static constexpr int kAvatarGap    = 8;  // gap between avatar and name
    static constexpr int kIconSize     = 14; // section / prefix icon size
    static constexpr int kGroupIndent =
        kIconSize + 6; // child-row indent (aligns with section label)
    int _relevantDays =
        14; // configurable via setRelevantDays(); default matches kDefaultRelevantDays
    static constexpr int kDefaultRelevantDays = 14;
};
