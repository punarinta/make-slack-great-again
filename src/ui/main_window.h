// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "auth/token_store.h"
#include "ui/composer/composer_draft.h"
#include "ui/content_view.h"
#include "ui/nav_history.h"
#include "rpl/lifetime.h"

#include <QWidget>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QSystemTrayIcon>
#include <QPointer>
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
class ForwardDialog;
class CanvasPage;
class SavedMessagesPage;
class TeammatePage;
class ThreadsPage;
class ConvTabsWidget;
class HeaderAvatarWidget;
class HuddleBanner;
class MembersPopup;
class ParallelUsageBanner;
class PopupTooltip;
class QSplitter;
class UpdateBar;
class UpdateChecker;
class DesktopNotifier;

class QCloseEvent;
class QShowEvent;

namespace auth {
class AuthStrategy;
}
#if defined(MSGA_DEMO)
namespace demo {
class Tour;
}
#endif

// A plain top-level QWidget, not a QMainWindow: we use no menu/tool/status bar or
// dock area, and QMainWindow alone links Qt's dock-area, menu-bar and tab-bar code
// into the static binaries (and blocks building Qt with -no-feature-dockwidget).
class MainWindow : public QWidget {
    Q_OBJECT
#if defined(MSGA_DEMO)
    // The scripted demo tour (--demo-tour) drives the real widgets — it reads
    // their geometry and calls the same private entry points the UI does.
    friend class demo::Tour;
#endif
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
    void     applyComposerAccess();
    void     startAgentSession(bool skipPermissionChecks);
    // A teammate's page (agent workspace): its sessions, and the composer
    // starting a new one with it.
    void     openTeammateView(const QString &role);
    bool     teammateViewOpen() const;
    // Composer on the teammate page: locked with the reason when no session
    // can start in the page's folder, else "Message <teammate>".
    void     applyTeammateComposer();
    // The Team section and an open teammate page, after the team changed.
    void     refreshTeammates();
    // "Add teammate" (`id` empty) / "Edit teammate…".
    void     editTeammate(const QString &id);
    void     removeTeammate(const QString &id);
    // The text (and any attached files) is the new session's first message.
    void     startSessionWithTeammate(const QString &text, const QStringList &filePaths = {});
    // Where the teammate page's unsent text is kept among the drafts.
    static ConversationId teammateDraftConv(const QString &role);
    // Slack connect entry: opens the session-import dialog (the default), with a
    // secondary "use app keys" escape into OAuth (loginWithService).
    void                  connectSlack();
    // Persist session-mode + save + activate imported session workspaces. Shared
    // by connectSlack() and the Settings import path.
    void                  addSessionWorkspaces(const QList<TokenStore::WorkspaceRecord> &records);
    // Convert existing app-key (OAuth) Slack workspaces to session auth in bulk,
    // reusing the `d` cookie from an already-session workspace, then restart.
    void                  migrateSlackToSession();
    void                  wireConvList(); // one-time Qt signal wiring (lambdas read _session)
    void                  connectToSession();
    void                  restoreLastConv();

    // Workspace management
    void refreshSwitcher();
    void logoutWorkspace(const QString &teamId);
    void toggleWorkspaceMute(const QString &teamId);
    // Pick/clear the local override for a workspace's icon (WorkspaceIconDialog).
    void changeWorkspaceIcon(const QString &teamId);
    void showWorkspaceMenu(const QString &teamId, const QPoint &globalPos);

