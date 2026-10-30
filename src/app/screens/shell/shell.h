// The app shell: our own title bar, the workspace rail, the sidebar, the
// conversation header with its tabs, the message area (the messages screens'
// MessageList once linked; a simple stand-in until then), the composer, the
// thread panel on a resizable splitter, and the OS integration — tray icon,
// notifications, the launcher badge, single-instance activation.
//
// Implements the navigation half of screens::Context (openConversation,
// openThread, closeThread, openUrl).
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "screens/shell/composer.h"
#include "screens/shell/nav_history.h"
#include "screens/shell/settings.h"
#include "screens/shell/sidebar.h"
#include "screens/shell/swipe_nav.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace claude {
class Links;
}
namespace settings {
class SettingsDialog;
}
namespace update {
class Updater;
}

namespace shell {

class CanvasPage;
class ConvHeader;
class OverviewPage;
class TeammatePage;
class SavedPage;
class ActivityPage;
class ScheduledPage;
class ThreadsPage;
class ConvTabs;
class HuddleBanner;
class Menus;
class MessageSearch;
class ProfileCards;
class QuickSwitcher;
class ThreadArea;
class TypingIndicator;
class UpdateBar;
class WorkspaceStack;

class Shell {
public:
    Shell(
        screens::Context &ctx,
        ui::Window       &win,
        Settings         &settings,
        std::string       settingsPath,
        std::string       notifyPath = {}
    );
    ~Shell();

    // Signed in to a workspace (the default), or not: then the rail shows no
    // workspace and the "Log in to workspace" page replaces the
    // sidebar and the conversation.
    void                                   setSignedIn(bool on);
    bool                                   signedIn() const { return _signedIn; }
    // The rail's "+" and "Log in to workspace";
    // `anchor` is where the service menu opens (window coordinates).
    std::function<void(ui::PointF anchor)> onAddWorkspace;
    // The workspace's name, icon or URL changed (signed in, switched): the
    // rail tile, the tray menu and the header follow.
    void                                   workspaceChanged();
    // The rail: every signed-in workspace, in the
    // saved order, and the one the Store holds ("" when signed out). The
    // active tile shows the Store's name and icon; the others these.
    struct Workspace {
        std::string key; // "service:id"
        std::string id;  // the record's id (its own icon file is keyed by it)
        std::string name;
        std::string icon; // a local picture ("" = the letter)
        bool        muted = false;
    };
    void                          setWorkspaces(std::vector<Workspace> list, std::string activeKey);
    const std::vector<Workspace> &workspaces() const { return _workspaces; }
    // The rail's order (a drag may have changed it), top to bottom.
    std::vector<std::string>      railOrder() const;
    ui::View                     *workspaceTile(const std::string &key) const; // tests
    int workspaceDot(const std::string &key) const; // tests: its tile's dot (0, 1 blue, 2 red)
    // A click on a tile or its tray item, and a drop that reordered the rail.
    std::function<void(const std::string &key)>               onSwitchWorkspace;
    std::function<void(const std::vector<std::string> &keys)> onReorderWorkspaces;
    // Settings → System → Slack connection (the accounts controller's).
    std::function<void()> onImportSlackSession, onConvertToSession;
    std::function<int()>  oauthSlackWorkspaces;
    // Every running workspace: the accounts
    // controller attaches each signed-in workspace's Store and Backend while
    // it runs, open or in the background. Its new messages notify (the title
    // says which workspace when it isn't the open one), its unread state
    // shows on its rail tile and counts toward the tray and launcher badge,
    // and the quick switcher, the forward dialog and the workspace menu
    // reach it. With none attached (the demo, tests) the open Store counts
    // alone.
    void attachWorkspace(const std::string &key, model::Store &store, model::Backend &backend);
    void detachWorkspace(const std::string &key);
    // After its first load: from then on its new messages notify.
    void setWorkspaceLive(const std::string &key, bool live);
    struct LiveWorkspace {
        std::string     key, name, icon; // the rail's name and picture
        model::Store   *store   = nullptr;
        model::Backend *backend = nullptr;
    };
    // The attached workspaces in the rail's order.
    std::vector<LiveWorkspace> liveWorkspaces() const;
    // A message reminder went off in workspace `key` (Backend::onReminderDue).
    void                       notifyReminderDue(
        const std::string &key, model::Store &store, model::ConvRef conv, model::Ts ts
    );
    // "Session expired", when the window is hidden.
    void notifySessionExpired(const std::string &workspace);
    // A message workspace `key` never sent (Backend::onUnsent): back in the
    // composer showing (conv, thread) — in front of what's typed there since,
    // its files attached again — or else in front of that draft.
    void giveBackUnsent(
        const std::string              &key,
        const model::Store             &store,
        model::ConvRef                  conv,
        model::Ts                       thread,
        const std::string              &text,
        const std::vector<std::string> &files
    );
    // The error banner: a message no call waits for (a queued Claude Code
    // message that never went out), shown for a few seconds.
    void               showError(const std::string &message);
    ui::Label         *errorBanner() const { return _errorBanner; } // tests
    // The parallel-usage banner: the same app keys run on another device and
    // keep interrupting the realtime connection. Stays until closed.
    void               showParallelUsage();
    ui::View          *parallelUsageBanner() const { return _parallelBanner; } // tests
    plat::Tray        *tray() const { return _tray.get(); }                    // tests
    const gfx::Bitmap *customTrayPicture() const { return _trayImage.get(); }  // tests

