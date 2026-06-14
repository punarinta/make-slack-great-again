// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "auth/token_store.h"
#include "ui/nav_history.h"
#include "rpl/lifetime.h"

#include <QMainWindow>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QSystemTrayIcon>
#include <QPushButton>
#include <QUrl>
#include <map>
#include <memory>

class Session;
class SocketModeRealtime;
class OAuthFlow;
class ImageCache;
class MessageListWidget;
class ComposerWidget;
class TypingIndicatorWidget;
class ConvListWidget;
class WorkspaceSwitcher;
class SettingsDialog;
class SearchWidget;
class WelcomeWidget;
class TitleBar;
class ThreadPanel;
class CanvasPage;
class ConvTabsWidget;
class HeaderAvatarWidget;
class HuddleBanner;
class PopupTooltip;
class QSplitter;
class UpdateBar;
class UpdateChecker;

class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // main() checks for this exit code after app.exec() to trigger a clean re-exec.
    static constexpr int kRestartExitCode = 64;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

public slots:
    // Called by SingleInstance when the OS delivers msga://oauth/callback?code=…
    void handleOAuthUri(const QUrl &uri);

private:
    // UI construction (called once)
    void     buildUi();
    QWidget *buildLoggedOutPage();
    QWidget *buildMainPage(); // session-agnostic; built lazily

    // buildMainPage sub-builders — each sets the corresponding member variable(s).
    QWidget *buildWorkspaceSwitcher(QWidget *parent);
    QWidget *buildConvPanel(QWidget *parent);
    QWidget *buildRightPanel(QWidget *parent);

    void applyTheme();

    // Session lifecycle.  One Session per logged-in workspace stays alive in
    // the background (badges + notifications); activateWorkspace() points the
    // UI at one of them.  Team ids are taken BY VALUE: callers often pass
    // strings owned by structures these functions rebuild (switcher entries,
    // _activeTeamId, the sessions map), which would dangle behind a reference.
    Session *ensureSession(const QString &teamId);
    void     activateWorkspace(QString teamId);
    void     dropSession(QString teamId);
    void     switchToWorkspace(QString teamId);
    void     showLoggedOut();
    bool     runLoginFlow();
    void     wireConvList(); // one-time Qt signal wiring (lambdas read _session)
    void     connectToSession();
    void     restoreLastConv();

    // Workspace management
    void refreshSwitcher();
    void logoutWorkspace(const QString &teamId);
    void showWorkspaceMenu(const QString &teamId, const QPoint &globalPos);

    // Header helpers
    void    updateHeaderForConv(const ConversationId &conv);
    void    updateStarBtn(bool starred);
    // Toggle the huddle banner from the open conversation's huddleActive flag.
    void    updateHuddleBanner();
    // Web join URL for a conversation's huddle: the room's own huddle_link if we
    // have it, else a constructed app.slack.com/huddle link.
    QString huddleJoinUrl(const ConversationId &conv) const;

    // Search overlay
    void repositionSearch();

    // Back/forward chat navigation (mouse side buttons, XF86 Back/Forward
    // keys, Alt+Left/Right) — works across workspaces.
    void navigateHistory(bool back);
    void applyNavLocation(const NavLocation &loc);

    // Tray
    void setupTray();
    void maybeNotify(const QString &teamId, const EvMessageNew &ev);
    void updateUnreadBadges(const QString &teamId, const std::vector<Conversation> &convs);
    void updateTrayIcon();

    // "Find a channel" dialog; initialTab 0 = Channels, 1 = People.
    void openBrowseDialog(int initialTab);

    // Error banner — shown briefly when a network error occurs with no UI handler.
    void showNetworkError(const QString &message);

    // Update
    void applyUpdateAndRestart();

    // Event handlers
    bool eventFilter(QObject *o, QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void updateRoundedMask();
    void populateConversations(const std::vector<Conversation> &convs);
    void openConversation(int row);

    // All logged-in workspaces, alive for the whole app run so unread badges
    // and notifications keep working for workspaces that aren't on screen.
    struct WorkspaceSession {
        std::unique_ptr<Session> session;
        rpl::lifetime            lifetime; // background subscriptions (badges/notify/auth)
    };
    // App-level Socket Mode socket shared by every workspace backend — Slack
    // delivers all workspaces' events over one connection (and round-robins
    // between sockets of the same app, so per-workspace sockets lose events).
    // Declared before _sessions: ~PublicBackend unregisters its sink from it,
    // so it must be destroyed after the sessions.
    std::unique_ptr<SocketModeRealtime> _sharedRealtime;
    std::map<QString, WorkspaceSession> _sessions;
    Session                            *_session = nullptr; // active workspace's session

    QString    _activeTeamId;
    OAuthFlow *_activeFlow = nullptr; // non-owning; valid only while runLoginFlow() blocks

    // Window frame
    TitleBar      *_titleBar      = nullptr;
    UpdateBar     *_updateBar     = nullptr;
    UpdateChecker *_updateChecker = nullptr;
    QWidget       *_frame         = nullptr; // central widget; hosts titleBar + _stack
    QVBoxLayout   *_frameLayout   = nullptr;

    QStackedWidget *_stack         = nullptr;
    QWidget        *_loggedOutPage = nullptr;
    QWidget        *_mainPage      = nullptr; // built lazily

    WorkspaceSwitcher *_switcher            = nullptr;
    SettingsDialog    *_settingsDialog      = nullptr;
    QWidget           *_convPanel           = nullptr;
    QWidget           *_convResizeHandle    = nullptr;
    QWidget           *_rightArea           = nullptr; // nav.bg wrapper — restyled on theme switch
    ConvListWidget    *_convList            = nullptr;
    QLabel            *_convNameLabel       = nullptr;
    QWidget           *_msgHeader           = nullptr;
    QHBoxLayout       *_rightPanelLayout    = nullptr; // right-area outer layout — right/bottom gap
    QVBoxLayout       *_loggedOutPageLayout = nullptr; // same border treatment for login screen
    QSplitter         *_msgSplitter         = nullptr;
    QWidget           *_msgArea             = nullptr; // parent of contentStack + composer
    QStackedWidget    *_contentStack        = nullptr;
    MessageListWidget *_messageList         = nullptr;
    ComposerWidget    *_composer            = nullptr;
    TypingIndicatorWidget *_typingIndicator = nullptr;
    SearchWidget          *_searchWidget    = nullptr;
    WelcomeWidget         *_welcomeTips     = nullptr;
    ThreadPanel           *_threadPanel     = nullptr;
    ConvTabsWidget        *_convTabs        = nullptr;
    HuddleBanner          *_huddleBanner    = nullptr;
    CanvasPage            *_canvasPage      = nullptr;
    QString                _currentCanvasFileId; // channel canvas of _currentConvId; empty = none
    QString                _currentCanvasTitle;

    std::vector<ConversationId> _convIds;
    ConversationId              _currentConvId;
    NavHistory                  _navHistory;
    bool                        _navApplying = false; // a back/forward jump is driving the UI
    ConversationId _pendingNavConv; // jump target awaiting the new workspace's conv list
    ConversationId _pendingNotifConv;
    QString        _pendingNotifTeam;
    // teamId → {normal unreads (blue), important: DM unreads + mentions (red)}
    QHash<QString, QPair<int, int>> _wsUnreads;
    bool                            _convListWired = false;
    rpl::lifetime                   _uiLifetime; // active-workspace UI subscriptions

    QHash<QString, QString> _drafts; // convId.value → unsent draft text

    ImageCache         *_imgCache         = nullptr;
    QLabel             *_errorBanner      = nullptr;
    QSystemTrayIcon    *_trayIcon         = nullptr;
    QPushButton        *_huddleBtn        = nullptr;
    PopupTooltip       *_huddleBtnTooltip = nullptr;
    QPushButton        *_starBtn          = nullptr;
    PopupTooltip       *_starBtnTooltip   = nullptr;
    QPushButton        *_searchBtn        = nullptr;
    PopupTooltip       *_searchBtnTooltip = nullptr;
    HeaderAvatarWidget *_headerAvatar     = nullptr;

    // Manual resize state (non-Wayland)
    Qt::Edges _resizeEdges = {};
    QPoint    _resizeDragStart;
    QRect     _resizeWinAtDrag;
    bool      _resizeHoverCursor = false;
};
