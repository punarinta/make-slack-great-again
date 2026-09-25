// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"
#include "ui/conv_list/named_conversation.h"
#include "ui/message_list/message_render.h"
#include "ui/virtual_list/virtual_list_widget.h"

#include <QHash>
#include <QPixmap>
#include <QStaticText>
#include <QTimer>
#include <QVariantAnimation>
#include <QVector>
#include <vector>

class ImageCache;
class PopupTooltip;
class QMovie;
class Session;

// Per-user info cached from setUsers().
struct UserInfo {
    QString displayName;
    QString avatarUrl;
    QString name; // Slack username (used for MPDM name parsing)
    bool    isDeactivated = false;
    bool    isActive      = false;
    bool    dndEnabled    = false;
    bool    unavailable   = false; // User::unavailable — the yellow dot
    bool    isBot         = false; // bot/app user (incl. Slackbot)
    bool    isExternal    = false; // Slack Connect external member ("EXT" tag)
    QString statusEmoji;           // resolved emoji name without colons, e.g. "palm_tree"
};

// Visual row kinds in the conversation list.
enum class RowKind { Threads, SavedMsgs, SectionHeader, Conv, AddChannels, ShowMore, Teammate };

// Maps a visual row index to its content.
struct RowItem {
    RowKind kind;
    int     convIdx   = -1; // index into _convs when kind == Conv, into _teammates for Teammate
    int     sectionId = -1; // 0 = Channels, 1 = Direct messages, 2 = Agents & apps,
                            // 3 = Starred, 4 = Team; valid for SectionHeader/AddChannels/ShowMore
    int     count     = 0;  // for ShowMore: number of hidden items
};

#if defined(MSGA_DEMO)
namespace demo {
class Tour;
}
#endif
// Virtual-painted conversation list with section grouping and collapse/expand.
// Zero QWidgets per row — scales to thousands of conversations.
class ConvListWidget : public VirtualListWidget {
    Q_OBJECT
#if defined(MSGA_DEMO)
    friend class demo::Tour; // the scripted demo drives real widgets (--demo-tour)
#endif
public:
    explicit ConvListWidget(ImageCache *imgCache, QWidget *parent = nullptr);
    ~ConvListWidget() override;

    // The active session — used only to ask the backend opaque-id questions
    // (synthetic/system accounts, unresolved raw ids). Re-set on workspace switch.
    // Also the custom-emoji resolver for status emoji: a status set to a
    // workspace emoji (":finland:") is an image from emoji.list, not a glyph.
    void setSession(Session *s);
    // Settings → Appearance → Visual effects: off releases every status-emoji
    // player, paints stills, and has the cache drop their animation bytes.
    void setEmojiAnimationsEnabled(bool on);

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
    QString                        resolvedName(int row) const;
    // Name resolution for one conversation, independent of any visual row (a
    // group DM with its localName cleared yields its member-list title).
    QString                        resolvedConvName(const Conversation &c) const;
    // Every conversation held, name-resolved and ordered most-recent first.
    std::vector<NamedConversation> namedConversations() const;
    // conv id → epoch seconds of the last time it was opened in this app. App-
    // wide (ids are unique across workspaces), so a background workspace's
    // conversations can be ranked from it too (namedConversationsFor()).
    const QHash<QString, qint64>  &visitedAt() const { return _visitedAt; }
    int                            selectedIndex() const { return _selected; }
    // Claude Code workspace: when the selected row is an idle (gray-dot)
    // session, emits leaveConversationRequested for it — the context menu's
    // "Remove from msga" — and returns true; otherwise does nothing.
    bool                           removeSelectedIdleSession();
    // Number of visual rows currently laid out (conversations plus the section
    // headers and action rows between them).
    bool                           sectionHasUnread(int sectionId) const;
    int                            rowCount() const { return int(_rows.size()); }
    // Resolved ConversationId for a visual row (-1 safe: returns empty id).
    ConversationId                 conversationId(int row) const;
    // The topmost conversation row's id, skipping `except`; empty when none.
    ConversationId                 firstConversationId(const ConversationId &except = {}) const;
    // Visual row for a given id; -1 if not found or section is collapsed.
    int                            rowForId(ConversationId id) const;
    // Viewport rectangle of a row (empty for an invalid row). Rows are virtual, so
    // this is the only way to point at one from outside (tests, the demo tour).
    QRect                          rowViewportRect(int row) const;
    // Programmatically select a row; emits conversationSelected.
    void                           selectRow(int row);
    // Select a conversation even if it is currently hidden by the relevance
    // filter: stamps it visited (making it relevant), expands its section,
    // then selects its row. Returns false if the id is not in the list at all.
    bool                           selectConversation(ConversationId id);