    // Navigation (also wired into the Context).
    void           open(model::ConvRef conv);
    void           openThread(model::ConvRef conv, model::Ts root);
    void           closeThread();
    // Before the Store is cleared for another workspace (Accounts::activate):
    // the open chat, thread and teammate page are left (drafts stashed), so
    // nothing points into the old data. The back/forward history stays: it
    // crosses workspaces.
    void           leaveWorkspace();
    // A signed-out workspace: its history entries go.
    void           purgeHistory(const std::string &key);
    model::ConvRef current() const { return _current; }
    bool           threadOpen() const;
    void           showQuickSwitcher();
    // Back/forward through the conversations opened (NavBack/NavForward),
    // switching workspaces where an entry is another's; false when there is
    // nowhere to go.
    bool           navigateHistory(bool back);
    // CloseFrontmost: the top dialog, else Settings, else the window.
    void           closeFrontmost();
    // SearchMessages and the header's search button: toggle the search
    // overlay over the message area.
    void           openSearch();
    MessageSearch *messageSearch() const { return _search; }
    QuickSwitcher *quickSwitcher() const { return _switcher; }
    void           showCanvas(bool on); // the header's tabs
    void           lookUpCanvas(model::ConvRef conv);
    CanvasPage    &canvasPage() const { return *_canvas; }
    // The profile card; beside `anchor` (window rect of the avatar/name)
    // when given, else centred near the top.
    void           showProfile(model::UserRef u, ui::RectF anchor = {});
    // The rename dialog: a local name for a group DM (or agent session).
    void           renameConversation(model::ConvRef c);
    // "Find a channel" on its Channels (0) or
    // People (1) tab — a channel opens (joined first), a person's DM opens.
    void           openBrowseDialog(int tab);
    // "Create a channel": the two-step dialog, then the backend creates it.
    void           openCreateChannel();

    // ── Agent thread links (shell_links.cpp) ──
    // The accounts controller's links (null: none, the default): the
    // message toolbar's robot, the message menu's items, the session
    // picker, the thread panel's chip — all of it shows only while set.
    void setAgentLinks(claude::Links *links);

    // ── Agent workspace: sessions and the team (shell_agents.cpp) ──
    void openSessionFinder();
    // The directory picker, then a new session there (opened, composer focused).
    void startAgentSession(bool skipPermissionChecks);
    // The teammate's page in the content area; its composer starts a session.
    void openTeammate(const std::string &role);
    // A message forwarded to a teammate: its page,
    // `text` (mrkdwn) after what was typed there and the files added — the
    // user picks the folder and sends.
    void prefillTeammate(const std::string &role, std::string text, std::vector<std::string> paths);
    bool teammateOpen() const;
    TeammatePage *teammatePage() const { return _teammatePage; }
    void          editTeammate(const std::string &role); // "" adds one
    void          restoreTeammate(const std::string &role);
    void          removeTeammate(const std::string &role);