    // Header helpers
    void    updateHeaderForConv(const ConversationId &conv);
    void    setHeaderGroupAvatars(const Conversation &conv);
    // The open conversation's member list, hanging below `anchorGlobal` (the
    // header's members button, or a group DM's stacked avatars).
    void    openMembersPopup(const QRect &anchorGlobal);
    // Open (creating if needed) the DM with `user` and navigate to it.
    void    openDmWith(UserId user);
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
    // `allowDefer` is false only on the self-scheduled retry below: a message
    // whose author or @mentions users.list omits is held back until users.info
    // resolves them, so the toast reads "Julian Bevan" and not "Someone:
    // @U0C3E7HGZHS".
    void maybeNotify(const QString &teamId, const EvMessageNew &ev, bool allowDefer = true);
    // Re-runs maybeNotify once every id in `pending` is known, or once the wait
    // budget (kNotifyResolveTries × kNotifyResolveStepMs) runs out — whichever
    // comes first. Re-entered with allowDefer = false, so a toast is never
    // deferred twice.
    void notifyWhenUsersResolve(
        const QString &teamId, const EvMessageNew &ev, std::vector<UserId> pending, int tries
    );
    // Popup notification (with a "Join" action button) when a huddle starts in a
    // non-muted conversation, even while the window is hidden to the tray.
    void maybeNotifyHuddle(const QString &teamId, const EvHuddleChanged &ev);
    // OS notification for a message reminder that came due (EvReminderDue from
    // the workspace's Session). Deliberately skips the per-conversation gates
    // maybeNotify applies (mute levels etc.) — the user explicitly asked to be
    // reminded; only the global notifications switch is honoured.
    void notifyReminderDue(const QString &teamId, const EvReminderDue &ev);
    // A workspace's credentials were rejected for good (token refresh failed or
    // the session cookie died). While the window is hidden in the tray or
    // minimized the login screen it falls back to is invisible, so raise an OS
    // notification whose click brings the window (and that workspace) back.
    // Nothing is shown when the window is already on screen — the logged-out
    // page itself is the message then.
    void notifySessionExpired(const QString &teamId);
    // What a click on the tray-balloon fallback opens (openNotifTarget):
    // workspace, conversation, and the thread root / exact message when set.
    void setPendingNotifTarget(
        const QString &teamId, const ConversationId &conv, const Ts &threadRoot, const Ts &msgTs
    );
    // The tray-balloon fallback for a notification the OS notifier didn't show:
    // `pix` as its icon, or the stock `iconWithoutPix` when there is none.
    void showTrayMessage(
        const QString               &title,
        const QString               &body,
        const QPixmap               &pix,
        int                          timeoutMs      = 5000,
        QSystemTrayIcon::MessageIcon iconWithoutPix = QSystemTrayIcon::NoIcon
    );
    // Fire a representative sample notification (Settings → "Sample
    // notifications" Test button); kind is a SettingsDialog::SampleNotif value.
    void showSampleNotification(int kind);
    // Bring the window forward and open the conversation a clicked notification
    // points at (shared by the tray and the freedesktop-notifier click paths).
    // When threadRoot is non-empty the notified message was a thread reply
    // (invisible in the channel timeline — conversations.history omits replies),
    // so open the thread panel at that root, matching what Slack does on click.
    // msgTs (reminder clicks) additionally scrolls to and flashes that exact
    // message — in the channel, or inside the thread when threadRoot is set.
    // Switch to `teamId` (if it isn't the active workspace) and open `conv`
    // there through the conv list's selection — the one path that moves the
    // header, the highlighted row and the message list together, and reveals
    // a row the relevance filter or a collapsed section is hiding. Shared by
    // notification clicks and the quick switcher. Empty teamId = active one.
    void openConversationIn(const QString &teamId, const ConversationId &conv);
    void openNotifTarget(
        const QString        &teamId,
        const ConversationId &conv,
        const Ts             &threadRoot = {},
        const Ts             &msgTs      = {}
    );
    // Reveal and load the thread panel for a conversation's thread root, sizing
    // the splitter if collapsed. Shared by the message-list thread click and the
    // thread-reply notification click.
    void openThreadPanel(const ConversationId &conv, const Ts &rootTs);
    // Leave the open conversation for an overview page (Threads, Saved
    // messages, a teammate): stash its draft, close the thread panel, stop
    // reading it and hide its chrome. The composer is left to the caller.
    void leaveConversationForOverview();
    // Show the workspace-wide Threads overview page in the content stack
    // (roster "Threads" entry; gated on Capabilities::threadsView).
    void openThreadsView();
    // Show the "Saved messages" page (roster entry; only visible while the
    // session has reminders).
    void openSavedMessagesView();
    // Show/hide the roster "Saved messages" entry to match the reminder list.
    void updateSavedMessagesEntry();
    // Follow a message-link chip: open the conversation (and, for a reply, the
    // thread it lives in) and move the focus to that message.
    void openMessageTarget(const ConversationId &conv, const Ts &ts, const Ts &threadRoot);
    // "Forward message": ask for a target, then re-post (or, on a label-based
    // backend, label) the message. `sourceConv` is where the message lives —
    // the thread panel can be showing a different conversation than the chat.
    void forwardMessage(const ConversationId &sourceConv, const Message &msg);
    // "Move to thread…" on a top-level message of the open conversation: pick a
    // thread among the loaded roots, then let the Session re-post + delete.
    void moveMessageToThread(const Message &msg);
    void updateUnreadBadges(const QString &teamId, const std::vector<Conversation> &convs);
    void updateTrayIcon();

    // "Find a channel" dialog; initialTab 0 = Channels, 1 = People.
    void openBrowseDialog(int initialTab);
    void openSessionFinder(); // an agent workspace's "Find a session"
    // "Name conversation…" on a group DM: dialog → Session::setConvLocalName,
    // then the header/composer of the open chat follow the new title.
    void renameConversation(ConversationId id);

    // Ctrl/Cmd+K: quick switcher over the active workspace's conversations.
    void openQuickSwitcher();

    // Error banner — shown briefly when a network error occurs with no UI handler.
    void showNetworkError(const QString &message);

    // Update
    void applyUpdateAndRestart();
    // Clean restart of the app (used after saving personal Slack app keys, which
    // only take effect on a fresh start). main() re-execs on kRestartExitCode.
    void restartApp();

    // Pull the window inside the work area of the screen it sits on. Shrink-only:
    // a display roomier than the window changes nothing at all (size or position).
    void fitToScreen();
    // Tray rescue for a window that has ended up unreachable: back to the default
    // size, fitted to the current screen, and centred on it.
    void resetWindowGeometry();

