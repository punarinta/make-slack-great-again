// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/virtual_list/virtual_list_widget.h"

#include <QHash>
#include <QPixmap>
#include <QStaticText>
#include <QTimer>
#include <QVariantAnimation>
#include <vector>

class ImageCache;
class PopupTooltip;
class Session;

// Per-user info cached from setUsers().
struct UserInfo {
    QString displayName;
    QString avatarUrl;
    QString name; // Slack username (used for MPDM name parsing)
    bool    isDeactivated = false;
    bool    isActive      = false;
    bool    dndEnabled    = false;
    bool    isBot         = false; // bot/app user (incl. Slackbot)
    bool    isExternal    = false; // Slack Connect external member ("EXT" tag)
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
    ~ConvListWidget() override;

    // The active session — used only to ask the backend opaque-id questions
    // (synthetic/system accounts, unresolved raw ids). Re-set on workspace switch.
    void setSession(Session *s) { _session = s; }

    void setConversations(std::vector<Conversation> convs);
    // Call with the full user list so DM names, avatars, and status can be resolved.
    void setUsers(const std::vector<User> &users);
    // Targeted presence/DND patch: updates one _userInfos entry and repaints
    // only that user's row(s). Presence events arrive in bursts for the whole
    // roster (reconnect, morning login) — routing them through setUsers()
    // would rebuild the entire list once per event.
    void setUserPresence(const UserId &id, bool active);
    void setUserDnd(const UserId &id, bool dnd);
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
    // Show/hide the whole "Agents & apps" section (Settings → Appearance).
    // Hidden app DMs stay reachable through search / browse.
    void setShowAgentsApps(bool show);
    // Wipe the in-memory visit history and rebuild rows so auto-seed runs from scratch.
    // Call after the user clears state in Settings.
    void resetVisitedAt();
    // Global default notification level applied to conversations whose own level
    // is NotificationLevel::Default. Drives which unread badges (and colors) show.
    void setDefaultNotifyLevel(NotificationLevel level);

signals:
    void conversationSelected(int row);
    void findChannelRequested();
    // "+" on the Direct messages header — open the browse dialog on People.
    void browsePeopleRequested();
    void createChannelRequested();
    void starConversationRequested(ConversationId id, bool star);
    // Click on a row's live-huddle indicator — open the huddle's web join link.
    void joinHuddleRequested(ConversationId id);
    void setNotificationLevelRequested(ConversationId id, NotificationLevel level);
    void muteConversationRequested(ConversationId id, bool muted);
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
    void  showDmContextMenu(int row, QPoint globalPos);
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
    // Schedule a repaint of every visible row whose avatar belongs to `userId`
    // (DM/MPDM rows use conv.dmUser). Cheap row scan, no rebuild.
    void  updateRowsForUser(const QString &userId);
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
        QPixmap huddle;                                       // live-huddle pill icon, onAccent
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
    // The exact user list _userInfos was last built from. setUsers() is invoked
    // on every workspace switch and on every _users re-emission (profile
    // changes, network refresh); skipping an unchanged list avoids rebuilding
    // the whole hash (with a per-user Emoji::expandCodes) + a redundant
    // rebuildRows. Kept presence-truthful by setUserPresence/setUserDnd.
    // QString is implicitly shared, so the copy is cheap.
    std::vector<User>         _lastUsers;
    // Slack username (user.name) → userId.value, for MPDM name parsing.
    QHash<QString, QString>   _usernameToId;
    // convId.value → Unix epoch sec of last time the user opened that conversation in this app.
    // Persisted to QSettings so recency survives restarts.
    QHash<QString, qint64>    _visitedAt;

    void          loadVisitedAt();
    void          saveVisitedAt();         // synchronous QSettings write
    void          scheduleSaveVisitedAt(); // debounced; serializes off the hot path
    QTimer        _saveVisitedTimer;
    IconPixmaps   _iconPx;
    qreal         _iconDpr  = 0; // DPR _iconPx was last rasterised at (see doPaint)
    ImageCache   *_imgCache = nullptr;
    PopupTooltip *_tooltip  = nullptr; // hover tooltip for the DM header "+"
    Session      *_session  = nullptr; // non-owning; for opaque-id queries only
    UserId        _meUserId;
    bool          _selfPhantomAway = false;