    // The sidebar's entries over the sections, each an overview page in the
    // content area in the conversation's place, no composer, refilled on
    // every open: Threads (the followed threads; the cards have their own
    // reply boxes), Saved messages (the saved list as cards), Scheduled
    // messages (what waits to be posted, as cards).
    using Page = Sidebar::Nav;
    void           showPage(Page p);
    bool           pageOpen(Page p) const;
    ActivityPage  *activityPage() const { return _activityPage; }
    ThreadsPage   *threadsPage() const { return _threadsPage; }
    SavedPage     *savedPage() const { return _savedPage; }
    ScheduledPage *scheduledPage() const { return _scheduledPage; }
    // The conversation, then the message (inside
    // its thread when `thread` is its root), scrolled to and flashed.
    void           jumpToMessage(model::ConvRef conv, model::Ts ts, model::Ts thread);
    // NotificationActivated handling, also reachable via main.cpp's
    // single-instance hand-off (another process received the click).
    void           activateNotification(
        uint64_t id, const std::string &action = {}, const std::string &token = {}
    );

    // The Settings dialog (the rail's gear, the tray menu); a second call
    // while it is open does nothing.
    void                      openSettings();
    // …on its AI assistance page (the Summarize notice's "Open settings").
    void                      openAiSettings();
    settings::SettingsDialog *settingsDialog() const { return _settingsDlg; }

    // Starts notifying about incoming messages (after the initial load); also
    // ends the first-load state (waiting()).
    void setLive(bool on);
    // The first workspace load with nothing cached: the conversation
    // column hidden and the message area's loading ring (with its hints)
    // until the conversations arrive. On while signed in with none yet.
    bool waiting() const { return _waiting; }
    // App-level plat events: tray, notification clicks, second instances.
    void handleAppEvent(const plat::Event &e);

    Sidebar                    &sidebar() { return *_sidebar; }
    HuddleBanner               *huddleBanner() const { return _huddleBanner; }
    Composer                   &composer() { return *_composer; }
    Composer                   *threadComposer();
    // The thread panel (the messages screens' ThreadPanel), null without them.
    ui::View                   *threadPanel() const;
    // The message list (the messages screens' MessageList), null without them.
    ui::View                   *messagesView() const { return _messages; }
    ConvHeader                 &header() { return *_header; }
    ConvTabs                   &tabs() { return *_tabs; }
    TypingIndicator            &typing() { return *_typing; }
    DraftStash                 &drafts() { return _drafts; }
    Avatars                    &avatars() { return _avatars; }
    Menus                      &menus() { return *_menus; } // the context menus (context_menus.h)
    ProfileCards               &profiles() { return *_profiles; }
    // The arrow badge a swipe that navigated flashes over the chat.
    const SwipeIndicator       &swipeBadge() const { return *_swipeBadge; }
    // Window-chrome hit test for Decorations::Custom (logical window coords).
    plat::HitArea               hitTest(plat::Point p) const;
    // Everything worth saving happens on close: geometry + stashed drafts,
    // the settings written at once.
    void                        saveState();
    // Issue #45: a window the work area can't hold
    // shrinks to it (its minimum first) and one hanging off it is pulled
    // back; a window that fits is left alone. At start, on every show and
    // whenever monitors come, go or change, or the window moves to another.
    void                        fitToScreen();
    // The minimum size before a small screen lowers it.
    static constexpr plat::Size kMinWindowSize{800, 600};
    // The window's close: hides to the tray when that is on and a tray host
    // shows our icon; false = nothing hidden, the caller quits.
    bool                        hideToTray();
    // The title bar's minimize: to the tray with Settings → Minimize to tray.
    void                        minimize();
    // Applies the settings the shell and the screens read (time format,
    // Ctrl+Enter, link previews, animations, cache bound, tray icon); called
    // at start and after every Settings change.
    void                        applySettings();
    // The settings to _settingsPath a moment after the first change, off the
    // UI thread (a burst of changes is one write; a warning when it fails).
    // saveState writes what is still waiting at once.
    void                        saveSettingsSoon();
    // Shows and raises the window, un-hiding and un-minimising it (tray
    // clicks, notification clicks, second instances).
    void                        restore(const std::string &activationToken = {});
    // The main window shows: not hidden to the tray, not minimised (where
    // the OS says). Every running backend hears it (Backend::setWindowVisible).
    bool                        windowVisible() const { return _windowVisible; }
    // Tray "Quit": saves state, then onQuit.
    void                        quit();
    std::function<void()>       onQuit;