    // Event handlers
    bool eventFilter(QObject *o, QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void updateRoundedMask();
    void populateConversations(const std::vector<Conversation> &convs);
    void openConversation(int row);
    // Empty the composer (text + attachments + subject) and file the content
    // under the conversation being left, keyed by workspace AND conversation.
    // Every path that leaves a conversation — opening another one, the threads/
    // saved overviews, a workspace switch, logout — must run this so staged
    // input can never be sent into whatever is shown next. Content with no home
    // (no conversation open) is discarded rather than kept.
    void stashComposerDraft();
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
    QString             _composerLockReason;   // what applyComposerAccess last applied
    QString             _composerSuggestion;   // …and the suggested reply it last handed on
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

    WorkspaceSwitcher *_switcher           = nullptr;
    SettingsDialog    *_settingsDialog     = nullptr;
    QWidget           *_convPanel          = nullptr;
    QWidget           *_convResizeHandle   = nullptr;
    QWidget           *_rightArea          = nullptr; // nav.bg wrapper — restyled on theme switch
    ConvListWidget    *_convList           = nullptr;
    ConvFooterWidget  *_convFooter         = nullptr;
    // Last time real input was forwarded to the sessions' presence links
    // (Session::noteUserActivity) — throttles the app-wide event filter.
    qint64             _lastActivityNoteMs = 0;
    QLabel            *_convNameLabel      = nullptr;
    QWidget           *_msgHeader          = nullptr;
    QWidget           *_rightPanel = nullptr; // content-surface fill — restyled on theme switch
    QHBoxLayout       *_rightPanelLayout    = nullptr; // right-area outer layout — right/bottom gap
    QVBoxLayout       *_loggedOutPageLayout = nullptr; // same border treatment for login screen
    QSplitter         *_msgSplitter         = nullptr;
    QWidget           *_msgArea             = nullptr; // parent of contentStack + composer
    QStackedWidget    *_contentStack        = nullptr;
    MessageListWidget *_messageList         = nullptr;
    ComposerWidget    *_composer            = nullptr;
    TypingIndicatorWidget  *_typingIndicator = nullptr;
    SearchWidget           *_searchWidget    = nullptr;
    WelcomeWidget          *_welcomeTips     = nullptr;
    ThreadPanel            *_threadPanel     = nullptr;
    // The open "Forward message" dialog; dropSession closes it if it holds the
    // session going away (a Claude Code forward lists every workspace).
    QPointer<ForwardDialog> _forwardDialog;
    ConvTabsWidget         *_convTabs     = nullptr;
    HuddleBanner           *_huddleBanner = nullptr;
    CanvasPage             *_canvasPage   = nullptr;
    ThreadsPage            *_threadsPage  = nullptr;
    SavedMessagesPage      *_savedPage    = nullptr;
    TeammatePage           *_teammatePage = nullptr;
    QString                 _currentCanvasFileId; // channel canvas of _currentConvId; empty = none
    QString                 _currentCanvasTitle;

    std::vector<ConversationId> _convIds;
    ConversationId              _currentConvId;
    // Which kind of page the content stack shows. Not derivable from
    // _currentConvId: it is empty on an overview page too (see content_view.h).
    ContentView                 _contentView = ContentView::None;
    NavHistory                  _navHistory;
    bool                        _navApplying = false; // a back/forward jump is driving the UI
    ConversationId _pendingNavConv; // jump target awaiting the new workspace's conv list
    ConversationId _pendingNotifConv;
    QString        _pendingNotifTeam;
    // Thread root of the pending (tray-fallback) notification, empty for a plain
    // message; carried so a tray click opens the thread the reply lives in.
    Ts             _pendingNotifThreadRoot;
    // Exact message ts of the pending notification (reminder clicks only) —
    // scrolls to the reminded message; empty for ordinary notifications.
    Ts             _pendingNotifMsgTs;
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

    // "teamId\x1fconvId" → unsent composer input (text, attachments, subject),
    // written/read only by stashComposerDraft() and the openConversation()
    // restore. Workspace-qualified so identical conversation ids in two
    // workspaces can never surface each other's input.
    QHash<QString, ComposerDraft> _drafts;

    ImageCache          *_imgCache            = nullptr;
    QLabel              *_errorBanner         = nullptr;
    ParallelUsageBanner *_parallelUsageBanner = nullptr;
    QSystemTrayIcon     *_trayIcon            = nullptr;
    DesktopNotifier     *_desktopNotifier     = nullptr;
    QPushButton         *_membersBtn          = nullptr; // channels; a group DM uses its avatars
    PopupTooltip        *_membersBtnTooltip   = nullptr;
    MembersPopup        *_membersPopup        = nullptr; // lazily created
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

    // Screen-fit wiring (QWindow::screenChanged) is only possible once the
    // window handle exists, i.e. from the first showEvent onwards.
    bool _screenFitWired = false;
};