    bool _channelsCollapsed = false;
    bool _dmsCollapsed      = false;
    bool _appsCollapsed     = false;
    bool _showAgentsApps    = true;  // Settings toggle; see setShowAgentsApps()
    bool _showAllChannels   = false; // true after user clicks "N more channels"

    // convId.value → viewport rect of the clickable huddle indicator, refreshed
    // each paint (so it tracks scroll); consulted on click to join the huddle.
    mutable QHash<QString, QRect> _huddleHitRects;

    // Visual row → viewport rect of its name text, recorded each paint only when
    // the name had to be elided. doMouseMove consults it to show a full-name
    // tooltip over truncated chat names. Cleared and repopulated per paint like
    // _huddleHitRects, so it always reflects the current scroll offset.
    mutable QHash<int, QRect> _truncNameRects;
    // Which row's name tooltip is currently showing (-1 none, -2 the DM "+"
    // button tooltip). Guards against re-issuing showAbove on every mouse move.
    int                       _tooltipRow = -1;

    // Per-conversation cache of the elided + shaped row name. elidedText /
    // drawText / horizontalAdvance each re-shape the string, so an uncached
    // name costs three shaping passes per row per frame.
    struct NameCache {
        QString     full; // source name the entry was built from
        int         maxW   = -1;
        int         weight = -1; // font weight (unread rows go DemiBold)
        QString     elided;
        int         elidedW = 0;
        QStaticText st;
    };
    mutable QHash<QString, NameCache> _nameCache; // key: conv id
    const NameCache &
    cachedName(const QString &convId, const QString &full, int maxW, const QFont &font) const;

    int            _hovered  = -1;
    int            _selected = -1;
    ConversationId _selectedId; // survives rebuildRows() calls
    // While the "Agents & apps" section is hidden, the app DM that is actually
    // open still gets a row. Without it selectConversation() would find none
    // and return false, and MainWindow opens notifications and search results
    // through `rowForId(conv) >= 0` — so clicking either would silently do
    // nothing. Transient (never persisted); cleared once the selection moves.
    ConversationId _revealedAppConv;

    // Selection slide animation: 0.0 = start of slide, 1.0 = settled
    QVariantAnimation _selAnim;
    int               _selFrom = -1;
    double            _selT    = 1.0;

    // Height of every row (uniform). kRowHBase scaled by the font-size setting
    // (ThemeManager::fontFactor) — recomputed in updateRowHeight() on theme
    // change so rows breathe with the text instead of cramping it.
    static constexpr int kRowHBase = 30;
    int                  _rowH     = 30;
    void                 updateRowHeight();
    static constexpr int kPadH         = 12; // horizontal left padding
    static constexpr int kPadV         = 8;  // vertical padding inside row
    static constexpr int kAvatarSize   = 20; // size of user avatar square
    static constexpr int kAvatarRadius = 5;  // corner radius
    static constexpr int kAvatarGap    = 8;  // gap between avatar and name
    static constexpr int kIconSize     = 14; // section / prefix icon size
    static constexpr int kHuddleIcon   = 13; // headphones glyph in the huddle pill
    static constexpr int kHuddlePad    = 6;  // horizontal padding inside the huddle pill
    static constexpr int kHuddleGap    = 6;  // gap between huddle avatar and pill
    static constexpr int kGroupIndent =
        kIconSize + 6; // child-row indent (aligns with section label)
    int _relevantDays =
        14; // configurable via setRelevantDays(); default matches kDefaultRelevantDays
    static constexpr int kDefaultRelevantDays = 14;
    // Global default for conversations with NotificationLevel::Default. Mirrors
    // the Settings "Notify me about" radio (default: All new posts).
    NotificationLevel    _defaultNotify       = NotificationLevel::All;
};