    // Quit, then the same executable starts again with
    // restartArgs (base::relaunchOnExit) — an applied update, Slack app keys.
    void                     restart();
    std::vector<std::string> restartArgs;
#ifdef MSGA_SELF_UPDATE
    // The in-app updater (main's; null: none): the update bar, Settings'
    // "Check for updates", a silent check 5 s in.
    void       setUpdater(update::Updater *u);
    UpdateBar *updateBar() const { return _updateBar; }
#endif

private:
    void        openSettingsAt(uint8_t page); // a settings::SettingsDialog::Page
    void        buildTitleBar(ui::View *parent);
    void        buildRail(ui::View *parent);
    void        buildMain(ui::View *parent);
    void        addWorkspace(ui::PointF anchor);
    void        togglePin();  // "Pin window on top"
    void        syncChrome(); // the OS-drawn chrome follows the app theme
    void        updateTitleButtons();
    void        updateHeader();
    void        applyScheduleSend();
    void        updateHuddleBanner();
    std::string workspaceIconPath() const;
    void        refreshWorkspaceIcon();
    void        setWaiting(bool on);
    void        updateAttention(); // badge count, tray dot, rail dots
    void        attentionSoon();   // a background workspace's counts changed
    void        onChange(const model::Change &ch);
    void        onWorkspaceChange(const std::string &key, const model::Change &ch);
    // New messages in `st` (`key`'s, "" = the open Store unattached): the
    // ones someone else posted that the settings want announced.
    // New messages, huddles, reminders (shell_notify.cpp).
    void        messagesArrived(model::Store &st, const std::string &key, const model::Change &ch);
    void        maybeNotify(
        model::Store         &st,
        const std::string    &key,
        model::ConvRef        conv,
        const model::Message &m,
        bool                  allowDefer
    );
    void notifyWhenUsersResolve(
        model::Store                   *st,
        const std::string              &key,
        model::ConvRef                  conv,
        std::shared_ptr<model::Message> m,
        std::vector<model::UserRef>     pending,
        int                             tries
    );
    void            huddleChanged(model::Store &st, const std::string &key, model::ConvRef conv);
    model::Backend &backendFor(const model::Store &st);
    bool            storeAlive(const model::Store *st) const; // open or attached
    // The Store of a workspace key: nullptr once it is gone (a draft of one is
    // then gone too).
    model::Store   *storeFor(const std::string &key) const;
    std::string     teamTitle(const model::Store &st, const std::string &key, std::string title);
    // The first of `paths` with a picture, for an OS notification, to `fn`:
    // at once when decoded already (or there is none to wait for), else once
    // the worker has decoded it.
    void notificationImage(std::vector<std::string> paths, std::function<void(plat::Image)> fn);
    std::string        workspaceIconFor(const model::Store &st) const;
    uint64_t           post(const plat::Notification &n);
    model::NotifyLevel defaultLevel() const; // Settings → Notifications: All or Mentions
    model::NotifyLevel effectiveLevel(const model::Conversation &c) const;

    struct Notified;
    // `n` with the first of `pics` there is, timed out and silent (the
    // chosen sound is played here), its click opening `where`.
    void postNotification(plat::Notification n, std::vector<std::string> pics, Notified where);