    // Set how many days of activity qualify a conversation as "relevant" (shown inline).
    // Conversations outside this window appear under "N more..." until expanded.
    void setRelevantDays(int days);
    // Show/hide the whole "Agents & apps" section (Settings → Appearance).
    // Hidden app DMs stay reachable through search / browse.
    void setShowAgentsApps(bool show);
    // "Show only unread conversations" (Settings → Appearance): list nothing but
    // conversations that paint as unread (unread > 0 and not muted), plus the
    // one that is open — it stays put until the selection moves on, like the
    // official client's "Unreads only" sidebar filter. Starred conversations are
    // exempt (a star is an explicit "keep in front of me"). Hidden channels
    // remain reachable through the "N more channels" expander, hidden DMs
    // through search / browse.
    void setUnreadsOnly(bool on);
    // Show the fixed "Threads" entry above the Channels section — gated on
    // Capabilities::threadsView, so it only appears for backends with a
    // workspace-wide threads feed.
    void setShowThreads(bool show);
    // Agent workspace (Capabilities::agentSessions, e.g. Claude Code): the DMs
    // are sessions — every one is listed (no relevance filter: finished sessions
    // stay reachable for as long as the service keeps them), the section reads
    // "Sessions", and an empty Channels section is left out.
    void setAgentSessions(bool on);
    // Agent workspace: the team (Backend::agentRoles), listed in a "Team"
    // section under the sessions. A teammate's row opens its page
    // (teammateSelected); its dot is its user's presence — working while any
    // of its sessions is. Empty hides the section.
    void setTeammates(std::vector<AgentRole> teammates);
    // Highlight a teammate's row as the open page (no signal); "" clears it.
    void setSelectedTeammate(const QString &roleId);
    // Number of followed threads holding unread replies (Session::
    // unreadThreadCount). Non-zero highlights the "Threads" entry — bright label
    // and icon, like an unread conversation row — so it stands out under the
    // unreads-only filter too (issue #59). No count badge: this is a group
    // title, badges belong on the threads themselves.
    void setUnreadThreadCount(int count);
    int  unreadThreadCount() const { return _unreadThreads; }
    // Show the fixed "Saved messages" entry (under Threads) — gated on
    // Capabilities::messageReminders AND the saved list being non-empty, so it
    // only appears while there is something to show.
    void setShowSavedMessages(bool show);
    // Wipe the in-memory visit history and rebuild rows so auto-seed runs from scratch.
    // Call after the user clears state in Settings.
    void resetVisitedAt();
    // Global default notification level applied to conversations whose own level
    // is NotificationLevel::Default. Drives which unread badges (and colors) show.
    void setDefaultNotifyLevel(NotificationLevel level);
    // Settings → Notifications "Highlight mentions-only channels for any new
    // message" (default on). On: a "Just mentions" channel paints bold for any
    // unread, even though only @mentions badge it. Off: it paints bold only
    // while it holds an @mention — i.e. exactly when it shows a badge — so a
    // channel the user opted out of stays quiet in every way. DMs and "All new
    // posts" channels are unaffected (their unreads always badge).
    void setHighlightMentionsOnlyUnreads(bool on);

signals:
    void conversationSelected(int row);
    // Click on the fixed "Threads" entry — open the threads overview page.
    void threadsViewRequested();
    // Click on the fixed "Saved messages" entry — open the saved messages page.
    void savedMessagesRequested();
    // Click on a teammate in the Team section — open its page.
    void teammateSelected(const QString &roleId);
    // The Team header's "+", and a teammate row's menu.
    void addTeammateRequested();
    void editTeammateRequested(const QString &roleId);
    void restoreTeammateRequested(const QString &roleId);
    void removeTeammateRequested(const QString &roleId);
    void findChannelRequested();
    // "+" on the Direct messages header — open the browse dialog on People.
    void browsePeopleRequested();
    void createChannelRequested();
    // Right-click on the Sessions "+" (agent workspace): start-session options.
    void agentSessionMenuRequested(QPoint globalPos);
    void starConversationRequested(ConversationId id, bool star);
    // Click on a row's live-huddle indicator — open the huddle's web join link.
    void joinHuddleRequested(ConversationId id);
    void setNotificationLevelRequested(ConversationId id, NotificationLevel level);
    void muteConversationRequested(ConversationId id, bool muted);
    void leaveConversationRequested(ConversationId id);
    // "Name conversation…" on a group DM — the host opens the naming dialog and
    // stores the result via Session::setConvLocalName.
    void renameConversationRequested(ConversationId id);
    // An agent session's "Stop" (Backend::stopAgentSession).
    void stopSessionRequested(ConversationId id);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    void doPaint(QPaintEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void doMouseMove(QMouseEvent *e) override;
    void doMousePress(QMouseEvent *e) override;
    void doMouseRelease(QMouseEvent *e) override;
    void doMouseLeave() override;

    int  rowAt(int viewportY) const; // -1 if none
    // Row geometry in document coords (kTopPad included) and viewport coords
    // (scroll offset applied). Everything that maps between rows and y must go
    // through these — a hand-rolled `row * _rowH` silently drops the top inset
    // and skews hit-testing against what was painted.
    int  contentHeight() const;
    int  rowTopDoc(int row) const;
    int  rowTopView(int row) const;
    int  firstVisibleRow() const;
    int  lastVisibleRow() const; // -1 when there are no rows
    void setHovered(int row);
    void setSelected(int row);        // emits conversationSelected (no-op for non-Conv rows)
    void selectThreadsRow(int row);   // Threads-row counterpart; emits threadsViewRequested
    void selectSavedMsgsRow(int row); // "Saved messages" counterpart; emits savedMessagesRequested
    void selectTeammateRow(int row);  // Team counterpart; emits teammateSelected
    void paintTeammateRow(QPainter &p, int row, int y) const;
    void showChannelContextMenu(int row, QPoint globalPos);
    void showMpdmContextMenu(int row, QPoint globalPos);
    void showDmContextMenu(int row, QPoint globalPos);
    void showTeammateContextMenu(int row, QPoint globalPos);
    void paintRow(QPainter &p, int row, int y) const;
    void paintSectionHeader(QPainter &p, int row, int y, int sectionId) const;
    void paintThreadsRow(QPainter &p, int row, int y) const;
    void paintSavedMsgsRow(QPainter &p, int row, int y) const;
    // Shared face of the fixed nav entries (Threads, Saved messages): pill
    // highlight + icon centred in the kIconSize slot + section-header label.
    // `unread` brightens the label and icon like an unread conversation row.
    void paintNavEntryRow(
        QPainter &p, int row, int y, const QPixmap &icon, const QString &label, bool unread = false
    ) const;
    void   paintAddChannelsRow(QPainter &p, int row, int y) const; // also sessions
    void   paintShowMoreRow(QPainter &p, int row, int y, int count) const;
    // Hit/paint rect of the "+" button on the Direct messages section header.
    QRect  dmPlusRect(int rowY) const;
    void   updateScrollRange();
    // True for 1:1 IMs whose counterpart is a bot/app (incl. Slackbot) —
    // these are grouped under "Agents & apps" instead of "Direct messages".
    bool   isAppConv(const Conversation &c) const;
    // Ordering signal: the later of the visit stamp and the conv's own activity.
    qint64 activitySeconds(const Conversation &c) const;
    // Schedule a repaint of every visible row whose avatar belongs to `userId`
    // (DM/MPDM rows use conv.dmUser). Cheap row scan, no rebuild.
    void   updateRowsForUser(const QString &userId);
    // Rebuild _convs from _allConvs, filtering deactivated / raw-ID DM users.
    void   rebuildFilteredConvs();
    // Rebuild _rows from _convs according to current section collapse state.
    void   rebuildRows();

    // Icon pixmaps colorized with nav-side theme tokens. Rebuilt on
    // themeChanged — a static-local cache would keep the old theme's tint.
    struct IconPixmaps {
        QPixmap chevDown, chevRight, hash, msg, bot, plusDim; // section headers, nav.itemTextDim
        QPixmap star;                                         // Starred section header
        QPixmap team;                                         // Team section header
        QPixmap plusBright;                                   // add-channels hover, nav.itemText
        QPixmap lockDim, lockBright, lockSelected;            // private channel prefix
        QPixmap hashSmDim, hashSmBright, hashSmSelected;      // public channel prefix
        QPixmap huddle;                                       // live-huddle pill icon, onAccent
        QPixmap threadsDim, threadsBright, threadsSelected;   // "Threads" entry (split icon)
        QPixmap savedDim, savedBright, savedSelected;         // "Saved messages" (bookmark)
    };
    void rebuildIconPixmaps();

    // Avatar helpers — trigger is non-const (starts downloads), draw is const.
    void                     triggerMissingAvatarDownloads();
    // Status emoji of a DM peer resolved against the workspace's custom-emoji
    // map: `unicode` for built-ins, `imageUrl` for custom ones, resolved=false
    // when the user has none or the name is unknown.
    MsgRender::EmojiResolved statusEmojiOf(const UserInfo &info) const;
    void                     drawUserAvatar(
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
    rpl::lifetime _sessionLifetime;    // emojiMapLoaded() subscription on _session
    UserId        _meUserId;
    bool          _selfPhantomAway = false;

    bool                   _animateEmoji      = true; // see setEmojiAnimationsEnabled()
    bool                   _starredCollapsed  = false;
    bool                   _channelsCollapsed = false;
    bool                   _dmsCollapsed      = false;
    bool                   _appsCollapsed     = false;
    bool                   _teamCollapsed     = false;
    bool                   _showAgentsApps    = true;  // Settings toggle; see setShowAgentsApps()
    bool                   _unreadsOnly       = false; // Settings toggle; see setUnreadsOnly()
    bool                   _showThreads       = false; // capability gate; see setShowThreads()
    bool                   _agentSessions     = false; // see setAgentSessions()
    int                    _unreadThreads     = 0;     // see setUnreadThreadCount()
    bool                   _showSavedMsgs     = false; // gate; see setShowSavedMessages()
    bool                   _showAllChannels   = false; // true after user clicks "N more channels"
    // The "Threads" entry is selected (the overview page is open). Mutually
    // exclusive with _selectedId; survives rebuildRows() like it.
    bool                   _threadsSelected   = false;
    // Same for the "Saved messages" entry; mutually exclusive with both.
    bool                   _savedMsgsSelected = false;
    // The teammate whose page is open ("" = none); exclusive with the above.
    QString                _selectedTeammate;
    std::vector<AgentRole> _teammates; // see setTeammates()

    // convId.value → viewport rect of the clickable huddle indicator, refreshed
    // each paint (so it tracks scroll); consulted on click to join the huddle.
    mutable QHash<QString, QRect> _huddleHitRects;

    // Animated custom status emoji (Slack serves many as GIFs). Players are
    // acquired from the shared ImageCache (one acquire per url, released in
    // the destructor) and only run while a row showing them is on screen:
    // _statusEmojiRects is cleared and repopulated each paint with the row
    // rects painted for each url; syncStatusEmojiPlayback() then starts the
    // players with a rect and pauses the rest. frameChanged repaints only
    // those rects.
    QMovie                                *statusEmojiMovie(const QString &url) const;
    void                                   syncStatusEmojiPlayback() const;
    void                                   releaseStatusEmojiMovies();
    mutable QHash<QString, QMovie *>       _statusEmojiMovies;
    mutable QHash<QString, QVector<QRect>> _statusEmojiRects;

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
    // Unreads-only counterpart, but only for the window inside
    // selectConversation() between the row rebuild and the actual selection: a
    // read conversation opened from a notification / search result is not
    // unread and not yet _selectedId, so without this the rebuild would yield
    // no row for it and the open would silently fail. Cleared as soon as the
    // selection lands (from then on _selectedId keeps the row).
    ConversationId _unreadsOnlyReveal;

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
    static constexpr int kTopPad       = 6;  // breathing room above the first row
    static constexpr int kAvatarSize   = 20; // size of user avatar square
    static constexpr int kAvatarRadius = 5;  // corner radius
    static constexpr int kAvatarGap    = 8;  // gap between avatar and name
    static constexpr int kIconSize     = 14; // section / prefix icon size
    // Lucide's split glyph paints wider inside its 24-unit viewBox than the
    // section icons (hash, messages-square) do, so an identical box still reads
    // a size bigger next to them. Bake it one notch down and centre it in the
    // kIconSize slot — the label keeps its x, the two icons look equal.
    static constexpr int kThreadsIcon  = 13;
    static constexpr int kHuddleIcon   = 13; // headphones glyph in the huddle pill
    static constexpr int kHuddlePad    = 6;  // horizontal padding inside the huddle pill
    static constexpr int kHuddleGap    = 6;  // gap between huddle avatar and pill
    static constexpr int kGroupIndent =
        kIconSize + 6; // child-row indent (aligns with section label)
    int _relevantDays =
        14; // configurable via setRelevantDays(); default matches kDefaultRelevantDays
    static constexpr int kDefaultRelevantDays   = 14;
    // Global default for conversations with NotificationLevel::Default. Mirrors
    // the Settings "Notify me about" radio (default: All new posts).
    NotificationLevel    _defaultNotify         = NotificationLevel::All;
    // See setHighlightMentionsOnlyUnreads().
    bool                 _highlightMentionsOnly = true;

    // The single "does this row paint as unread" rule — bold/bright emphasis in
    // paintRow and the unreads-only filter share it, so the filter never hides a
    // bold row or lists a dim one.
    bool paintsUnread(const Conversation &c) const;
};
