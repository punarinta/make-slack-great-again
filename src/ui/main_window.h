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
#include <QSet>
#include <QUrl>
#include <map>
#include <memory>

class Session;
class ImageCache;
class MessageListWidget;
class ComposerWidget;
class TypingIndicatorWidget;
class ConvListWidget;
class ConvFooterWidget;
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
class ParallelUsageBanner;
class PopupTooltip;
class QSplitter;
class UpdateBar;
class UpdateChecker;
class DesktopNotifier;

class QCloseEvent;

namespace auth {
class AuthStrategy;
}

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
    // Open the target a clicked notification points at, from an opaque
    // encodeNotifToken() string (team + conv, plus a thread root for a reply).
    // Both the in-process notifier click signal and the Windows toast's
    // msga://notif protocol-activation funnel through here.
    void handleNotifToken(const QString &token);

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
    // Bring up background workspaces one per timer tick: each ensureSession()
    // parses that workspace's cache JSON synchronously (multi-MB users.json for
    // a large org), so starting them all on the constructor path would block
    // the first paint N-workspaces wide.
    void     ensureSessionsSequentially(QStringList pending);
    void     activateWorkspace(QString teamId);
    void     dropSession(QString teamId);
    void     switchToWorkspace(QString teamId);
    void     showLoggedOut();
    // Add-workspace entry point: with one registered service, starts its login
    // directly; with several, pops a ContextMenu (anchored at anchorGlobal) to
    // pick the service, then starts that one. Async — the workspace activates
    // from the auth strategy's success signal.
    void     promptAddWorkspace(const QPoint &anchorGlobal);
    // Runs one service's auth strategy and, on success, saves + activates the
    // new workspace.
    void     loginWithService(Service service);
    // Slack connect entry: opens the session-import dialog (the default), with a
    // secondary "use app keys" escape into OAuth (loginWithService).
    void     connectSlack();
    // Persist session-mode + save + activate imported session workspaces. Shared
    // by connectSlack() and the Settings import path.
    void     addSessionWorkspaces(const QList<TokenStore::WorkspaceRecord> &records);
    // Convert existing app-key (OAuth) Slack workspaces to session auth in bulk,
    // reusing the `d` cookie from an already-session workspace, then restart.
    void     migrateSlackToSession();
    void     wireConvList(); // one-time Qt signal wiring (lambdas read _session)
    void     connectToSession();
    void     restoreLastConv();

    // Workspace management
    void refreshSwitcher();
    void logoutWorkspace(const QString &teamId);
    void toggleWorkspaceMute(const QString &teamId);
    void showWorkspaceMenu(const QString &teamId, const QPoint &globalPos);

    // Header helpers
    void    updateHeaderForConv(const ConversationId &conv);
    void    updateStarBtn(bool starred);
    // Toggle the huddle banner from the open conversation's huddleActive flag.
    void    updateHuddleBanner();
    // Web join URL for a conversation's huddle: the room's own huddle_link if we
    // have it (the only link that reliably works for a DM huddle), else a
    // constructed app.slack.com/huddle link for a channel, or — for a DM with no
    // live huddle, where /huddle/ server-errors — the plain open-conversation
    // link. The teamId overload is for notifications, whose huddle may be in a
    // background workspace.
    QString huddleJoinUrl(const ConversationId &conv) const;
    QString huddleJoinUrl(const QString &teamId, const ConversationId &conv) const;

    // Search overlay
    void repositionSearch();

    // Back/forward chat navigation (mouse side buttons, XF86 Back/Forward
    // keys, Alt+Left/Right) — works across workspaces.
    void navigateHistory(bool back);
    void applyNavLocation(const NavLocation &loc);

    // Tray
    void setupTray();
    void restoreFromTray();
    void maybeNotify(const QString &teamId, const EvMessageNew &ev);
    // Popup notification (with a "Join" action button) when a huddle starts in a
    // non-muted conversation, even while the window is hidden to the tray.
    void maybeNotifyHuddle(const QString &teamId, const EvHuddleChanged &ev);
    // Fire a representative sample notification (Settings → "Sample
    // notifications" Test button); kind is a SettingsDialog::SampleNotif value.
    void showSampleNotification(int kind);
    // Bring the window forward and open the conversation a clicked notification
    // points at (shared by the tray and the freedesktop-notifier click paths).
    // When threadRoot is non-empty the notified message was a thread reply
    // (invisible in the channel timeline — conversations.history omits replies),
    // so open the thread panel at that root, matching what Slack does on click.
    void
    openNotifTarget(const QString &teamId, const ConversationId &conv, const Ts &threadRoot = {});
    // Reveal and load the thread panel for a conversation's thread root, sizing
    // the splitter if collapsed. Shared by the message-list thread click and the
    // thread-reply notification click.
    void openThreadPanel(const ConversationId &conv, const Ts &rootTs);
    void updateUnreadBadges(const QString &teamId, const std::vector<Conversation> &convs);
    void updateTrayIcon();

    // "Find a channel" dialog; initialTab 0 = Channels, 1 = People.
    void openBrowseDialog(int initialTab);

    // Error banner — shown briefly when a network error occurs with no UI handler.
    void showNetworkError(const QString &message);

    // Update
    void applyUpdateAndRestart();
    // Clean restart of the app (used after saving personal Slack app keys, which
    // only take effect on a fresh start). main() re-execs on kRestartExitCode.
    void restartApp();

    // Event handlers
    bool eventFilter(QObject *o, QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void updateRoundedMask();
    void populateConversations(const std::vector<Conversation> &convs);
    void openConversation(int row);
    // Put the cursor in the composer when a conversation becomes visible, but
    // only while the window is foreground — a background workspace switch or
    // restore must not steal focus from whatever the user is doing.
    void focusComposerIfActive();

    // All logged-in workspaces, alive for the whole app run so unread badges
    // and notifications keep working for workspaces that aren't on screen.
    struct WorkspaceSession {
        std::unique_ptr<Session> session;
        rpl::lifetime            lifetime; // background subscriptions (badges/notify/auth)
    };
    // _sessions is keyed by the WorkspaceKey handle string ("slack:T0123…").
    // The app-level Socket Mode socket is no longer owned here: each Slack
    // backend acquires it from the refcounted slack::SharedRealtime, so it
    // exists iff ≥1 Slack workspace is live.
    std::map<QString, WorkspaceSession> _sessions;
    Session                            *_session = nullptr; // active workspace's session

    QString             _activeTeamId;
    auth::AuthStrategy *_activeFlow = nullptr; // valid only while a login flow is in progress

    // Window frame
    TitleBar      *_titleBar      = nullptr;
    UpdateBar     *_updateBar     = nullptr;
    UpdateChecker *_updateChecker = nullptr;
    QWidget       *_frame         = nullptr; // central widget; hosts titleBar + _stack
    QVBoxLayout   *_frameLayout   = nullptr;

    QStackedWidget *_stack         = nullptr;
    QWidget        *_loggedOutPage = nullptr;
    QWidget        *_mainPage      = nullptr; // built lazily

    WorkspaceSwitcher *_switcher         = nullptr;
    SettingsDialog    *_settingsDialog   = nullptr;
    QWidget           *_convPanel        = nullptr;
    QWidget           *_convResizeHandle = nullptr;
    QWidget           *_rightArea        = nullptr; // nav.bg wrapper — restyled on theme switch
    ConvListWidget    *_convList         = nullptr;
    ConvFooterWidget  *_convFooter       = nullptr;
    QLabel            *_convNameLabel    = nullptr;
    QWidget           *_msgHeader        = nullptr;
    QWidget           *_rightPanel = nullptr; // content-surface fill — restyled on theme switch
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
    // Thread root of the pending (tray-fallback) notification, empty for a plain
    // message; carried so a tray click opens the thread the reply lives in.
    Ts             _pendingNotifThreadRoot;
    // "teamId\x1fconvId" of huddles we've already shown a notification for, so a
    // re-fired EvHuddleChanged (edit / history reconcile) can't double-notify;
    // cleared when the huddle ends so its next start notifies again.
    QSet<QString>  _notifiedHuddles;
    // teamIds the user muted: no OS notifications, no tray badge contribution
    // (in-app unread counters/emphasis still apply). Mirrors the persisted
    // TokenStore mute flag; rebuilt in refreshSwitcher() and on toggle.
    QSet<QString>  _mutedTeams;
    // teamId → {normal unreads (blue), important: DM unreads + mentions (red)}
    QHash<QString, QPair<int, int>> _wsUnreads;
    bool                            _convListWired = false;
    rpl::lifetime                   _uiLifetime; // active-workspace UI subscriptions

    QHash<QString, QString> _drafts; // convId.value → unsent draft text

    ImageCache          *_imgCache            = nullptr;
    QLabel              *_errorBanner         = nullptr;
    ParallelUsageBanner *_parallelUsageBanner = nullptr;
    QSystemTrayIcon     *_trayIcon            = nullptr;
    DesktopNotifier     *_desktopNotifier     = nullptr;
    QPushButton         *_huddleBtn           = nullptr;
    PopupTooltip        *_huddleBtnTooltip    = nullptr;
    QPushButton         *_starBtn             = nullptr;
    PopupTooltip        *_starBtnTooltip      = nullptr;
    QPushButton         *_searchBtn           = nullptr;
    PopupTooltip        *_searchBtnTooltip    = nullptr;
    HeaderAvatarWidget  *_headerAvatar        = nullptr;

    // Manual resize state (non-Wayland)
    Qt::Edges _resizeEdges = {};
    QPoint    _resizeDragStart;
    QRect     _resizeWinAtDrag;
    bool      _resizeHoverCursor = false;
};