    struct Running; // an attached workspace
    Running   *findRunning(const std::string &key);
    void       refreshTrayIcon(int mentions, bool unread);
    void       rebuildTrayMenu();
    void       resetWindowGeometry();
    uint64_t   _fitMonitor = 0; // the monitor the window was last fitted on
    ui::Popup *topDialog() const;
    bool       removeIdleSession(const ui::Event &e); // the window's key filter
    bool       navInput(const plat::Event &e);        // the window's input filter
    // A workspace's tray item is kTrayWorkspace + its index on the rail.
    enum : uint32_t { kTraySettings = 1, kTrayResetSize, kTrayQuit, kTrayWorkspace = 100 };
    void          showWorkspaceMenu(const std::string &key, ui::PointF at);
    void          refreshRail(); // the tiles' names, icons and active state
    bool          hideWindow();  // to the tray, while a tray host shows our icon
    // The open chat is on screen and the window focused
    // (not hidden, minimized or in the background); updateReading tells the
    // message list.
    bool          reading() const;
    void          updateReading();
    // Agent workspace (shell_agents.cpp): the teammate page's wiring, leaving
    // it (its draft kept), its composer's lock, sending to it.
    void          buildTeammatePage(ui::View *stack);
    void          buildThreadsPage(ui::View *stack);
    void          buildSavedPage(ui::View *stack);
    void          buildActivityPage(ui::View *stack);
    void          buildScheduledPage(ui::View *stack);
    // What the overview pages and the teammate page take away: the
    // conversation's chrome and, unless kept (the teammate page writes to
    // the teammate), the composer; its draft is stashed.
    void          leaveConversationChrome(bool keepComposer);
    OverviewPage *pageView(Page p) const; // null: not built
    void          leavePage(Page p);
    void          leavePages();               // whichever overview page is open
    void          setupComposer(Composer &c); // the avatars, GIF key and tips every composer gets
    void          leaveTeammate();
    void          refreshTeammates();
    void          applyTeammateComposer();
    bool          startSessionWithTeammate();
    void          saveSettingsNow(); // what saveSettingsSoon has waiting, written at once
    // The same without the wait on the UI thread: asked for now, written
    // on a worker (in order with every other settings write).
    void          saveSettingsAsync();
    // _notified across restarts, debounced the same way as settings
    // (saveSettingsSoon); loaded once in the constructor.
    void          loadNotified();
    void          saveNotifiedSoon();
    void          saveNotifiedNow(); // what saveNotifiedSoon has waiting, written at once
    // The id for the next plat::App::notify() call: persisted (same file as
    // _notified) so it's never reused across a restart -- a backend only
    // knows about requests from its own run, so we own uniqueness instead.
    uint64_t      nextNotifyId();
    void
    showSampleNotification(plat::Notification n, std::function<void(const std::string &)> result);
    void sampleNotificationReady(const plat::Notification &n); // its picture is there
#ifdef MSGA_SELF_UPDATE
    void applyUpdate(); // the update bar's button
#endif
    void storeVisited(); // the sidebar's visit stamps into _settings
    // The open workspace's section sorts: from _settings into the sidebar,
    // and back when the section menu changed one.
    void loadSorts();
    void storeSorts();
    // Claude Code UI for agent sessions: the
    // composer's lock and suggestion, slash commands that msga runs itself,
    // the footer's zen toggle, the thread panel's "Open as session".
    void applyComposerAccess();
    void runCommand(
        model::ConvRef conv, model::Ts thread, const std::string &name, const std::string &args
    );
    void                        wireAgentUi();
    // Agent thread links (shell_links.cpp).
    screens::Context::AgentLink agentLink(model::ConvRef conv, const model::Message &m) const;
    void agentLinkAction(model::ConvRef conv, model::Ts ts, screens::Context::AgentLinkAction a);
    void agentLinksChanged();
    // The workspace whose Store is `st` comes on screen (switched to when
    // it isn't); true once its Store is the open one.
    bool showWorkspaceOf(const model::Store *st);
    claude::Links *_links = nullptr;

    screens::Context &_ctx;
    ui::Window       &_win;
    Settings         &_settings;
    std::string       _settingsPath;
    std::string       _notifyPath; // _notified's file, empty = don't persist (tests, no app dir)
    Avatars           _avatars;
    DraftStash        _drafts;

    Sidebar               *_sidebar  = nullptr;
    Composer              *_composer = nullptr;
    ui::View              *_titleBar = nullptr, *_rail = nullptr;
    float                  _titleBarH  = 0;       // buildTitleBar
    ui::Label             *_titleLabel = nullptr; // macOS: the workspace, without a header
    WorkspaceStack        *_wsStack    = nullptr;
    std::vector<Workspace> _workspaces;
    std::string            _activeKey;
    bool                   _haveWorkspaces = false; // setWorkspaces was called
    ui::View              *_listHandle = nullptr, *_signedOut = nullptr, *_addBtn = nullptr;
    bool                   _signedIn = true;
    ui::Button    *_pinBtn = nullptr, *_minBtn = nullptr, *_maxBtn = nullptr, *_closeBtn = nullptr;
    ui::Clickable *_prefsBtn                = nullptr;
    ConvHeader    *_header                  = nullptr;
    ConvTabs      *_tabs                    = nullptr;
    HuddleBanner  *_huddleBanner            = nullptr;
    TypingIndicator              *_typing   = nullptr;
    ui::View                     *_messages = nullptr, *_mainPane = nullptr;
    CanvasPage                   *_canvas   = nullptr;
    ui::View                     *_welcome  = nullptr; // the shortcuts panel (no conversation open)
    ui::View                     *_splitter = nullptr;
    ThreadArea                   *_thread   = nullptr;
    QuickSwitcher                *_switcher = nullptr;
    MessageSearch                *_search   = nullptr;
    std::unique_ptr<Menus>        _menus;
    std::unique_ptr<ProfileCards> _profiles;

    model::ConvRef              _current = model::kNoConv;
    NavHistory                  _nav;
    bool                        _navApplying = false, _navSwitching = false;
    // A back/forward jump into a workspace whose
    // conversations haven't arrived yet; opened when they do.
    NavLocation                 _pendingNav;
    void                        applyPendingNav();
    NavLocation                 here(model::ConvRef conv) const;
    SwipeNav                    _swipe;
    SwipeIndicator             *_swipeBadge = nullptr;
    model::Store::ObserverId    _observer   = 0;
    bool                        _live       = false;
    bool                        _waiting    = false;
    int                         _lastBadge = -1, _lastTray = -1;
    bool                        _hidden = false, _hiddenMaximized = false;
    std::unique_ptr<plat::Tray> _tray;
    // convId, not a ConvRef: a ConvRef is a transient Store index, dead
    // after a restart reloads the Store. Resolved back on click.
    struct Notified {
        std::string key; // the workspace ("" = the open Store unattached)
        std::string convId;
        model::Ts   root = 0, ts = 0; // a reply's thread; ts: scroll to it (reminders)
        std::string join;             // a huddle's link (its "Join" action)
        int64_t     postedAt = 0;     // base::nowSecs(), set by postNotification -- pruning
    };
    // Notification id -> where it opens. Persisted to _notifyPath: macOS
    // and Windows keep delivered notifications around across launches, so
    // a click after a restart needs this to still resolve.
    std::unordered_map<uint64_t, Notified> _notified;
    std::unordered_set<std::string>        _notifiedHuddles; // "key\x1Fconv": announced, still live
    plat::TimerId                          _notifySaveTimer = 0; // saveNotifiedSoon's
    uint64_t                               _nextNotifyId    = 1; // nextNotifyId's, persisted
    std::vector<plat::TimerId>             _resolveTimers;       // notifyWhenUsersResolve's
    std::vector<std::unique_ptr<Running>>  _running;
    plat::TimerId                          _attentionTimer = 0;
    ui::Popup                             *_forward        = nullptr; // the open forward dialog

    settings::SettingsDialog *_settingsDlg   = nullptr;
    // What the message views were built with (a change rebuilds them).
    bool                      _builtPreviews = true, _built24h = false, _builtAnimate = true;
    std::string               _builtLanguage; // the date language the rows were built in
    bool                      _builtEmoji = true;
    std::string               _trayImagePath; // the custom tray picture, decoded
    std::shared_ptr<const gfx::Bitmap> _trayImage;
    // While it decodes off the UI thread the tray keeps what it shows; the
    // last state asked for is drawn once the picture is in.
    bool                               _trayDecoding = false, _trayUnread = false;
    int                                _trayMentions  = 0;
    // The teammate page and what was typed to each teammate (the
    // "teammate:<role>" drafts); guards the agent flows' callbacks.
    TeammatePage                      *_teammatePage  = nullptr;
    ActivityPage                      *_activityPage  = nullptr;
    ThreadsPage                       *_threadsPage   = nullptr;
    SavedPage                         *_savedPage     = nullptr;
    ScheduledPage                     *_scheduledPage = nullptr;
    struct TeammateDraft {
        std::string              role, html;
        std::vector<std::string> files;
    };
    std::vector<TeammateDraft> _teammateDrafts;
    std::shared_ptr<int>       _agentAlive       = std::make_shared<int>(0);
    // The error banner (5 s), and what the composer was last given
    // (applyComposerAccess hands changes on only).
    ui::Label                 *_errorBanner      = nullptr;
    bool                       _ownsBackendError = false; // set ctx.backend.onError
    plat::TimerId              _errorTimer       = 0;
    ui::View                  *_parallelBanner   = nullptr;
    // Real input feeds the presence link (noteActivity), throttled.
    int64_t                    _lastActivityNote = 0;
    void                       noteActivity();
    bool                       _windowVisible = true;
    void                       syncWindowVisible(); // after a hide, show or state change
    std::string                _composerLock, _composerSuggestion;

    // The visit stamps' save (coalesced), the update bar and checker,
    // Settings' sample notification and where its outcome goes.
    plat::TimerId _visitedTimer = 0;
    plat::TimerId _saveTimer    = 0; // saveSettingsSoon's
#ifdef MSGA_SELF_UPDATE
    plat::TimerId    _updateTimer    = 0;
    UpdateBar       *_updateBar      = nullptr;
    update::Updater *_updater        = nullptr;
    int              _updateListener = 0;
#endif
    uint64_t                                 _sampleNotification = 0;
    std::function<void(const std::string &)> _sampleResult;
};

} // namespace shell
