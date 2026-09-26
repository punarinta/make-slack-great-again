// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "main_window.h"
#include "theme.h"
#include "theme_manager.h"
#include "header_avatar_widget.h"
#include "conv_list/named_conversation.h"
#include "image_cache.h"
#include "title_bar/title_bar.h"
#include "popup_tooltip/popup_tooltip.h"
#include "message_list/message_list.h"
#include "composer/composer_widget.h"
#include "typing_indicator/typing_indicator.h"
#include "conv_list/conv_list_widget.h"
#include "conv_footer/conv_footer_widget.h"
#include "context_menu/context_menu.h"
#include "app_dialog/app_dialog.h"
#include "workspace_switcher/workspace_switcher.h"
#include "session/session.h"
#include "cache/cache_evictor.h"
#include "auth/token_store.h"
#include "auth/auth_strategy.h"
#include "backend/slack/session_import/session_migrator.h"
#include "backend/slack/session_import/token_deriver.h"
#include "backend/slack/slack_auth.h"
#include "ui/session_import_dialog/session_import_dialog.h"
#include "ui/workspace_icon_dialog/workspace_icon_dialog.h"
#include "util/custom_tray_icon.h"
#include "util/custom_workspace_icon.h"
#include "auth/auth_strategy_factory.h"
#include "backend/backend.h"
#include "backend/backend_factory.h"
#include "backend/backend_registry.h"
#include "ui/file_dialog_utils.h"
#include "settings/settings_dialog.h"
#include "search/search_widget.h"
#include "thread_panel/thread_panel.h"
#include "message_list/message_render.h"
#include "canvas_page/canvas_page.h"
#include "threads_page/threads_page.h"
#include "saved_page/saved_messages_page.h"
#include "teammate_dialog/teammate_dialog.h"
#include "teammate_page/teammate_page.h"
#include "conv_tabs/conv_tabs_widget.h"
#include "welcome_tips/welcome_widget.h"
#include "forward_dialog/forward_dialog.h"
#include "move_to_thread_dialog/move_to_thread_dialog.h"
#include "create_channel_dialog/create_channel_dialog.h"
#include "rename_conversation_dialog/rename_conversation_dialog.h"
#include "session_finder_dialog/session_finder_dialog.h"
#include "session_status_dialog/session_status_dialog.h"
#include "profile_dialog/profile_dialog.h"
#include "status_dialog/status_dialog.h"
#include "browse_channels_dialog/browse_channels_dialog.h"
#include "quick_switcher/quick_switcher_dialog.h"
#include "update_checker/update_checker.h"
#include "huddle_banner/huddle_banner.h"
#include "members_popup/members_popup.h"
#include "parallel_usage_banner/parallel_usage_banner.h"
#include "update_bar/update_bar.h"
#include "styled_button/styled_button.h"
#include "shortcuts.h"

#include "ui/icon_utils.h"
#include "util/desktop_notifier.h"
#include "util/slack_links.h"
#include "util/sound_player.h"
#ifdef Q_OS_MACOS
#include "util/mac_app_badge.h"
#endif

#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QEventLoop>
#include <QIcon>
#include <QMessageBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QCursor>
#include <QDir>
#include <QSettings>
#include <QSplitter>
#include <QWindow>
#include <QBitmap>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <QDesktopServices>
#include <QScreen>
#include <QShowEvent>

#include <algorithm>
#include <memory>

static constexpr int kResizeBorder = 6;

// What the window asks for before the display gets a say. fitToScreen() only
// ever shrinks these — see its definition for why the minimum is negotiable.
static constexpr QSize kDefaultWindowSize{1200, 800};
static constexpr QSize kPreferredMinSize{800, 600};

static constexpr int    kConvMinWidth      = 160;
static constexpr int    kConvMaxWidth      = 400;
static constexpr int    kConvInitWidth     = 240;
// Min spacing between input notifications to the sessions' presence links
// (MainWindow::eventFilter → Session::noteUserActivity). Coarse on purpose: the
// WhileUsing idle timeout is 30 min and the links throttle tickles themselves.
static constexpr qint64 kActivityNoteGapMs = 20'000;

// The workspace identifier flowing through MainWindow (_sessions key,
// _activeTeamId, switcher ids, NavHistory) is the composite WorkspaceKey handle
// string, e.g. "slack:T0123ABCD". This helper resolves a handle to its neutral
// registry record (empty record if the handle is malformed or absent).
// Key for _drafts: workspace-qualified so identical conversation ids in two
// workspaces stay separate stashes (same separator convention as _notifiedHuddles).
static QString draftKey(const QString &teamId, const ConversationId &conv) {
    return teamId + QLatin1Char('\x1f') + conv.value;
}

static TokenStore::WorkspaceRecord recordForHandle(const QString &handle) {
    if (const auto key = WorkspaceKey::fromString(handle))
        if (const auto rec = TokenStore::loadWorkspace(*key))
            return *rec;
    return {};
}

// The icon to SHOW for a workspace — honours the user's local override, unlike
// the record's raw iconUrl (which is what the server serves).
static QString iconUrlForHandle(const QString &handle) {
    return TokenStore::displayIconUrl(recordForHandle(handle));
}

// Global default notification level (Settings → "Notify me about"): applied to
// conversations whose own level is NotificationLevel::Default. Defaults to "All
// new posts" (0); 1 = "Just mentions".
static NotificationLevel globalDefaultNotifLevel() {
    return QSettings("msga", "msga").value("notifications/level", 0).toInt() == 1
               ? NotificationLevel::Mentions
               : NotificationLevel::All;
}

// Settings → Notifications "Highlight mentions-only channels for any new
// message" (default on): whether a "Just mentions" channel paints bold in the
// chat list for unreads that don't @mention me (see
// ConvListWidget::setHighlightMentionsOnlyUnreads).
static bool highlightMentionsOnlyUnreads() {
    return QSettings("msga", "msga").value("notifications/boldMentionsOnly", true).toBool();
}

// Thin drag handle between the conv panel and the message area.
class ConvResizeHandle final : public QWidget {
public:
    explicit ConvResizeHandle(QWidget *target, QWidget *parent = nullptr)
        : QWidget(parent), _target(target) {
        setFixedWidth(4);
        setCursor(Qt::SizeHorCursor);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMouseTracking(true);
        connect(
            &ThemeManager::instance(),
            &ThemeManager::themeChanged,
            this,
            QOverload<>::of(&QWidget::update)
        );
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        if (_hovered) {
            // Same blue the composer's @mention popup uses for its highlighted
            // row — clearly visible, unlike the subtle nav.itemHover tint.
            p.fillRect(rect(), Th::c().text.link);
            return;
        }
        // At rest, blend into the conversation list this handle borders: the
        // same window-anchored chats-bar gradient, so the seam is invisible.
        p.fillRect(
            rect(), Th::navGradient(this, Th::c().nav.primaryGradTop, Th::c().nav.primaryGradBottom)
        );
    }

    void enterEvent(QEnterEvent *) override {
        _hovered = true;
        update();
    }

    void leaveEvent(QEvent *) override {
        _hovered = false;
        update();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;
        _dragging = true;
        _startX   = e->globalPosition().x();
        _startW   = _target->width();
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!_dragging)
            return;
        const int w =
            qBound(kConvMinWidth, _startW + int(e->globalPosition().x() - _startX), kConvMaxWidth);
        _target->setFixedWidth(w);
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton)
            _dragging = false;
    }

private:
    QWidget *_target;
    bool     _hovered  = false;
    bool     _dragging = false;
    qreal    _startX   = 0;
    int      _startW   = 0;
};

// Root container of the frameless window. At fractional display scale Qt rounds
// each child widget's painted device-pixel region independently, which can leave
// a 1-device-pixel gap at a sibling boundary; whatever this frame paints there
// shows through as a hairline seam. A single flat fill can't hide every seam,
// because the chrome blocks above it differ in colour — a dark nav block on one
// side, the light message surface on the other; one backdrop colour always
// contrasts with one of them.
//
// So the frame paints a low-fidelity *mirror* of the chrome: the dark nav tone
// everywhere, then the light content surface under the right-hand panel. A gap
// then exposes the same colour as the block beside it (dark next to nav, light
// next to the message area), so the seam vanishes instead of showing a
// contrasting line. The mirror reads live geometry/theme each paint, so it
// tracks window resizes, conv-panel drags and theme switches automatically.
class BackdropFrame final : public QWidget {
public:
    using QWidget::QWidget;

    // The light content panel (right of the resize handle) and the handle itself,
    // so the light region can start at the handle's right edge — the handle is
    // dark and must stay backed by the nav tone, not the light surface.
    void setContentSources(QWidget *content, QWidget *handle) {
        _content = content;
        _handle  = handle;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Th::c().nav.bg);
        if (_content && _content->isVisible()) {
            const QPoint tl = _content->mapTo(this, QPoint(0, 0));
            const int    hw = (_handle && _handle->isVisible()) ? _handle->width() : 0;
            const QRect  light(tl.x() + hw, tl.y(), _content->width() - hw, _content->height());
            p.fillRect(light, Th::c().surface.content);
        }
    }

private:
    QPointer<QWidget> _content;
    QPointer<QWidget> _handle;
};

MainWindow::~MainWindow() {
#ifdef Q_OS_MACOS
    // The Dock tile outlives the process, so an unread count left on it stays
    // painted over the icon after we quit. Clear it on the way out.
    macSetDockBadge(0);
#endif
}

// Zen mode is per workspace (off by default).
static QString zenModeKey(const QString &teamId) {
    return QStringLiteral("zenMode/") + QString::fromLatin1(QUrl::toPercentEncoding(teamId));
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("");
#ifdef Q_OS_MACOS
    // Keep AppKit's traffic lights, shadow, rounded corners, and resize handling.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    setWindowFlags(Qt::Window | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint);
    // We reserve room for the traffic lights in the header itself. Otherwise
    // QWidget inserts a second, empty title-bar-height margin above the header.
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#else
    // Earlier Qt versions need the native full-size-content style; the macOS
    // title-bar helper applies it once the NSWindow exists.
    setWindowFlags(Qt::Window);
#endif
#else
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
#endif
    setMouseTracking(true);
    setMinimumSize(kPreferredMinSize);
    resize(kDefaultWindowSize);
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
    updateRoundedMask();
    qApp->installEventFilter(this);

    Ui::Shortcuts::install(Ui::Shortcut::OpenSettings, this, [this] { _settingsDialog->open(); });

    // Back/forward chat navigation: mouse side buttons are handled in
    // eventFilter; these cover the keyboard equivalents (the registry binds both
    // the dedicated XF86 keys and the conventional Alt+arrows to each action).
    Ui::Shortcuts::install(Ui::Shortcut::NavBack, this, [this] { navigateHistory(true); });
    Ui::Shortcuts::install(Ui::Shortcut::NavForward, this, [this] { navigateHistory(false); });

    // Cmd+W (Ctrl+W elsewhere): close the frontmost thing. Our modal dialogs and
    // the settings panel are in-window child overlays, not top-level windows, so
    // nothing else would treat them as closeable — without this the shortcut
    // would hide the whole window with a dialog still up on it. Dismiss that
    // first (like macOS closing a sheet before its window); only when nothing is
    // open does the window itself close, which goes through closeEvent and so
    // hides to the tray exactly like the titlebar close button — the app keeps
    // running for badges and notifications.
    //
    // One shortcut has to arbitrate all of this: two QShortcuts on the same
    // sequence in one window (say a competing one owned by AppDialog) both match,
    // and Qt then reports the press as ambiguous and runs NEITHER handler. That
    // is what Ui::Shortcuts::install() now guards against registry-wide.
    Ui::Shortcuts::install(Ui::Shortcut::CloseFrontmost, this, [this] {
        if (AppDialog *dialog = AppDialog::topmostVisible(this)) {
            dialog->reject(); // same path as Escape / the × button / backdrop click
            return;
        }
        if (_settingsDialog && _settingsDialog->isVisible()) {
            _settingsDialog->hide();
            return;
        }
        close();
    });

    setupTray();

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        for (auto &[teamId, ws] : _sessions)
            ws.session->persistUnreads();
    });

    if (TokenStore::hasAnyWorkspace()) {
        const auto    keys         = TokenStore::workspaceKeys();
        const auto    active       = TokenStore::activeWorkspace();
        const QString activeHandle = active ? active->toString() : keys.front().toString();
        // Only the active workspace starts on the constructor path (its cached
        // conversation list is what the first paint shows). The rest still
        // connect — badges and notifications must not depend on clicking each
        // one — but deferred to after the first frame, one per tick, because
        // each start() parses that workspace's cache JSON synchronously.
        activateWorkspace(activeHandle);
        QStringList rest;
        for (const auto &key : keys)
            if (key.toString() != activeHandle)
                rest.append(key.toString());
        ensureSessionsSequentially(std::move(rest));
    } else {
        showLoggedOut();
    }

    const QByteArray geo = QSettings("msga", "msga").value("window/geometry").toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);

    // Both paths above can hand us a window the display can't hold: the default
    // size is a fixed guess, and a restored one was measured on whatever monitor
    // was attached last time.
    fitToScreen();

    // Displays come and go. Unplugging the external monitor of a two-screen setup
    // strands us at a size — or on coordinates — that no remaining screen can
    // hold. Re-fit once the windowing system has finished reshuffling, hence the
    // queued hop: geometry right at screenRemoved is still the pre-removal one.
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) {
        QMetaObject::invokeMethod(this, [this] { fitToScreen(); }, Qt::QueuedConnection);
    });
}

// ── UI construction ───────────────────────────────────────────────────────────

void MainWindow::buildUi() {
    _frame = new BackdropFrame(this);
    _frame->setObjectName("windowFrame");
    _frame->setMouseTracking(true);
    // Background is painted by BackdropFrame::paintEvent (a colour-matched
    // mirror of the chrome — see its definition), reading the live theme each
    // paint. No stylesheet on this root container (setStyleSheet here would
    // re-polish the whole window subtree on every theme switch, ~150 ms on a
    // populated window) and no autoFillBackground (the paintEvent fills it).

    _frameLayout = new QVBoxLayout(_frame);
    _frameLayout->setContentsMargins(0, 0, 0, 0);
    _frameLayout->setSpacing(0);

    _titleBar = new TitleBar(_frame);
    _titleBar->setTitle({});
    _frameLayout->addWidget(_titleBar);

    _updateBar = new UpdateBar(_frame);
    _frameLayout->addWidget(_updateBar);

    // Horizontal body: switcher rail always present, stack fills the rest.
    // _stack must be created before buildWorkspaceSwitcher (SettingsDialog parents to body).
    auto *body       = new QWidget(_frame);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    _frameLayout->addWidget(body, 1);

    _stack = new QStackedWidget(body);

    _updateChecker = new UpdateChecker(this);

    bodyLayout->addWidget(buildWorkspaceSwitcher(body));
    bodyLayout->addWidget(_stack, 1);

    setCentralWidget(_frame);

    connect(_updateChecker, &UpdateChecker::downloadReady, _updateBar, &UpdateBar::showUpdateReady);
    connect(_updateChecker, &UpdateChecker::checkFailed, this, &MainWindow::showNetworkError);
    connect(_updateBar, &UpdateBar::restartRequested, this, &MainWindow::applyUpdateAndRestart);
    QTimer::singleShot(5000, _updateChecker, &UpdateChecker::checkInBackground);

    // LRU cache cap: sweep shortly after startup, then periodically so a
    // long-running session can't grow the disk cache unbounded.
    QTimer::singleShot(15000, this, [] { CacheEvictor::instance()->schedule(); });
    auto *cacheSweep = new QTimer(this);
    cacheSweep->setInterval(30 * 60 * 1000);
    connect(cacheSweep, &QTimer::timeout, this, [] { CacheEvictor::instance()->schedule(); });
    cacheSweep->start();

    _loggedOutPage = buildLoggedOutPage();
    _stack->addWidget(_loggedOutPage);
    _stack->setCurrentWidget(_loggedOutPage);
}

QWidget *MainWindow::buildLoggedOutPage() {
    // Outer nav.bg wrapper — right/bottom margin exposes nav.bg as a colored border,
    // matching the same treatment applied to rightArea in buildMainPage().
    auto *wrapper = new QWidget;
    wrapper->setObjectName("loggedOutWrapper");
    wrapper->setAttribute(Qt::WA_StyledBackground);
    _loggedOutPageLayout = new QVBoxLayout(wrapper);
    _loggedOutPageLayout->setContentsMargins(0, 0, 0, 0);
    _loggedOutPageLayout->setSpacing(0);

    // Both backgrounds are applied (and re-applied) in applyTheme().
    auto *page = new QWidget(wrapper);
    page->setObjectName("loggedOutPage");
    page->setAttribute(Qt::WA_StyledBackground);
    _loggedOutPageLayout->addWidget(page);

    auto       *outer = new QVBoxLayout(page);
    const auto &sp    = Th::c().spacing;
    outer->setAlignment(Qt::AlignCenter);
    outer->setContentsMargins(32, 32, 32, 32);

    auto *inner = new QWidget(page);
    inner->setFixedWidth(300);

    auto *layout = new QVBoxLayout(inner);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(sp.xl);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *icon = new QLabel(inner);
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(72, 72);
    icon->setPixmap(QIcon(":/icon.svg").pixmap(QSize(72, 72), qApp->devicePixelRatio()));

    auto *titleBlock  = new QWidget(inner);
    auto *titleLayout = new QVBoxLayout(titleBlock);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(sp.md);

    auto *title = new QLabel("MSGA", titleBlock);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2; margin-top: 4px;")
                             .arg(Th::c().fonts.xxxl)
                             .arg(Th::qss(Th::c().text.primary)));

    auto *tagline = new QLabel(titleBlock);
    tagline->setAlignment(Qt::AlignCenter);
    tagline->setText(QString(
                         "<span style='font-size:%3px; color:%1; letter-spacing:0.06em;'>"
                         "[<span style='color:%2;'>m</span>ake "
                         "<span style='color:%2;'>s</span>lack "
                         "<span style='color:%2;'>g</span>reat "
                         "<span style='color:%2;'>a</span>gain]"
                         "</span>"
    )
                         .arg(Th::qss(Th::c().text.tertiary), Th::qss(Th::c().text.primary))
                         .arg(Th::c().fonts.sm));

    titleLayout->addWidget(title);
    titleLayout->addWidget(tagline);

    auto *loginBtn =
        new StyledButton(tr("Log in to workspace"), StyledButton::Variant::Primary, inner);
    connect(loginBtn, &QPushButton::clicked, this, [this, loginBtn] {
        promptAddWorkspace(loginBtn->mapToGlobal(loginBtn->rect().bottomLeft()));
    });

    layout->addWidget(icon, 0, Qt::AlignCenter);
    layout->addWidget(titleBlock);
    layout->addSpacing(sp.lg);
    layout->addWidget(loginBtn);

    outer->addWidget(inner, 0, Qt::AlignCenter);
    return wrapper;
}

QWidget *MainWindow::buildMainPage() {
    _imgCache = new ImageCache(this);
    _imgCache->setDiskCache(
        [this](const QString &url) -> QByteArray {
            return _session ? _session->cachedImage(url) : QByteArray{};
        },
        [this](const QString &url, const QByteArray &data) {
            if (_session)
                _session->cacheImage(url, data);
        }
    );
    _switcher->setImageCache(_imgCache);

    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);
    root->setSpacing(0);
    root->setContentsMargins(0, 0, 0, 0);

    root->addWidget(buildConvPanel(page));

    // Transparent wrapper: the resize-handle strip and the right panel each
    // paint their own background fully across it, and the window backdrop
    // (BackdropFrame) paints the correct colour — nav tone behind the handle,
    // light surface behind the panel — under any sub-pixel gap between them.
    // No own fill, so a stale palette colour can never read as a seam.
    auto *rightArea = new QWidget(page);
    rightArea->setObjectName("rightArea");
    _rightArea        = rightArea;
    _rightPanelLayout = new QHBoxLayout(rightArea);
    _rightPanelLayout->setContentsMargins(0, 0, 0, 0);
    _rightPanelLayout->setSpacing(0);
    _convResizeHandle = new ConvResizeHandle(_convPanel, rightArea);
    _rightPanelLayout->addWidget(_convResizeHandle);
    _rightPanelLayout->addWidget(buildRightPanel(rightArea), 1);
    root->addWidget(rightArea, 1);

    // Tell the window backdrop where the light content panel lives so it can
    // back that region with the message surface (the rest stays nav tone),
    // eliminating the hairline seam at the dark-chrome/light-content boundary
    // at fractional display scale. The resize handle is the dark strip on the
    // content's left edge, so the light fill starts to its right.
    static_cast<BackdropFrame *>(_frame)->setContentSources(rightArea, _convResizeHandle);
    // Repaint the backdrop when the content area moves/resizes without the
    // window itself resizing — i.e. when the conv panel is dragged wider or
    // shown/hidden — so the mirrored light region tracks it (a window resize
    // already repaints the frame).
    rightArea->installEventFilter(this);

    // Apply stored appearance setting and keep conv list in sync when settings are saved.
    _convList->setRelevantDays(
        QSettings("msga", "msga").value("appearance/relevantDays", 14).toInt()
    );
    connect(
        _settingsDialog,
        &SettingsDialog::appearanceChanged,
        _convList,
        &ConvListWidget::setRelevantDays
    );
    _convList->setShowAgentsApps(
        QSettings("msga", "msga").value("appearance/showAgentsApps", true).toBool()
    );
    connect(
        _settingsDialog,
        &SettingsDialog::agentsAppsVisibilityChanged,
        _convList,
        &ConvListWidget::setShowAgentsApps
    );
    _convList->setUnreadsOnly(
        QSettings("msga", "msga").value("appearance/unreadsOnly", false).toBool()
    );
    connect(
        _settingsDialog,
        &SettingsDialog::unreadsOnlyChanged,
        _convList,
        &ConvListWidget::setUnreadsOnly
    );
    connect(
        _settingsDialog, &SettingsDialog::stateCleared, _convList, &ConvListWidget::resetVisitedAt
    );
    // Timestamps are formatted at paint time, so a repaint is enough to apply
    // a new 12h/24h preference everywhere.
    connect(_settingsDialog, &SettingsDialog::timeFormatChanged, this, [this] {
        _messageList->viewport()->update();
        _threadPanel->refreshTimestamps();
    });
    // Threads display mode (standalone panel vs. inline expansion).
    _messageList->setThreadsInline(
        QSettings("msga", "msga").value("appearance/threadsInline", false).toBool()
    );
    connect(
        _settingsDialog,
        &SettingsDialog::threadDisplayChanged,
        _messageList,
        &MessageListWidget::setThreadsInline
    );
    const bool showLinkPreviews =
        QSettings("msga", "msga").value("appearance/showLinkPreviews", true).toBool();
    _messageList->setLinkPreviewsEnabled(showLinkPreviews);
    _threadPanel->setLinkPreviewsEnabled(showLinkPreviews);
    connect(
        _settingsDialog,
        &SettingsDialog::linkPreviewsChanged,
        _messageList,
        &MessageListWidget::setLinkPreviewsEnabled
    );
    connect(
        _settingsDialog,
        &SettingsDialog::linkPreviewsChanged,
        _threadPanel,
        &ThreadPanel::setLinkPreviewsEnabled
    );
    // Visual effects. The cache's retention switch is the OR of the kinds: it
    // can't tell an emoji from a GIF, the per-kind discards happen in the views.
    const auto applyAnimationSettings = [this] {
        QSettings  st("msga", "msga");
        const bool emoji = st.value("appearance/animateEmoji", true).toBool();
        const bool media = st.value("appearance/animateMedia", true).toBool();
        _imgCache->setAnimationsRetained(emoji || media);
        _messageList->setEmojiAnimationsEnabled(emoji);
        _messageList->setMediaAnimationsEnabled(media);
        _threadPanel->setEmojiAnimationsEnabled(emoji);
        _threadPanel->setMediaAnimationsEnabled(media);
        if (_convList)
            _convList->setEmojiAnimationsEnabled(emoji);
    };
    applyAnimationSettings();
    connect(_settingsDialog, &SettingsDialog::emojiAnimationsChanged, this, applyAnimationSettings);
    connect(_settingsDialog, &SettingsDialog::mediaAnimationsChanged, this, applyAnimationSettings);
    // Enter vs Ctrl+Enter: the composers read the registry per keypress; only
    // the welcome screen's shortcut panel holds built rows to refresh.
    connect(
        _settingsDialog,
        &SettingsDialog::sendKeyChanged,
        _welcomeTips,
        &WelcomeWidget::refreshShortcuts
    );
    // The global default notification level decides what unconfigured channels
    // notify/badge about; apply it now and re-resolve everything when it changes.
    _convList->setDefaultNotifyLevel(globalDefaultNotifLevel());
    _convList->setHighlightMentionsOnlyUnreads(highlightMentionsOnlyUnreads());
    connect(_settingsDialog, &SettingsDialog::notificationsChanged, this, [this] {
        _convList->setDefaultNotifyLevel(globalDefaultNotifLevel());
        _convList->setHighlightMentionsOnlyUnreads(highlightMentionsOnlyUnreads());
        for (auto &[teamId, ws] : _sessions)
            if (ws.session)
                updateUnreadBadges(teamId, ws.session->currentConversations());
    });
    connect(
        _settingsDialog,
        &SettingsDialog::testNotificationRequested,
        this,
        &MainWindow::showSampleNotification
    );
    // The presence preference is global: re-apply it to every live session (a
    // session created later reads it in Session::start()).
    connect(_settingsDialog, &SettingsDialog::presenceModeChanged, this, [this](PresenceMode m) {
        for (auto &[teamId, ws] : _sessions)
            if (ws.session)
                ws.session->setPresenceMode(m);
    });
    connect(_settingsDialog, &SettingsDialog::trayIconChanged, this, &MainWindow::updateTrayIcon);
    connect(_settingsDialog, &SettingsDialog::restartRequested, this, &MainWindow::restartApp);
    connect(
        _settingsDialog,
        &SettingsDialog::slackWorkspacesImported,
        this,
        [this](const QList<TokenStore::WorkspaceRecord> &recs) {
            if (recs.isEmpty())
                return;
            _settingsDialog->hide();
            addSessionWorkspaces(recs);
        }
    );
    connect(_settingsDialog, &SettingsDialog::migrateSlackToSessionRequested, this, [this] {
        _settingsDialog->hide();
        migrateSlackToSession();
    });
    // "Use my Slack theme": the active workspace when it can serve one (Slack
    // session auth), else the first that can.
    const auto themeSession = [this]() -> Session * {
        if (_session && _session->capabilities().sidebarTheme)
            return _session;
        for (auto &[teamId, ws] : _sessions)
            if (ws.session && ws.session->capabilities().sidebarTheme)
                return ws.session.get();
        return nullptr;
    };
    _settingsDialog->setSlackThemeSource({
        .available = [themeSession] { return themeSession() != nullptr; },
        .fetch =
            [this, themeSession](auto done) {
                Session *s = themeSession();
                if (!s) {
                    done({}, QStringLiteral("no_workspace"));
                    return;
                }
                s->loadSidebarTheme([dlg = QPointer<SettingsDialog>(_settingsDialog),
                                     done](SidebarThemePrefs prefs, QString err) {
                    if (dlg) // the reply may land after the dialog was torn down
                        done(prefs, err);
                });
            },
    });

    return page;
}

QWidget *MainWindow::buildWorkspaceSwitcher(QWidget *parent) {
    _switcher = new WorkspaceSwitcher(parent);
    _switcher->setObjectName("workspaceSidebar");
    connect(_switcher, &WorkspaceSwitcher::workspaceClicked, this, &MainWindow::switchToWorkspace);
    connect(_switcher, &WorkspaceSwitcher::addWorkspaceClicked, this, [this] {
        // Anchor the service menu at the add button's right edge (it lives in the
        // vertical left rail, so the menu opens to its right).
        promptAddWorkspace(_switcher->addButtonGlobalRect().topRight());
    });
    connect(
        _switcher, &WorkspaceSwitcher::workspaceRightClicked, this, &MainWindow::showWorkspaceMenu
    );
    connect(_switcher, &WorkspaceSwitcher::workspacesReordered, this, [](const QStringList &ids) {
        std::vector<WorkspaceKey> keys;
        keys.reserve(static_cast<size_t>(ids.size()));
        for (const auto &h : ids)
            if (auto k = WorkspaceKey::fromString(h))
                keys.push_back(*k);
        TokenStore::setWorkspaceOrder(keys);
    });

    _settingsDialog = new SettingsDialog(qobject_cast<QWidget *>(_stack->parent()));
    _settingsDialog->setUpdateChecker(_updateChecker);
    connect(_switcher, &WorkspaceSwitcher::settingsClicked, _settingsDialog, &SettingsDialog::open);
    return _switcher;
}

QWidget *MainWindow::buildConvPanel(QWidget *parent) {
    _convPanel = new QWidget(parent);
    _convPanel->setObjectName("convPanel");
    _convPanel->setFixedWidth(kConvInitWidth);

    auto *convLayout = new QVBoxLayout(_convPanel);
    convLayout->setContentsMargins(0, 0, 0, 0);
    convLayout->setSpacing(0);

    _convList = new ConvListWidget(_imgCache, _convPanel);
    _convList->setObjectName("convList");
    convLayout->addWidget(_convList, /*stretch=*/1);

    // Self-presence footer pinned to the bottom (no top border — blends into the list).
    _convFooter = new ConvFooterWidget(_imgCache, _convPanel);
    convLayout->addWidget(_convFooter);
    connect(_convFooter, &ConvFooterWidget::presenceToggleRequested, this, [this](bool away) {
        if (_session)
            _session->setPresence(away);
    });
    // Zen mode: remembered per workspace; the open chat reloads to match.
    connect(_convFooter, &ConvFooterWidget::zenModeToggled, this, [this](bool on) {
        if (!_session)
            return;
        QSettings("msga", "msga").setValue(zenModeKey(_activeTeamId), on);
        _session->setZenMode(on);
        if (_contentView == ContentView::Conversation && !_currentConvId.value.isEmpty())
            if (const int row = _convList->rowForId(_currentConvId); row >= 0)
                openConversation(row);
    });
    connect(_convFooter, &ConvFooterWidget::manageProfileRequested, this, [this] {
        if (!_session)
            return;
        auto *dlg = new ProfileDialog(_session, _imgCache, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });
    connect(_convFooter, &ConvFooterWidget::manageStatusRequested, this, [this] {
        if (!_session)
            return;
        const QString workspace = recordForHandle(_activeTeamId).displayName;
        auto         *dlg       = new StatusDialog(_session, _imgCache, workspace, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });

    connect(_convList, &ConvListWidget::conversationSelected, this, &MainWindow::openConversation);
    return _convPanel;
}

QWidget *MainWindow::buildRightPanel(QWidget *parent) {
    auto *rightPanel = new QWidget(parent);
    rightPanel->setAttribute(Qt::WA_StyledBackground);
    _rightPanel       = rightPanel; // stylesheet applied (and re-applied) in applyTheme()
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // ── Header bar: avatar, conv name, star, search ───────────────────
    auto *msgHeader = new QWidget(rightPanel);
    msgHeader->setObjectName("msgHeader");
    msgHeader->setAttribute(Qt::WA_StyledBackground);
    const auto &sp = Th::c().spacing;
#ifdef Q_OS_MACOS
    // TitleBar's layout owns the unified header's height.
    auto *headerGrid = new QGridLayout(msgHeader);
    headerGrid->setContentsMargins(0, 0, 0, 1);
    headerGrid->setSpacing(0);
    auto *heading         = new QWidget(msgHeader);
    auto *msgHeaderLayout = new QHBoxLayout(heading);
    msgHeaderLayout->setContentsMargins(0, 0, 0, 0);
    msgHeaderLayout->addStretch(1);
    headerGrid->addWidget(heading, 0, 1);
    headerGrid->setColumnStretch(1, 1);
#else
    msgHeader->setFixedHeight(48);
    auto *msgHeaderLayout = new QHBoxLayout(msgHeader);
    msgHeaderLayout->setContentsMargins(sp.xl, 0, sp.md, 0);
#endif
    msgHeaderLayout->setSpacing(sp.md);

    _headerAvatar = new HeaderAvatarWidget(msgHeader);
    _headerAvatar->setVisible(false);
    msgHeaderLayout->addWidget(_headerAvatar);

    _convNameLabel = new QLabel("", msgHeader);
    _convNameLabel->setObjectName("convNameLabel");
#ifdef Q_OS_MACOS
    _convNameLabel->setAlignment(Qt::AlignCenter);
    _convNameLabel->setMinimumWidth(0);
    _convNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    // Let empty header space and the title drag the native window.
    _convNameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    // The left column mirrors the actions' width so the title stays centred;
    // the actions have room for the members button next to the three icons.
    constexpr int kActionsW = 200;
    auto         *actions   = new QWidget(msgHeader);
    actions->setFixedWidth(kActionsW);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, sp.md, 0);
    actionsLayout->setSpacing(sp.xs);
    actionsLayout->addStretch();
    headerGrid->setColumnMinimumWidth(0, kActionsW);
    headerGrid->addWidget(actions, 0, 2);
    msgHeaderLayout->addWidget(_convNameLabel);
    msgHeaderLayout->addStretch(1);
#else
    auto *actionsLayout = msgHeaderLayout;
    msgHeaderLayout->addWidget(_convNameLabel, 1);
#endif

    // Members button (channels): people icon + member count, opening the member
    // list. A group DM shows its stacked avatars instead, which open the same
    // list (see the _headerAvatar branch of eventFilter).
    _membersBtn = new QPushButton(msgHeader);
    _membersBtn->setObjectName("headerMembersBtn");
    _membersBtn->setFixedHeight(28);
    _membersBtn->setFlat(true);
    _membersBtn->setCursor(Qt::PointingHandCursor);
    _membersBtn->setIconSize(QSize(16, 16));
    _membersBtn->setIcon(svgIcon(":/ui/users.svg", QSize(16, 16), Th::c().icon.def));
    _membersBtn->setVisible(false);
    _membersBtnTooltip = new PopupTooltip(_membersBtn);
    _membersBtn->installEventFilter(this);
    connect(_membersBtn, &QPushButton::clicked, this, [this] {
        openMembersPopup(QRect(_membersBtn->mapToGlobal(QPoint(0, 0)), _membersBtn->size()));
    });
    actionsLayout->addWidget(_membersBtn);
    actionsLayout->addSpacing(sp.xs);
    _headerAvatar->installEventFilter(this);

    // Huddle button — hands off to the Slack web client like the huddle banner's
    // Join (huddles aren't startable through the public API).
    _huddleBtn = new QPushButton(msgHeader);
    _huddleBtn->setFixedSize(28, 28);
    _huddleBtn->setFlat(true);
    _huddleBtn->setCursor(Qt::PointingHandCursor);
    _huddleBtn->setIconSize(QSize(16, 16));
    _huddleBtn->setIcon(svgIcon(":/ui/headphones.svg", QSize(16, 16), Th::c().icon.def));
    _huddleBtnTooltip = new PopupTooltip(_huddleBtn);
    _huddleBtn->installEventFilter(this);
    actionsLayout->addWidget(_huddleBtn);
    actionsLayout->addSpacing(sp.xs);

    _starBtn = new QPushButton(msgHeader);
    _starBtn->setFixedSize(28, 28);
    _starBtn->setFlat(true);
    _starBtn->setCursor(Qt::PointingHandCursor);
    _starBtn->setIconSize(QSize(15, 15));
    _starBtn->setVisible(false);
    _starBtnTooltip = new PopupTooltip(_starBtn);
    _starBtn->installEventFilter(this);
    actionsLayout->addWidget(_starBtn);
    actionsLayout->addSpacing(sp.xs);

    _searchBtn = new QPushButton(msgHeader);
    _searchBtn->setObjectName("headerSearchBtn");
    _searchBtn->setFixedSize(32, 32);
    _searchBtn->setFlat(true);
    _searchBtn->setCursor(Qt::PointingHandCursor);
    _searchBtn->setIconSize(QSize(16, 16));
    _searchBtn->setIcon(svgIcon(":/ui/search.svg", QSize(16, 16), Th::c().icon.def));
    _searchBtnTooltip = new PopupTooltip(_searchBtn);
    _searchBtn->installEventFilter(this);
    actionsLayout->addWidget(_searchBtn);
    _msgHeader = msgHeader;
#ifdef Q_OS_MACOS
    // Settings covers the body, but the unified conversation header sits above
    // that overlay. Block its actions while Settings is open.
    msgHeader->setEnabled(!_settingsDialog->isVisible());
    connect(_settingsDialog, &SettingsDialog::visibilityChanged, msgHeader, &QWidget::setDisabled);
    _titleBar->setContent(msgHeader);
#else
    rightLayout->addWidget(msgHeader);
#endif

    // Messages / canvas tab strip; paints its own bottom divider, replacing
    // the old 1px header divider.
    _convTabs = new ConvTabsWidget(rightPanel);
    _convTabs->hide();
    rightLayout->addWidget(_convTabs);

    // ── Huddle banner — shown when a huddle is live in the open conversation;
    //    Join hands off to the Slack web client (no desktop install needed) ──
    _huddleBanner = new HuddleBanner(rightPanel);
    connect(_huddleBanner, &HuddleBanner::joinClicked, this, [this] {
        if (_currentConvId.value.isEmpty())
            return;
        QDesktopServices::openUrl(QUrl(huddleJoinUrl(_currentConvId)));
    });
    rightLayout->addWidget(_huddleBanner);

    // ── Error banner — shown briefly when a background network error fires ──
    _errorBanner = new QLabel(rightPanel);
    _errorBanner->setObjectName("errorBanner");
    _errorBanner->setAlignment(Qt::AlignCenter);
    _errorBanner->hide();
    rightLayout->addWidget(_errorBanner);

    // ── Parallel-usage banner — persistent, dismissable; shown when the same
    //    app keys run on another device and keep evicting our Socket Mode link ──
    _parallelUsageBanner = new ParallelUsageBanner(rightPanel);
    rightLayout->addWidget(_parallelUsageBanner);

    // ── Content splitter: message area (left) + thread panel (right) ──
    _msgSplitter = new QSplitter(Qt::Horizontal, rightPanel);
    _msgSplitter->setHandleWidth(1);
    _msgSplitter->setChildrenCollapsible(false);
    rightLayout->addWidget(_msgSplitter, 1);

    auto *msgArea   = new QWidget(_msgSplitter);
    _msgArea        = msgArea;
    auto *msgLayout = new QVBoxLayout(msgArea);
    msgLayout->setContentsMargins(0, 0, 0, 0);
    msgLayout->setSpacing(0);

    _contentStack = new QStackedWidget(msgArea);
    msgLayout->addWidget(_contentStack, 1);

    _messageList = new MessageListWidget(nullptr, _imgCache, _contentStack);
    _contentStack->addWidget(_messageList);

    _welcomeTips = new WelcomeWidget(_contentStack);
    _contentStack->addWidget(_welcomeTips);
    _contentStack->setCurrentWidget(_welcomeTips);

    _canvasPage = new CanvasPage(_contentStack);
    _contentStack->addWidget(_canvasPage);

    _threadsPage = new ThreadsPage(_imgCache, _contentStack);
    _contentStack->addWidget(_threadsPage);
    // Leaving the overview for a real thread: open its channel (the same
    // coordinated path as a search result / notification), then the panel.
    connect(
        _threadsPage,
        &ThreadsPage::openThreadRequested,
        this,
        [this](ConversationId conv, Ts root) {
            if (!_convList->selectConversation(conv))
                return;
            if (_currentConvId != conv) {
                const int row = _convList->rowForId(conv);
                if (row >= 0)
                    openConversation(row);
            }
            openThreadPanel(conv, root);
        }
    );
    connect(_threadsPage, &ThreadsPage::openChannelRequested, this, [this](ConversationId conv) {
        _convList->selectConversation(conv);
    });

    _savedPage = new SavedMessagesPage(_imgCache, _contentStack);
    _contentStack->addWidget(_savedPage);
    // A saved-message click jumps to the exact message (same coordinated path
    // as a message-link chip / reminder notification).
    connect(
        _savedPage,
        &SavedMessagesPage::openMessageRequested,
        this,
        [this](ConversationId conv, Ts ts, Ts root) { openMessageTarget(conv, ts, root); }
    );
    connect(
        _savedPage, &SavedMessagesPage::openChannelRequested, this, [this](ConversationId conv) {
            _convList->selectConversation(conv);
        }
    );

    _teammatePage = new TeammatePage(_imgCache, _contentStack);
    _contentStack->addWidget(_teammatePage);
    connect(_teammatePage, &TeammatePage::openSessionRequested, this, [this](ConversationId conv) {
        _convList->selectConversation(conv);
    });
    connect(_teammatePage, &TeammatePage::folderChanged, this, [this] { applyTeammateComposer(); });
    connect(_teammatePage, &TeammatePage::editRequested, this, [this](const QString &id) {
        editTeammate(id);
    });

    // Search is an overlay on msgArea — not a stack page, so it doesn't replace the
    // message list.  Show/hide it; the message list stays loaded beneath it.
    _searchWidget = new SearchWidget(msgArea);
    _searchWidget->hide();
    _contentStack->installEventFilter(this);

    _typingIndicator = new TypingIndicatorWidget(msgArea);
    msgLayout->addWidget(_typingIndicator);

    _composer = new ComposerWidget(msgArea);
    _composer->setEnabled(false);
    _composer->setImageCache(_imgCache);
    msgLayout->addWidget(_composer);

    _msgSplitter->addWidget(msgArea);

    _threadPanel = new ThreadPanel(_imgCache, _msgSplitter);
    _threadPanel->setVisible(false);
    _msgSplitter->addWidget(_threadPanel);
    _msgSplitter->setStretchFactor(0, 1);
    _msgSplitter->setStretchFactor(1, 0);
    // Remember the width the thread panel is dragged to. splitterMoved fires for
    // every pixel of a drag, so write once it settles.
    auto *saveThreadWidth = new QTimer(this);
    saveThreadWidth->setSingleShot(true);
    saveThreadWidth->setInterval(300);
    connect(saveThreadWidth, &QTimer::timeout, this, [this] {
        if (_threadPanel->isVisible() && _threadPanel->width() >= 100)
            QSettings("msga", "msga").setValue("window/threadWidth", _threadPanel->width());
    });
    connect(_msgSplitter, &QSplitter::splitterMoved, saveThreadWidth, qOverload<>(&QTimer::start));

    // ── Signal wiring ─────────────────────────────────────────────────
    auto openSearch = [this] {
        if (_searchWidget->isVisible()) {
            _searchWidget->hide();
        } else {
            repositionSearch();
            _searchWidget->show();
            _searchWidget->raise();
        }
    };
    connect(_searchBtn, &QPushButton::clicked, this, openSearch);
    Ui::Shortcuts::install(Ui::Shortcut::SearchMessages, this, openSearch);
    Ui::Shortcuts::install(Ui::Shortcut::QuickSwitch, this, [this] { openQuickSwitcher(); });
    connect(_searchWidget, &SearchWidget::closeRequested, this, [this] {
        _searchWidget->closeSearch(); // animated
    });
    connect(_searchWidget, &SearchWidget::resultSelected, this, [this](ConversationId conv, Ts ts) {
        if (conv != _currentConvId) {
            // Same coordinated path as a notification open: selectConversation()
            // moves the list highlight and drives openConversation() via the
            // signal, so the header and the selected row don't stay on the old
            // conversation. (It also un-hides a relevance-filtered result.)
            _convList->selectConversation(conv);
            if (_currentConvId != conv) {
                const int row = _convList->rowForId(conv);
                if (row >= 0)
                    openConversation(row);
            }
        }
        // Issued after the open so it outranks the restore-reading-position
        // intent, and survives the switch: the jump re-targets once the freshly
        // opened conversation's history page lands.
        _messageList->jumpToTs(ts);
    });

    connect(
        _messageList,
        &MessageListWidget::threadClicked,
        this,
        [this](ConversationId conv, Ts rootTs) { openThreadPanel(conv, rootTs); }
    );
    // Hide the panel and clear the list's open-thread state. This does NOT
    // collapse an inline expansion: the panel can be opened purely to reply to an
    // inline thread, and closing it (its own ✕) must leave the inline replies in
    // place. The "close both" case is driven the other way — the reply bar's
    // "Close thread" collapses the inline region and then emits
    // threadCloseRequested, so the panel follows the bar, not vice versa.
    auto closeThreadPanel = [this] {
        _threadPanel->close();
        _threadPanel->setVisible(false);
        _messageList->setOpenThreadRoot({});
    };
    connect(_threadPanel, &ThreadPanel::closeRequested, this, closeThreadPanel);
    connect(
        _threadPanel,
        &ThreadPanel::openAsSessionRequested,
        this,
        [this, closeThreadPanel](ConversationId conv, Ts rootTs) {
            if (!_session)
                return;
            const ConversationId session = _session->backend()->openThreadAsSession(conv, rootTs);
            if (session.value.isEmpty())
                return;
            closeThreadPanel();
            // The list picks the new session up from the event just fired.
            QTimer::singleShot(0, this, [this, session] {
                _convList->selectConversation(session);
            });
        }
    );
    connect(_messageList, &MessageListWidget::threadCloseRequested, this, closeThreadPanel);

    // ── Messages / canvas tabs ────────────────────────────────────────
    connect(_convTabs, &ConvTabsWidget::tabSelected, this, [this](ConvTabsWidget::Tab tab) {
        if (_currentConvId.value.isEmpty())
            return;
        if (tab == ConvTabsWidget::Tab::Messages) {
            _canvasPage->flushPendingSave();
            _contentStack->setCurrentWidget(_messageList);
            _composer->show();
        } else {
            _contentStack->setCurrentWidget(_canvasPage);
            _composer->hide();
            _canvasPage->open(_currentConvId, _currentCanvasFileId, _currentCanvasTitle);
        }
    });
    connect(_canvasPage, &CanvasPage::canvasCreated, this, [this](const QString &fileId) {
        _currentCanvasFileId = fileId;
        _convTabs->setCanvasInfo(true, _currentCanvasTitle);
    });
    connect(_canvasPage, &CanvasPage::titleChanged, this, [this](const QString &title) {
        _currentCanvasTitle = title;
        if (!_currentCanvasFileId.isEmpty())
            _convTabs->setCanvasInfo(true, title);
    });
    connect(_canvasPage, &CanvasPage::canvasDeleted, this, [this] {
        _currentCanvasFileId.clear();
        _currentCanvasTitle.clear();
        _convTabs->setCanvasInfo(false);
        _convTabs->setActiveTab(ConvTabsWidget::Tab::Messages);
        _contentStack->setCurrentWidget(_messageList);
        _composer->show();
    });

    // "Message" on the mention-hover profile card → open/create the DM and
    // navigate to it (same path as the People browser).
    connect(_messageList, &MessageListWidget::openDmRequested, this, &MainWindow::openDmWith);
    connect(_threadPanel, &ThreadPanel::openDmRequested, this, &MainWindow::openDmWith);

    // Clicking a #channel mention navigates to that channel — joining it first
    // when not yet a member (same flow as the channel browser).
    const auto openChannelFor = [this](ConversationId conv) {
        if (_convList->selectConversation(conv))
            return;
        if (!_session)
            return;
        _session->joinChannel(
            conv,
            [this](ConversationId joined) { _convList->selectConversation(joined); },
            [this](const QString &err) { showNetworkError(err); }
        );
    };
    connect(_messageList, &MessageListWidget::openChannelRequested, this, openChannelFor);
    connect(_threadPanel, &ThreadPanel::openChannelRequested, this, openChannelFor);

    // Clicking a link to another message jumps to it.
    connect(
        _messageList, &MessageListWidget::messageLinkRequested, this, &MainWindow::openMessageTarget
    );
    connect(_threadPanel, &ThreadPanel::messageLinkRequested, this, &MainWindow::openMessageTarget);

    // Summarize's no-provider notice deep-links to Settings → AI assistance.
    const auto openAiSettings = [this] { _settingsDialog->openAt(SettingsDialog::Page::Ai); };
    connect(_messageList, &MessageListWidget::aiSettingsRequested, this, openAiSettings);
    connect(_threadPanel, &ThreadPanel::aiSettingsRequested, this, openAiSettings);

    connect(
        _messageList,
        &MessageListWidget::editMessageRequested,
        this,
        [this](const Ts &ts, const QString &rawText, const std::vector<File> &files) {
            _composer->enterEditMode(ts, rawText, files);
        }
    );
    connect(
        _messageList,
        &MessageListWidget::forwardMessageRequested,
        this,
        [this](const Message &msg) { forwardMessage(_currentConvId, msg); }
    );
    connect(_threadPanel, &ThreadPanel::forwardMessageRequested, this, &MainWindow::forwardMessage);
    // Channel mode only (the item is never offered inside a thread), so the
    // thread panel's list needs no wiring for this one.
    connect(
        _messageList,
        &MessageListWidget::moveToThreadRequested,
        this,
        &MainWindow::moveMessageToThread
    );

    connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &text) {
        if (teammateViewOpen()) {
            startSessionWithTeammate(text);
            return;
        }
        if (!_session || _currentConvId.value.isEmpty())
            return;
        const Ts ghost =
            _session->sendMessage(_currentConvId, text, std::nullopt, _composer->subjectText());
        _composer->offerUndoSend(_currentConvId, ghost);
    });
    connect(
        _composer,
        &ComposerWidget::commandRequested,
        this,
        [this](const QString &name, const QString &args) {
            // To a teammate, a command is the new session's first prompt.
            if (teammateViewOpen()) {
                startSessionWithTeammate(
                    QLatin1Char('/') + name + (args.isEmpty() ? QString() : QLatin1Char(' ') + args)
                );
                return;
            }
            if (!_session || _currentConvId.value.isEmpty())
                return;
            // A command the app runs itself (an agent session's /status, /clear):
            // nothing is sent.
            if (const auto cmd = _session->findCommand(name); cmd && cmd->local) {
                const auto r = _session->backend()->runLocalCommand(_currentConvId, name, args);
                if (!r.error.isEmpty())
                    showNetworkError(r.error);
                if (!r.status.empty()) {
                    SessionStatusDialog dlg(r.status, this);
                    dlg.exec();
                }
                if (!r.open.value.isEmpty()) {
                    // The list picks the new conversation up from the event just fired.
                    const ConversationId open = r.open;
                    QTimer::singleShot(0, this, [this, open] {
                        _convList->selectConversation(open);
                    });
                }
                return;
            }
            _session->runCommand(_currentConvId, name, args);
        }
    );
    connect(
        _composer,
        &ComposerWidget::uploadRequested,
        this,
        [this](const QStringList &filePaths, const QString &text) {
            if (teammateViewOpen()) {
                startSessionWithTeammate(text, filePaths);
                return;
            }
            if (!_session || _currentConvId.value.isEmpty())
                return;
            const Ts ghost = _session->uploadFiles(_currentConvId, filePaths, text);
            _composer->offerUndoSend(_currentConvId, ghost);
        }
    );
    connect(
        _composer,
        &ComposerWidget::editRequested,
        this,
        [this](const Ts &ts, const QString &newText) {
            if (_session && !_currentConvId.value.isEmpty())
                _session->editMessage(_currentConvId, ts, newText);
        }
    );
    // ↑ in a Claude Code session: its prompt history, as in Claude Code itself.
    _composer->setPromptHistorySource([this] {
        return _session ? _session->promptHistory(_currentConvId) : QStringList();
    });
    connect(_composer, &ComposerWidget::editLastRequested, this, [this] {
        // Only where the backend can edit: Claude Code and email have no edit,
        // so edit mode there would swallow the rewrite into a no-op.
        if (!_session || !_messageList || !_session->capabilities().editMessage)
            return;
        const auto msg = _messageList->lastOwnMessage(_session->meUserId());
        if (!msg)
            return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    connect(_composer, &ComposerWidget::typingStarted, this, [this] {
        if (_session && !_currentConvId.value.isEmpty())
            _session->sendTyping(_currentConvId);
    });
    connect(
        _composer,
        &ComposerWidget::scheduleRequested,
        this,
        [this](const QString &text, qint64 postAt) {
            if (_session && !_currentConvId.value.isEmpty())
                _session->scheduleMessage(_currentConvId, text, postAt);
        }
    );

    // The header title + avatar are updated inside openConversation(), which is
    // the slot wired to conversationSelected (see buildConvPanel) and the single
    // point every conversation open passes through — so no separate signal
    // handler is needed here. Keeping the title update only in the signal path
    // is what used to leave the header stale on programmatic opens (notification
    // click, search result), which never emit conversationSelected.

    connect(_huddleBtn, &QPushButton::clicked, this, [this] {
        if (_currentConvId.value.isEmpty())
            return;
        QDesktopServices::openUrl(QUrl(huddleJoinUrl(_currentConvId)));
    });

    connect(_starBtn, &QPushButton::clicked, this, [this] {
        if (_currentConvId.value.isEmpty() || !_session)
            return;
        const auto *conv       = _session->findConversation(_currentConvId);
        const bool  nowStarred = conv ? !conv->isStarred : true;
        _session->starConversation(_currentConvId, nowStarred);
    });

    return rightPanel;
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void MainWindow::applyTheme() {
    // qApp->setStyleSheet() forces Qt to re-polish (recompute the style of)
    // EVERY widget in the application — ~150 ms on a populated window. The app
    // sheet therefore carries no colors (only the tooltip's shape and font), so
    // it changes only with the font size; the tooltip's theme colors are put on
    // Qt's tooltip label itself by the styler.
    static QString lastGlobalQss;
    const QString  gqss = Th::globalQss();
    if (gqss != lastGlobalQss) {
        lastGlobalQss = gqss;
        qApp->setStyleSheet(gqss);
    }
    Th::installToolTipStyler();

    const auto &th = Th::c();

#ifdef Q_OS_MACOS
    if (_msgHeader)
        Th::setStyleSheetIfChanged(
            _msgHeader,
            QString("QWidget#msgHeader { background: %1; border-bottom: 1px solid %2; }")
                .arg(Th::qss(th.surface.content), Th::qss(th.divider.subtle))
        );
#endif

    // The window backdrop (nav tone + mirrored light content region) is painted
    // by BackdropFrame::paintEvent reading the live theme, so a theme switch just
    // needs a repaint rather than a palette/stylesheet change (which would
    // re-polish the whole subtree). _rightArea no longer needs its own opaque
    // nav.bg fill: the backdrop already paints nav tone behind the resize-handle
    // strip, and the right panel paints its own light surface on top.
    _frame->update();

    // The content-surface fill behind the whole right side — the chat header,
    // the area around the composer, the welcome page. The `QWidget` selector
    // deliberately cascades to unstyled descendants; anything with its own
    // stylesheet (banners…) still wins. Must be re-applied here: set only at
    // build time it kept the previous theme's surface after a light↔dark
    // switch.
    if (_rightPanel) {
        Th::setStyleSheetIfChanged(
            _rightPanel, QString("QWidget { background: %1; }").arg(Th::qss(th.surface.content))
        );
    }

    // Same one-time-style trap as the right panel, for the pre-login screen.
    if (_loggedOutPage) {
        Th::setStyleSheetIfChanged(
            _loggedOutPage,
            QString("QWidget#loggedOutWrapper { background: %1; }").arg(Th::qss(th.nav.bg))
        );
        if (auto *page = _loggedOutPage->findChild<QWidget *>("loggedOutPage")) {
            Th::setStyleSheetIfChanged(
                page, QString("QWidget { background: %1; }").arg(Th::qss(th.surface.content))
            );
        }
    }

    // The header uses the same content surface on both platforms, keeping the
    // macOS overlay consistent with the right-panel header elsewhere.
    if (_convNameLabel) {
#ifdef Q_OS_MACOS
        const int titleFontSize = th.fonts.xl;
#else
        const int titleFontSize = th.fonts.xxl;
#endif
        Th::setStyleSheetIfChanged(
            _convNameLabel,
            QString("font-weight: 600; font-size: %1px; color: %2;")
                .arg(titleFontSize)
                .arg(Th::qss(th.text.primary))
        );
    }
    if (_membersBtn) {
        Th::setStyleSheetIfChanged(
            _membersBtn,
            QString(
                "QPushButton { border: none; background: transparent;"
                "  padding: 0 %1px; color: %2; font-size: %3px; }"
            )
                .arg(th.spacing.sm)
                .arg(Th::qss(th.text.secondary))
                .arg(th.fonts.sm)
        );
        _membersBtn->setIcon(svgIcon(":/ui/users.svg", QSize(16, 16), th.icon.def));
    }
    if (_huddleBtn) {
        Th::setStyleSheetIfChanged(
            _huddleBtn, "QPushButton { border: none; background: transparent; }"
        );
        _huddleBtn->setIcon(svgIcon(":/ui/headphones.svg", QSize(16, 16), th.icon.def));
    }
    if (_starBtn) {
        Th::setStyleSheetIfChanged(
            _starBtn, "QPushButton { border: none; background: transparent; }"
        );
    }
    if (_searchBtn) {
        Th::setStyleSheetIfChanged(
            _searchBtn, "QPushButton { border: none; background: transparent; }"
        );
        _searchBtn->setIcon(svgIcon(":/ui/search.svg", QSize(16, 16), th.icon.def));
    }
    if (_errorBanner) {
        Th::setStyleSheetIfChanged(
            _errorBanner,
            QString(
                "QLabel#errorBanner {"
                "  background: %1;"
                "  color: %2;"
                "  padding: 6px 12px;"
                "  font-size: %3px;"
                "}"
            )
                .arg(Th::qss(th.danger.icon), Th::qss(th.surface.raised))
                .arg(th.fonts.md)
        );
    }
}

// ── Session lifecycle ─────────────────────────────────────────────────────────

Session *MainWindow::ensureSession(const QString &teamId) {
    auto it = _sessions.find(teamId);
    if (it != _sessions.end())
        return it->second.session.get();

    // teamId is the WorkspaceKey handle. The factory builds the right backend
    // for the service encoded in the handle; any per-service shared resource
    // (e.g. the Slack Socket Mode socket) is created lazily by that backend.
    const auto key = WorkspaceKey::fromString(teamId);
    if (!key)
        return nullptr;
    const auto rec = TokenStore::loadWorkspace(*key);
    if (!rec)
        return nullptr;
    auto backend = makeBackend(*rec);
    if (!backend)
        return nullptr;

    auto &entry      = _sessions[teamId];
    entry.session    = std::make_unique<Session>(std::move(backend), teamId);
    Session *session = entry.session.get();
    if (session->capabilities().zenMode)
        session->setZenMode(QSettings("msga", "msga").value(zenModeKey(teamId), false).toBool());

    // Background subscriptions — alive for the whole session, active
    // workspace or not, so badges and notifications never depend on what's
    // on screen.
    session->conversations() |
        rpl::on_next(
            [this, teamId](std::vector<Conversation> convs) { updateUnreadBadges(teamId, convs); },
            entry.lifetime
        );

    session->events() | rpl::on_next(
                            [this, teamId](Event e) {
                                if (const auto *ev = std::get_if<EvMessageNew>(&e))
                                    maybeNotify(teamId, *ev);
                                else if (const auto *hv = std::get_if<EvHuddleChanged>(&e))
                                    maybeNotifyHuddle(teamId, *hv);
                                else if (const auto *rd = std::get_if<EvReminderDue>(&e))
                                    notifyReminderDue(teamId, *rd);
                            },
                            entry.lifetime
                        );

    // If the backend can't refresh the token (no refresh token, or refresh
    // fails), it sets authState → NotLoggedIn. Deferred: this fires from
    // inside the session's own rpl chain, and dropSession destroys it.
    session->authState() | rpl::on_next(
                               [this, teamId](AuthState state) {
                                   if (state != AuthState::NotLoggedIn)
                                       return;
                                   QMetaObject::invokeMethod(
                                       this,
                                       [this, teamId] {
                                           // Before the session goes: a user who
                                           // parked the app in the tray would
                                           // otherwise never learn it stopped
                                           // receiving messages.
                                           notifySessionExpired(teamId);
                                           dropSession(teamId);
                                           if (teamId == _activeTeamId || _activeTeamId.isEmpty())
                                               showLoggedOut();
                                       },
                                       Qt::QueuedConnection
                                   );
                               },
                               entry.lifetime
                           );

    // start() loads cache into _conversations/_users, so the subscriptions
    // above fire immediately with cached data (badges show before the
    // network responds).
    session->start();
    return session;
}

void MainWindow::ensureSessionsSequentially(QStringList pending) {
    if (pending.isEmpty())
        return;
    QTimer::singleShot(100, this, [this, pending]() mutable {
        const QString teamId = pending.takeFirst();
        // No-op if the user already activated it meanwhile (ensureSession
        // returns the existing entry) or logged it out (TokenStore lookup
        // inside returns nothing).
        ensureSession(teamId);
        ensureSessionsSequentially(std::move(pending));
    });
}

void MainWindow::dropSession(QString teamId) {
    auto it = _sessions.find(teamId);
    if (it == _sessions.end())
        return;
    if (_forwardDialog && _forwardDialog->usesSession(it->second.session.get()))
        _forwardDialog->reject();
    if (it->second.session.get() == _session) {
        // The composers may still hold input for this workspace; the workspace
        // is going away, so take it and let it fall on the floor. The thread
        // panel is also detached here — its Session pointer would dangle the
        // moment the erase below destroys the session.
        if (_composer)
            _composer->takeDraft();
        if (_threadPanel) {
            _threadPanel->close();
            _threadPanel->setVisible(false);
            _threadPanel->setSession(nullptr);
        }
        _uiLifetime = rpl::lifetime();
        _session    = nullptr;
        if (_messageList)
            _messageList->setSession(nullptr);
        _currentConvId  = {};
        _pendingNavConv = {};
    }
    // A logged-out workspace's drafts must not linger: the same team could be
    // added again later (possibly by someone else's account).
    const QString prefix = draftKey(teamId, ConversationId{});
    _drafts.removeIf([&prefix](const auto &it) { return it.key().startsWith(prefix); });
    if (_threadPanel)
        _threadPanel->purgeDrafts(teamId);
    _navHistory.purgeTeam(teamId);
    _sessions.erase(it);
    _wsUnreads.remove(teamId);
    if (_switcher)
        _switcher->setUnreadCounts(teamId, 0, 0);
    updateTrayIcon();
}

void MainWindow::activateWorkspace(QString teamId) {
    if (teamId.isEmpty()) {
        showLoggedOut();
        return;
    }
    // A workspace that can't be opened (listed, but its record is missing or
    // unreadable, or its service isn't built in) must never leave the window
    // without a session: stay in the open one, else go to another, else to
    // the sign-in page. Checked before anything of the open one is torn down.
    if (!ensureSession(teamId)) {
        qWarning("workspace %s can't be opened", qPrintable(teamId));
        if (_session)
            return;
        for (const auto &key : TokenStore::workspaceKeys())
            if (key.toString() != teamId && ensureSession(key.toString())) {
                activateWorkspace(key.toString());
                return;
            }
        showLoggedOut();
        return;
    }

    // Build the main page once, lazily
    if (!_mainPage) {
        _mainPage = buildMainPage();
        _stack->addWidget(_mainPage);
        applyTheme();
        // _rightPanelLayout is born here with zero margins. The earlier
        // updateRoundedMask() (constructor / window show) ran while it was still
        // null and skipped it, so the windowed nav.bg border depends on a resize
        // event landing *after* this point — which doesn't happen when the page
        // is built after the window is already shown (login, async activation).
        // Re-run it now so the border is correct regardless of resize timing.
        updateRoundedMask();
    }

    // Remember the outgoing chat's scroll position now, while the message list is
    // still laid out normally. The widget hides below grow its viewport, which
    // would clamp a slightly-scrolled-up position to a false "at bottom".
    if (_messageList)
        _messageList->saveScrollAnchor();

    // Stash the outgoing workspace's unsent composer input while _activeTeamId
    // and _currentConvId still name it — switching workspaces must neither lose
    // a draft nor carry it (or its attachments) into the incoming workspace.
    stashComposerDraft();

    // Detach the UI from the outgoing session — it stays alive in the
    // background and keeps accumulating unreads / firing notifications.
    _uiLifetime = rpl::lifetime();
    if (_session) {
        // No conversation is open in this session's UI anymore — stop both the
        // mark-read cursor and the realtime safety poll for it. (A mere window
        // blur clears only setReading; here the whole session leaves the screen.)
        _session->setOpenConversation({});
        _session->setReading({});
        // Debounced: the conv-list serialization + file write must not block the
        // switch. A pending save is flushed on drop / shutdown / destruction.
        _session->scheduleSaveUnreads();
    }
    _currentConvId = {};
    _contentView   = ContentView::None;
    if (_convFooter)
        _convFooter->clear();
    if (_searchWidget)
        _searchWidget->hide();
    _composer->setEnabled(false);
    _composer->hide();
    if (_msgHeader)
        _msgHeader->hide();
    if (_convTabs)
        _convTabs->hide();
    if (_huddleBanner)
        _huddleBanner->hide();
    if (_contentStack && _messageList)
        _contentStack->setCurrentWidget(_messageList);

    _session = ensureSession(teamId);
    _messageList->setSession(_session);
    if (_convList)
        _convList->setSession(_session);
    if (_searchWidget)
        _searchWidget->setSession(_session);
    if (_composer)
        _composer->setSession(_session);
    if (_threadPanel) {
        _threadPanel->setSession(_session);
        _threadPanel->close();
        _threadPanel->setVisible(false);
    }
    if (_canvasPage) {
        _canvasPage->flushPendingSave();
        _canvasPage->setSession(_session);
        _canvasPage->clear();
    }
    if (_threadsPage)
        _threadsPage->setSession(_session); // also drops the old workspace's cards
    if (_savedPage)
        _savedPage->setSession(_session);
    if (_teammatePage)
        _teammatePage->setSession(_session);
    _currentCanvasFileId.clear();
    _currentCanvasTitle.clear();
    if (_convTabs)
        _convTabs->setCanvasInfo(false);

    _activeTeamId = teamId;
    if (const auto key = WorkspaceKey::fromString(teamId))
        TokenStore::setActiveWorkspace(*key);

    // Update switcher + title bar
    refreshSwitcher();
    if (_titleBar)
        _titleBar->setTitle(recordForHandle(teamId).displayName);

    // First load with no cache: hide the conv column and show a spinner in the
    // message area until conversations arrive from the network.
    if (_convPanel && _messageList) {
        const bool hasCached = !_session->currentConversations().empty();
        _convPanel->setVisible(hasCached);
        if (_convResizeHandle)
            _convResizeHandle->setVisible(hasCached);
        _messageList->setWaiting(!hasCached);
    }

    connectToSession();
    _stack->setCurrentWidget(_mainPage);
}

void MainWindow::switchToWorkspace(QString teamId) {
    if (teamId == _activeTeamId)
        return;
    // A manual switch cancels any in-flight back/forward jump target.
    _pendingNavConv = {};
    activateWorkspace(std::move(teamId));
}

void MainWindow::showLoggedOut() {
    // Detach the UI only — background sessions for other workspaces (if any)
    // keep running and keep their badges/notifications.
    stashComposerDraft();
    _uiLifetime = rpl::lifetime();
    _session    = nullptr;
    if (_messageList)
        _messageList->setSession(nullptr);
    if (_convFooter)
        _convFooter->clear();
    _activeTeamId.clear();
    _currentConvId = {};
    _contentView   = ContentView::None;
    updateTrayIcon();
    if (_titleBar)
        _titleBar->setTitle({});
    if (_msgHeader)
        _msgHeader->hide();
    refreshSwitcher(); // switcher is always visible — deselect the active entry
    _stack->setCurrentWidget(_loggedOutPage);
}

void MainWindow::promptAddWorkspace(const QPoint &anchorGlobal) {
    const auto services = auth::registeredAuthServices();
    if (services.empty())
        return;
    // A single-workspace service (Claude Code) that's already connected can't be
    // added again: its row is greyed out.
    const auto keys         = TokenStore::workspaceKeys();
    const auto alreadyAdded = [&keys](const Service &s) {
        const auto *d = backends::find(s);
        if (!d || !d->singleWorkspace)
            return false;
        for (const auto &k : keys)
            if (k.service == s)
                return true;
        return false;
    };
    // One service → no point in a menu; start it directly.
    if (services.size() == 1) {
        if (alreadyAdded(services.front()))
            return;
        if (services.front() == slack::kService)
            connectSlack();
        else
            loginWithService(services.front());
        return;
    }

    // Several services → our themed ContextMenu anchored at the add button, one
    // row per service. Defer the actual login to the next event-loop turn so the
    // popup finishes dismissing before a browser opens.
    auto *menu = new ContextMenu(this);
    menu->setWidthMode(ContextMenu::WidthMode::MinWidth);
    for (const Service s : services) {
        if (alreadyAdded(s)) {
            menu->addDisabledItem(backends::displayName(s));
            continue;
        }
        menu->addItem(backends::displayName(s), [this, s] {
            QTimer::singleShot(0, this, [this, s] {
                if (s == slack::kService)
                    connectSlack();
                else
                    loginWithService(s);
            });
        });
    }
    menu->popup(anchorGlobal);
}

void MainWindow::connectSlack() {
    // Session is the default Slack connection method: open the import dialog first.
    // Its secondary "use app keys" escape falls back to the OAuth flow.
    auto *dlg = new SessionImportDialog(this);
    connect(
        dlg,
        &SessionImportDialog::imported,
        this,
        [this](const QList<TokenStore::WorkspaceRecord> &recs) { addSessionWorkspaces(recs); }
    );
    connect(dlg, &SessionImportDialog::useAppKeysRequested, this, [this] {
        loginWithService(slack::kService);
    });
    connect(dlg, &AppDialog::finished, dlg, [dlg](int) { dlg->deleteLater(); });
    dlg->open();
}

void MainWindow::migrateSlackToSession() {
    // The `d` cookie is per-account, so reuse the one from any session workspace.
    QString                                      cookie;
    QList<slack::session::SessionMigrator::Item> items;
    for (const auto &key : TokenStore::workspaceKeys()) {
        if (key.service != slack::kService)
            continue;
        const auto rec = TokenStore::loadWorkspace(key);
        if (!rec)
            continue;
        const auto creds = slack::fromRecord(*rec);
        if (!creds.cookie.isEmpty()) {
            if (cookie.isEmpty())
                cookie = creds.cookie;
        } else {
            items.append({creds.teamId, creds.teamName, creds.iconUrl, creds.xoxp});
        }
    }
    if (cookie.isEmpty()) {
        QMessageBox::information(
            this,
            tr("Convert to session"),
            tr("Add one workspace with your Slack session first — its cookie is reused for the "
               "rest.")
        );
        return;
    }
    if (items.isEmpty()) {
        QMessageBox::information(
            this, tr("Convert to session"), tr("All Slack workspaces already use your session.")
        );
        return;
    }
    auto *mig = new slack::session::SessionMigrator(this);
    connect(
        mig,
        &slack::session::SessionMigrator::finished,
        this,
        [this, mig](const QList<slack::Credentials> &converted, const QString &error) {
            mig->deleteLater();
            if (converted.isEmpty()) {
                QMessageBox::warning(
                    this,
                    tr("Convert to session"),
                    tr("Couldn't convert your workspaces: %1").arg(error)
                );
                return;
            }
            for (const auto &c : converted)
                TokenStore::saveWorkspace(slack::toRecord(c));
            slack::setConnectionMode(slack::ConnectionMode::Session);
            // Restart to drop Socket Mode and rebuild every backend in session mode.
            restartApp();
        }
    );
    mig->run(cookie, items);
}

void MainWindow::addSessionWorkspaces(const QList<TokenStore::WorkspaceRecord> &recs) {
    if (recs.isEmpty())
        return;
    // Connecting via session pins the app into session mode (no app keys / Socket
    // Mode) — the "mode follows how you connect" rule that keeps the Settings
    // switch honest.
    slack::setConnectionMode(slack::ConnectionMode::Session);
    for (const auto &rec : recs)
        TokenStore::saveWorkspace(rec);
    _activeTeamId = recs.first().key.toString();

    // The account-wide `d` cookie rotates on each browser re-login, so importing a
    // fresh one stales the OTHER session workspaces' stored token+cookie (this is
    // why adding a workspace disconnected the previous one). Re-mint each other
    // session workspace's token against the new cookie using its stored URL, then
    // restart so running backends reload fresh creds. Workspaces with no stored
    // URL (added before URLs were persisted) can't be auto-healed — re-import once.
    const QString                      newCookie = slack::fromRecord(recs.first()).cookie;
    QList<slack::session::TeamSession> stale;
    if (!newCookie.isEmpty()) {
        QSet<QString> justAdded;
        for (const auto &rec : recs)
            justAdded.insert(rec.key.id);
        for (const auto &key : TokenStore::workspaceKeys()) {
            if (key.service != slack::kService || justAdded.contains(key.id))
                continue;
            const auto rec = TokenStore::loadWorkspace(key);
            if (!rec)
                continue;
            const auto c = slack::fromRecord(*rec);
            if (c.cookie.isEmpty() || c.workspaceUrl.isEmpty() || c.cookie == newCookie)
                continue; // OAuth, no URL to re-derive from, or already fresh
            slack::session::TeamSession t;
            t.workspaceUrl = c.workspaceUrl;
            t.teamId       = c.teamId;
            t.teamName     = c.teamName;
            t.iconUrl      = c.iconUrl;
            stale.append(t);
        }
    }
    if (stale.isEmpty()) {
        activateWorkspace(_activeTeamId);
        return;
    }
    if (const auto key = WorkspaceKey::fromString(_activeTeamId))
        TokenStore::setActiveWorkspace(*key); // persist so the restart lands here
    auto *deriver = new slack::session::TokenDeriver(this);
    connect(
        deriver,
        &slack::session::TokenDeriver::finished,
        this,
        [this, deriver](const QList<slack::Credentials> &valid, const QString &) {
            deriver->deleteLater();
            for (const auto &c : valid)
                TokenStore::saveWorkspace(slack::toRecord(c));
            restartApp(); // reload every backend with refreshed session creds
        }
    );
    deriver->run(newCookie, stale);
}

void MainWindow::loginWithService(Service service) {
    // The auth strategy is neutral: it runs the service's own flow (Slack's
    // OAuth, Teams' Auth Code + PKCE, …) and hands back a ready-to-store
    // WorkspaceRecord. MainWindow never sees a service-specific credential type.
    auto strategy = auth::makeAuthStrategy(service, this);
    if (!strategy) {
        QMessageBox::critical(this, tr("Login failed"), tr("This service is not supported."));
        return;
    }

    // Keep the strategy alive across the async flow (parented to this); the
    // OAuth callback reaches it via handleOAuthUri → _activeFlow.
    auto *s     = strategy.release();
    _activeFlow = s;

    connect(
        s,
        &auth::AuthStrategy::succeeded,
        this,
        [this, s, service](TokenStore::WorkspaceRecord rec) {
            // OAuth sign-in ⇒ app-keys mode (Socket Mode on) — mode follows how you connect.
            if (service == slack::kService)
                slack::setConnectionMode(slack::ConnectionMode::AppKeys);
            TokenStore::saveWorkspace(rec);
            _activeTeamId = rec.key.toString();
            _activeFlow   = nullptr;
            s->deleteLater();
            activateWorkspace(_activeTeamId);
        }
    );
    connect(s, &auth::AuthStrategy::failed, this, [this, s](const QString &reason) {
        _activeFlow = nullptr;
        s->deleteLater();
        // A user-initiated cancel is not an error — close silently.
        if (reason != QLatin1String("cancelled"))
            QMessageBox::critical(this, tr("Login failed"), reason);
    });

    s->start();
}

void MainWindow::handleOAuthUri(const QUrl &uri) {
    if (_activeFlow)
        _activeFlow->handleCallbackUri(uri);
}

// How the user's own presence reads to others — shown on the self-DM header avatar.
static QString selfPresenceTooltip(const SelfPresence &sp) {
    if (sp.phantomAway())
        return QCoreApplication::translate(
            "MainWindow", "You appear away to others — no official Slack client is connected"
        );
    if (sp.active)
        return QCoreApplication::translate("MainWindow", "Active");
    if (sp.loaded)
        return QCoreApplication::translate("MainWindow", "Away");
    return {};
}

void MainWindow::wireConvList() {
    // Qt signal connections survive workspace switches (the lambdas read
    // _session at invoke time), so wire them exactly once — re-connecting on
    // every switch would fire each handler N times.
    if (_convListWired || !_convList)
        return;
    _convListWired = true;

    connect(
        _convList,
        &ConvListWidget::starConversationRequested,
        this,
        [this](ConversationId id, bool star) {
            if (_session)
                _session->starConversation(id, star);
        }
    );
    connect(
        _convList, &ConvListWidget::leaveConversationRequested, this, [this](ConversationId id) {
            if (!_session)
                return;
            // A Claude Code session removed while open: show the one below it
            // (the one above when it was the last) rather than a chat that is
            // gone. Picked now, while the removed row is still in the list.
            const ConversationId next =
                (id == _currentConvId && _session->capabilities().agentSessions)
                    ? _convList->neighbourConversationId(id)
                    : ConversationId{};
            _session->leaveConversation(id);
            if (!next.value.isEmpty())
                QTimer::singleShot(0, this, [this, id, next] {
                    if (_currentConvId == id)
                        openConversationIn({}, next); // focuses the composer too
                });
        }
    );
    connect(_convList, &ConvListWidget::joinHuddleRequested, this, [this](ConversationId id) {
        QDesktopServices::openUrl(QUrl(huddleJoinUrl(id)));
    });
    connect(
        _convList,
        &ConvListWidget::setNotificationLevelRequested,
        this,
        [this](ConversationId id, NotificationLevel level) {
            if (_session)
                _session->setNotificationLevel(id, level);
        }
    );
    connect(
        _convList,
        &ConvListWidget::muteConversationRequested,
        this,
        [this](ConversationId id, bool muted) {
            if (_session)
                _session->setConvMuted(id, muted);
        }
    );
    connect(
        _convList, &ConvListWidget::renameConversationRequested, this, [this](ConversationId id) {
            renameConversation(id);
        }
    );
    connect(_convList, &ConvListWidget::stopSessionRequested, this, [this](ConversationId id) {
        if (_session)
            _session->backend()->stopAgentSession(id);
    });
    connect(_convList, &ConvListWidget::threadsViewRequested, this, [this] { openThreadsView(); });
    connect(_convList, &ConvListWidget::savedMessagesRequested, this, [this] {
        openSavedMessagesView();
    });
    connect(_convList, &ConvListWidget::teammateSelected, this, [this](const QString &role) {
        openTeammateView(role);
    });
    connect(_convList, &ConvListWidget::addTeammateRequested, this, [this] {
        QTimer::singleShot(0, this, [this] { editTeammate({}); });
    });
    connect(_convList, &ConvListWidget::editTeammateRequested, this, [this](const QString &id) {
        QTimer::singleShot(0, this, [this, id] { editTeammate(id); });
    });
    connect(_convList, &ConvListWidget::restoreTeammateRequested, this, [this](const QString &id) {
        if (_session) {
            _session->backend()->restoreAgentRole(id);
            refreshTeammates();
        }
    });
    connect(_convList, &ConvListWidget::removeTeammateRequested, this, [this](const QString &id) {
        QTimer::singleShot(0, this, [this, id] { removeTeammate(id); });
    });
    connect(_convList, &ConvListWidget::findChannelRequested, this, [this] {
        openBrowseDialog(0);
    });
    connect(_convList, &ConvListWidget::browsePeopleRequested, this, [this] {
        openBrowseDialog(1);
    });
    // The "+" on an agent workspace's Sessions header: its "Add channels".
    connect(_convList, &ConvListWidget::agentSessionMenuRequested, this, [this](QPoint at) {
        if (!_session || !_session->capabilities().agentSessions)
            return;
        auto *menu = new ContextMenu(this);
        menu->setWidthMode(ContextMenu::WidthMode::MinWidth);
        menu->addItem(tr("Find a session"), [this] {
            QTimer::singleShot(0, this, [this] { openSessionFinder(); });
        });
        menu->addItem(tr("Create a session"), [this] {
            QTimer::singleShot(0, this, [this] { startAgentSession(false); });
        });
        menu->addItem(tr("Create an unsafe session"), [this] {
            QTimer::singleShot(0, this, [this] { startAgentSession(true); });
        });
        menu->popup(at);
    });
    connect(_convList, &ConvListWidget::createChannelRequested, this, [this] {
        if (!_session)
            return;
        auto *dlg = new CreateChannelDialog(recordForHandle(_activeTeamId).displayName, this);
        if (dlg->exec() == QDialog::Accepted) {
            const QString name = dlg->channelName();
            const bool    priv = dlg->isPrivate();
            _session->createChannel(name, priv, {}, [this](const QString &err) {
                showNetworkError(err);
            });
        }
        dlg->deleteLater();
    });
}

void MainWindow::renameConversation(ConversationId id) {
    if (!_session || !_convList)
        return;
    const auto *conv = _session->findConversation(id);
    // A Claude Code session is a DM whose backend applies the name (to the
    // session's user, which titles it everywhere); its own name is conv->name.
    const bool  agentSession =
        conv && conv->kind == ConvKind::Im && _session->capabilities().agentSessions;
    if (!conv || (conv->kind != ConvKind::Mpim && !agentSession))
        return;
    // Placeholder = what the list falls back to without an alias.
    Conversation bare = *conv;
    bare.localName.clear();
    const QString derived = agentSession ? conv->name : _convList->resolvedConvName(bare);

    RenameConversationDialog dlg(
        conv->localName,
        derived,
        this,
        agentSession ? RenameConversationDialog::Kind::AgentSession
                     : RenameConversationDialog::Kind::GroupDm
    );
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.name();
    // The dialog's nested loop may have outlived the conversation (a reload
    // reassigns the roster) — re-find rather than reuse `conv`.
    if (!_session->findConversation(id))
        return;
    _session->setConvLocalName(id, name);

    // The chats list repaints off the roster change; the header and the composer
    // placeholder were set when the chat was opened, so re-aim them now.
    if (id != _currentConvId)
        return;
    const int row = _convList->rowForId(id);
    if (row < 0)
        return;
    const QString title = _convList->resolvedName(row);
    if (_convNameLabel)
        _convNameLabel->setText(title);
    if (_composer && _composerLockReason.isEmpty()) // a locked one shows why instead
        _composer->setPlaceholderText(
            title.isEmpty() ? tr("Message") : tr("Message %1").arg(title)
        );
    updateHeaderForConv(id);
}

// "+" in an agent workspace: pick the directory the new session works in, and
// open its conversation (already listed when onSuccess runs). The session
// itself starts with the first message sent there.
void MainWindow::startAgentSession(bool skipPermissionChecks) {
    if (!_session)
        return;
    QSettings     s("msga", "msga");
    const QString last  = s.value("claudeCode/lastDir", QDir::homePath()).toString();
    const QString title = skipPermissionChecks
                              ? tr("Start session in a directory without permission checks…")
                              : tr("Start session in a directory…");
    const QString dir   = Ui::getExistingDirectory(this, title, last);
    if (dir.isEmpty() || !_session)
        return;
    s.setValue("claudeCode/lastDir", dir);
    _session->startAgentSession(
        dir,
        skipPermissionChecks,
        {}, // the generalist; a specialist is written to on its page
        [this](ConversationId id) {
            if (_convList && _convList->selectConversation(id))
                focusComposerIfActive();
        },
        [this](const QString &err) { showNetworkError(err); }
    );
}

// A conversation nobody can post to from here right now (Conversation::
// readOnlyReason — e.g. a Claude Code session a terminal is driving) gets a
// locked composer that says why. Re-run on every roster change, since the state
// flips while the chat is open (the terminal session ends → it can be continued).
void MainWindow::applyComposerAccess() {
    if (!_composer || !_session || _currentConvId.value.isEmpty())
        return;
    const auto   *conv       = _session->findConversation(_currentConvId);
    const QString reason     = conv ? conv->readOnlyReason : QString();
    // Handed on only when it changes: a send drops it from the composer, and
    // the roster may still carry it until Claude Code starts the next turn.
    const QString suggestion = conv ? conv->suggestedReply : QString();
    if (suggestion != _composerSuggestion) {
        _composerSuggestion = suggestion;
        _composer->setSuggestion(suggestion);
    }
    if (reason == _composerLockReason)
        return;
    _composerLockReason = reason;
    _composer->setEnabled(reason.isEmpty());
    if (!reason.isEmpty()) {
        _composer->setPlaceholderText(reason);
        return;
    }
    const int     row   = _convList ? _convList->rowForId(_currentConvId) : -1;
    const QString title = row >= 0 ? _convList->resolvedName(row) : QString();
    _composer->setPlaceholderText(title.isEmpty() ? tr("Message") : tr("Message %1").arg(title));
}

void MainWindow::openSessionFinder() {
    if (!_session)
        return;
    auto *dlg = new SessionFinderDialog(_imgCache, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<SessionFinderDialog> alive(dlg);
    _session->backend()->findAgentSessions([alive](std::vector<FoundSession> sessions) {
        if (alive)
            alive->setSessions(std::move(sessions));
    });
    connect(dlg, &SessionFinderDialog::sessionActivated, this, [this](const QString &id) {
        if (!_session)
            return;
        const ConversationId conv = _session->backend()->addFoundSession(id);
        if (conv.value.isEmpty()) {
            showNetworkError(tr("That session is gone: Claude Code no longer has it."));
            return;
        }
        // Listed by the backend's announcement; select once that has landed.
        QTimer::singleShot(0, this, [this, conv] { _convList->selectConversation(conv); });
    });
    connect(dlg, &SessionFinderDialog::createSessionRequested, this, [this] {
        QTimer::singleShot(0, this, [this] { startAgentSession(false); });
    });
    dlg->exec();
}

void MainWindow::openBrowseDialog(int initialTab) {
    if (!_session)
        return;
    auto *dlg = new BrowseChannelsDialog(
        _session->currentConversations(), _session->currentUsers(), _imgCache, this
    );
    if (initialTab == 1)
        dlg->showPeopleTab();
    connect(dlg, &BrowseChannelsDialog::createChannelRequested, this, [this] {
        if (!_session)
            return;
        auto *cdlg = new CreateChannelDialog(recordForHandle(_activeTeamId).displayName, this);
        if (cdlg->exec() == QDialog::Accepted) {
            _session->createChannel(
                cdlg->channelName(), cdlg->isPrivate(), {}, [this](const QString &err) {
                    showNetworkError(err);
                }
            );
        }
        cdlg->deleteLater();
    });
    connect(dlg, &BrowseChannelsDialog::channelActivated, this, [this](ConversationId id) {
        // Already a member: just navigate (even if hidden by the relevance filter)
        if (_convList->selectConversation(id))
            return;
        // Not a member: join first, then navigate when conv list updates
        if (!_session)
            return;
        _session->joinChannel(
            id,
            [this](ConversationId joined) { _convList->selectConversation(joined); },
            [this](const QString &err) { showNetworkError(err); }
        );
    });
    connect(dlg, &BrowseChannelsDialog::userActivated, this, [this](UserId id) {
        if (!_session)
            return;
        // Open the existing DM — creating it via conversations.open if the user
        // has never been messaged — and select it even if currently hidden.
        _session->openDm(
            id,
            [this](ConversationId conv) { _convList->selectConversation(conv); },
            [this](const QString &err) { showNetworkError(err); }
        );
    });
    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::openQuickSwitcher() {
    if (!_session || !_convList)
        return;
    // Not on top of another modal: the switcher navigates the window behind the
    // backdrop, which would leave that dialog stranded over a chat it was never
    // opened from.
    if (AppDialog::topmostVisible(this))
        return;

    // One tab per live workspace, in the rail's order. The active one takes its
    // names from the sidebar (freshest visit stamps, the exact filter it
    // applies); a background one has only its Session, so it is resolved from
    // that. A workspace whose Session hasn't been brought up yet (first seconds
    // after launch) has nothing to list and gets no tab.
    std::vector<QuickSwitcherDialog::Workspace> workspaces;
    const QStringList order = _switcher ? _switcher->workspaceIds() : QStringList{_activeTeamId};
    for (const QString &teamId : order) {
        const auto it = _sessions.find(teamId);
        if (it == _sessions.end() || !it->second.session)
            continue;
        const auto rec = recordForHandle(teamId);
        workspaces.push_back(
            {.teamId  = teamId,
             .name    = rec.displayName,
             .iconUrl = TokenStore::displayIconUrl(rec),
             .conversations =
                 teamId == _activeTeamId
                     ? _convList->namedConversations()
                     : namedConversationsFor(it->second.session.get(), _convList->visitedAt())}
        );
    }
    if (workspaces.empty())
        return;

    auto *dlg = new QuickSwitcherDialog(std::move(workspaces), _activeTeamId, _imgCache, this);
    connect(
        dlg,
        &QuickSwitcherDialog::conversationActivated,
        this,
        [this](const QString &teamId, ConversationId id) {
            // Same path as a notification open: switches workspace if needed,
            // moves the list highlight and drives openConversation().
            openConversationIn(teamId, id);
        }
    );
    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::connectToSession() {
    wireConvList();

    // The footer's visible/hidden presence toggle only makes sense on a backend
    // with a presence concept (Slack); IMAP/email has none, so drop the control.
    if (_convFooter) {
        _convFooter->setPresenceSupported(
            _session->capabilities().presence && _session->capabilities().selfPresence
        );
        _convFooter->setStatusSupported(_session->capabilities().selfStatus);
        _convFooter->setZenMode(
            _session->capabilities().zenMode,
            QSettings("msga", "msga").value(zenModeKey(_activeTeamId), false).toBool()
        );
    }

    // The "Threads" roster entry needs a workspace-wide threads feed. Its unread
    // face follows the session's count of followed threads with unread replies
    // (the producer replays the current value on subscribe, so a switch back to
    // a workspace restores its badge at once).
    if (_convList) {
        _convList->setShowThreads(_session->capabilities().threadsView);
        _convList->setAgentSessions(_session->capabilities().agentSessions);
        refreshTeammates();
        _session->unreadThreadCountValue() | rpl::on_next(
                                                 [this](int n) {
                                                     if (_convList)
                                                         _convList->setUnreadThreadCount(n);
                                                 },
                                                 _uiLifetime
                                             );
    }

    // The "Saved messages" entry only shows while there is something saved.
    updateSavedMessagesEntry();
    _session->remindersChanged() |
        rpl::on_next([this] { updateSavedMessagesEntry(); }, _uiLifetime);

    // Auth loss, unread badges and notifications are handled by the
    // per-session background subscriptions in ensureSession(); here we only
    // wire what drives the visible UI.
    _session->conversations() |
        rpl::on_next(
            [this](std::vector<Conversation> convs) {
                populateConversations(convs);
                // Keep the header star in sync when isStarred changes.
                if (!_currentConvId.value.isEmpty() && _starBtn) {
                    for (const auto &c : convs) {
                        if (c.id == _currentConvId) {
                            updateStarBtn(c.isStarred);
                            break;
                        }
                    }
                }
                // Huddle state rides on the conversation list, so re-evaluate
                // the banner whenever it changes (incl. the setReading refresh).
                updateHuddleBanner();
                // …and so does whether the open chat can be posted to at all.
                applyComposerAccess();
                // Reveal the conv column the moment real data arrives.
                if (!convs.empty() && _convPanel && !_convPanel->isVisible()) {
                    if (_messageList)
                        _messageList->setWaiting(false);
                    _convPanel->show();
                    if (_convResizeHandle)
                        _convResizeHandle->show();
                }
                // A back/forward jump into this workspace is waiting for the
                // conv list — open the jump target instead of the last conv.
                if (!convs.empty() && !_pendingNavConv.value.isEmpty()) {
                    const int row   = _convList->rowForId(_pendingNavConv);
                    _pendingNavConv = {};
                    if (row >= 0) {
                        _navApplying = true;
                        _convList->selectRow(row);
                        if (_currentConvId.value.isEmpty())
                            openConversation(row);
                        _navApplying = false;
                    }
                    // Conversation gone (left/archived) — fall through to the
                    // usual last-conv restore below.
                }
                // On first populate (nothing shown yet), jump to the last
                // conversation the user had open in the previous session. Gated
                // on the content view, not on _currentConvId: the id is empty on
                // the Threads / Saved overview pages too, and this fires on
                // every sweep that reassigns the list (issue #81).
                if (shouldRestoreLastConv(_contentView)) {
                    restoreLastConv();
                    // If still no conversation after restore attempt, show tips now
                    // that the list is ready (not during initial loading spinner).
                    if (_currentConvId.value.isEmpty() && _convPanel && _convPanel->isVisible() &&
                        _contentStack && _welcomeTips)
                        _contentStack->setCurrentWidget(_welcomeTips);
                }
            },
            _uiLifetime
        );

    _session->users() |
        rpl::on_next(
            [this](std::vector<User> users) {
                if (_convList) {
                    _convList->setUsers(users);
                    _convList->setMe(_session->meUserId());
                }
                if (_convFooter) {
                    const auto meId = _session->meUserId();
                    for (const auto &u : users) {
                        if (u.id == meId) {
                            _convFooter->setUser(
                                u.displayName.isEmpty() ? u.name : u.displayName, u.avatarUrl
                            );
                            break;
                        }
                    }
                }
                if (_convList) {
                    // Re-apply header for current DM conv now that user names are resolved.
                    if (!_currentConvId.value.isEmpty()) {
                        const auto *conv = _session->findConversation(_currentConvId);
                        if (conv && (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim)) {
                            const int row = _convList->rowForId(_currentConvId);
                            if (row >= 0) {
                                const QString name = _convList->resolvedName(row);
                                if (_convNameLabel)
                                    _convNameLabel->setText(name);
                                // Also update the composer placeholder, which was set
                                // from the (still-unresolved) user ID on open —
                                // unless it is showing why the chat is locked.
                                if (_composer && _composerLockReason.isEmpty())
                                    _composer->setPlaceholderText(
                                        name.isEmpty() ? tr("Message") : tr("Message %1").arg(name)
                                    );
                            }
                            updateHeaderForConv(_currentConvId);
                        }
                    }
                }
            },
            _uiLifetime
        );

    _session->events() |
        rpl::on_next(
            [this](Event e) {
                if (const auto *ev = std::get_if<EvPresenceChanged>(&e)) {
                    // Presence flips no longer re-emit users() (see
                    // Session::patchUserSilently) — patch the conv list's dot
                    // directly.
                    if (_convList)
                        _convList->setUserPresence(ev->user, ev->active);
                    if (_headerAvatar && _headerAvatar->isVisible()) {
                        const auto *conv = _session->findConversation(_currentConvId);
                        if (conv && conv->dmUser && *conv->dmUser == ev->user)
                            _headerAvatar->setPresence(ev->active);
                    }
                } else if (const auto *ev = std::get_if<EvDndChanged>(&e)) {
                    if (_convList)
                        _convList->setUserDnd(ev->user, ev->dndEnabled);
                    if (_headerAvatar && _headerAvatar->isVisible()) {
                        const auto *conv = _session->findConversation(_currentConvId);
                        if (conv && conv->dmUser && *conv->dmUser == ev->user)
                            _headerAvatar->setDnd(ev->dndEnabled);
                    }
                } else if (const auto *ev = std::get_if<EvTyping>(&e)) {
                    // Slack never fires this in practice — it delivers
                    // user_typing only over the deprecated RTM API, which has no
                    // Events API / Socket Mode equivalent and which a maintainer
                    // confirmed will not be added (node-slack-sdk#1130); see the
                    // dead user_typing branch in socket_mode_realtime.cpp. The
                    // Claude Code backend does: a session working on a turn
                    // "types" (Capabilities::typing), shown as thinking with the
                    // turn's elapsed time (EvTyping::thinkingSinceMs).
                    //
                    // A background subagent thinks in its thread (threadRoot).
                    //
                    // Show typing for the open conversation (or thread) only.  Our
                    // own id can arrive here when we type from another client (we
                    // never echo local typing), shown as "You … on another device".
                    const bool isSelf = ev->user == _session->meUserId();
                    if (ev->threadRoot) {
                        if (_threadPanel)
                            _threadPanel->userTyping(
                                ev->conv,
                                *ev->threadRoot,
                                ev->user,
                                _session->userDisplayName(ev->user),
                                isSelf,
                                ev->thinkingSinceMs
                            );
                    } else if (_typingIndicator && ev->conv == _currentConvId) {
                        _typingIndicator->userTyping(
                            ev->user,
                            _session->userDisplayName(ev->user),
                            isSelf,
                            ev->thinkingSinceMs
                        );
                    }
                } else if (const auto *ev = std::get_if<EvMessageNew>(&e)) {
                    // A delivered message means that author has stopped typing —
                    // except an agent's mid-task update: it is still working.
                    if (_typingIndicator && ev->conv == _currentConvId &&
                        !isProgressMessage(ev->msg))
                        _typingIndicator->userStopped(ev->msg.author);
                }
            },
            _uiLifetime
        );

    _session->presenceLink() | rpl::on_next(
                                   [this](PresenceLinkState link) {
                                       if (_convFooter)
                                           _convFooter->setPresenceLink(link);
                                   },
                                   _uiLifetime
                               );

    _session->selfPresence() |
        rpl::on_next(
            [this](SelfPresence sp) {
                if (_convList)
                    _convList->setSelfPhantomAway(sp.phantomAway());
                if (_convFooter)
                    _convFooter->setSelfPresence(sp);
                if (_headerAvatar && _headerAvatar->isVisible()) {
                    const auto *conv = _session->findConversation(_currentConvId);
                    if (conv && conv->dmUser && *conv->dmUser == _session->meUserId()) {
                        _headerAvatar->setPhantomAway(sp.phantomAway());
                        _headerAvatar->setToolTip(selfPresenceTooltip(sp));
                    }
                }
            },
            _uiLifetime
        );

    _session->errors() | rpl::on_next([this](QString msg) { showNetworkError(msg); }, _uiLifetime);

    _session->parallelUsageNotice() | rpl::on_next(
                                          [this] {
                                              if (_parallelUsageBanner)
                                                  _parallelUsageBanner->show();
                                          },
                                          _uiLifetime
                                      );
}

void MainWindow::repositionSearch() {
    if (!_searchWidget || !_contentStack || !_msgArea)
        return;
    _searchWidget->setGeometry(_msgArea->rect());
}

// ── Back/forward chat navigation ─────────────────────────────────────────────

void MainWindow::navigateHistory(bool back) {
    const auto valid = [this](const NavLocation &loc) {
        // Workspace must still be logged in; within the active workspace the
        // conversation must still be listed (it may have been left/archived).
        // Background workspaces are checked when their conv list arrives.
        if (!_sessions.count(loc.teamId))
            return false;
        if (loc.teamId == _activeTeamId)
            return _convList && _convList->rowForId(loc.conv) >= 0;
        return true;
    };
    const auto target = back ? _navHistory.goBack(valid) : _navHistory.goForward(valid);
    if (target)
        applyNavLocation(*target);
}

void MainWindow::applyNavLocation(const NavLocation &loc) {
    if (loc.teamId != _activeTeamId) {
        // Cross-workspace jump: open the target conversation (not the
        // last-open one) once the workspace's conv list is available.
        // activateWorkspace replays the cached list synchronously, so this
        // normally completes before it returns.
        _pendingNavConv = loc.conv;
        activateWorkspace(loc.teamId);
        return;
    }
    const int row = _convList ? _convList->rowForId(loc.conv) : -1;
    if (row < 0)
        return;
    _navApplying = true;
    _convList->selectRow(row); // emits conversationSelected → openConversation
    // selectRow no-ops when the row is already visually selected (e.g. stale
    // selection after a workspace round-trip) — drive the open directly.
    if (_currentConvId != loc.conv)
        openConversation(row);
    _navApplying = false;
}

void MainWindow::showNetworkError(const QString &message) {
    if (!_errorBanner)
        return;
    _errorBanner->setText(message);
    _errorBanner->show();
    QTimer::singleShot(5000, _errorBanner, &QWidget::hide);
}

void MainWindow::applyUpdateAndRestart() {
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    // Signal main() to release SingleInstance and re-exec after the event loop exits.
    QCoreApplication::exit(kRestartExitCode);
#elif defined(Q_OS_MACOS)
    QDesktopServices::openUrl(QUrl::fromLocalFile(_updateChecker->downloadedPath()));
#endif
}

void MainWindow::restartApp() {
    // Same clean re-exec path as an applied update (main() handles kRestartExitCode
    // on every platform); no download involved.
    QCoreApplication::exit(kRestartExitCode);
}

// Center-crop `src` to a square and mask it into a rounded-rect — the same
// shape avatars take everywhere else in the app — for use as a notification
// image. Rendered at `side`px (the OS toast rescales as needed). A null input
// (avatar not cached yet / no URL) yields a null pixmap so the caller can fall
// back to a no-image toast.
static QPixmap roundedNotifIcon(const QPixmap &src, int side = 64) {
    if (src.isNull())
        return {};
    const int     s  = qMin(src.width(), src.height());
    const QPixmap sq = src.copy((src.width() - s) / 2, (src.height() - s) / 2, s, s);
    const qreal   r  = side * 0.22;

    QPixmap out(side, side);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, side, side), r, r);
    p.setClipPath(clip);
    p.drawPixmap(QRect(0, 0, side, side), sq);
    return out;
}

// The notification chime, when the user has it on.
static void playNotificationSound(const QSettings &s) {
    if (s.value("notifications/sound", true).toBool())
        Sound::Player::instance().play(
            s.value("notifications/soundId", Sound::Player::defaultId()).toString()
        );
}

void MainWindow::setPendingNotifTarget(
    const QString &teamId, const ConversationId &conv, const Ts &threadRoot, const Ts &msgTs
) {
    _pendingNotifTeam       = teamId;
    _pendingNotifConv       = conv;
    _pendingNotifThreadRoot = threadRoot;
    _pendingNotifMsgTs      = msgTs;
}

void MainWindow::showTrayMessage(
    const QString               &title,
    const QString               &body,
    const QPixmap               &pix,
    int                          timeoutMs,
    QSystemTrayIcon::MessageIcon iconWithoutPix
) {
    if (pix.isNull())
        _trayIcon->showMessage(title, body, iconWithoutPix, timeoutMs);
    else
        _trayIcon->showMessage(title, body, QIcon(pix), timeoutMs);
}

void MainWindow::maybeNotify(const QString &teamId, const EvMessageNew &ev, bool allowDefer) {
    // Too old to announce: a reconnect backfill, a cache replay or a history
    // sweep can surface month-old messages as fresh EvMessageNew. Drop those
    // silently — no toast, no sound (see kMaxNotifyAgeDays). Before every other
    // gate so an ancient message can't reach any surface.
    if (tooOldToNotify(ev.msg.date))
        return;

    QSettings s("msga", "msga");
    if (!s.value("notifications/enabled", true).toBool())
        return;

    // Muted workspace: high-level switch suppresses every OS notification from it.
    if (_mutedTeams.contains(teamId))
        return;

    const auto it = _sessions.find(teamId);
    if (it == _sessions.end())
        return;
    Session *session = it->second.session.get();

    // Skip own messages and bot messages with no author
    const UserId me = session->meUserId();
    if (!me.value.isEmpty() && ev.msg.author == me)
        return;

    // A huddle posts a "huddle_thread" system message that also arrives here;
    // it gets its own dedicated notification via maybeNotifyHuddle, so don't
    // double-notify with the raw system message.
    if (ev.msg.subtype && *ev.msg.subtype == QLatin1String("huddle_thread"))
        return;

    // An agent's mid-task updates (Claude Code's tool calls, interim remarks)
    // are shown but never announced — only the answer that ends the task is.
    if (isProgressMessage(ev.msg))
        return;

    // Skip if this conversation is on screen right now
    if (isActiveWindow() && teamId == _activeTeamId && ev.conv == _currentConvId)
        return;

    const auto *conv = session->findConversation(ev.conv);

    // Unknown conversation → not a member (or another workspace's event from
    // the shared socket); muted → no notification wanted, mentions included.
    if (!conv || conv->isMuted || conv->locallyMuted || !conv->isMember ||
        conv->notifLevel == NotificationLevel::Mute)
        return;

    const bool     isDm = (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
    const QString &mt   = ev.msg.rawText.isEmpty() ? ev.msg.text.text : ev.msg.rawText;

    // A thread the user muted suppresses notifications for its replies (Slack's
    // "Mute thread") — except an explicit @mention, which always gets through.
    if (ev.msg.threadRoot && session->isThreadMuted(ev.conv, *ev.msg.threadRoot) &&
        !session->mentionsMe(mt))
        return;

    // A reply in a thread we follow notifies regardless of the channel's level,
    // matching Slack's default-on "Replies to threads you're following" — the
    // thread we started (parent_user_id) and, from the session's followed set,
    // the thread we merely replied in.
    const bool followedReply =
        ev.msg.threadRoot && (isFollowedThreadReply(ev.msg, me) ||
                              session->isThreadFollowed(ev.conv, *ev.msg.threadRoot));
    const bool isImportant = isDm || session->mentionsMe(mt) || followedReply;

    // Per-conversation level wins over the global default ("All new posts" unless
    // the user changed it). Muted conversations already returned above, so the
    // effective level here is only ever All or Mentions: "Just mentions" delivers
    // no notification (no tray, no D-Bus) for a non-DM, non-@mention message.
    const NotificationLevel lvl = effectiveNotifLevel(*conv, globalDefaultNotifLevel());
    if (lvl != NotificationLevel::All && !isImportant)
        return;

    // Names first: a Slack Connect / system / deactivated account is absent from
    // users.list, so its message would announce itself as "Someone: @U0C3E7HGZHS
    // has joined the channel". users.info can resolve both the author and the
    // @mentions, but only asynchronously — so start the fetches and re-run this
    // once they land (bounded wait; whatever is still raw by then is shown as
    // it is). Only ids the backend can actually look up are waited on, so a bot
    // post (author "B…", never in the user cache) still notifies immediately.
    if (allowDefer) {
        std::vector<UserId> pending;
        auto                need = [&](const UserId &id) {
            if (id.value.isEmpty() || session->findUser(id))
                return;
            if (!session->backend() || !session->backend()->isUserId(id))
                return;
            if (std::find(pending.begin(), pending.end(), id) == pending.end())
                pending.push_back(id);
        };
        need(ev.msg.author);
        for (const auto &uid : MsgRender::notificationRawMentions(ev.msg))
            need(uid);
        if (!pending.empty()) {
            for (const auto &uid : pending)
                session->fetchUserIfNeeded(uid);
            notifyWhenUsersResolve(teamId, ev, std::move(pending), 0);
            return;
        }
    }

    // Build title and body
    const auto *sender = session->findUser(ev.msg.author);
    if (!sender)
        // External/system sender absent from users.list: resolve it so the next
        // notification (and the chat) shows a name instead of "Someone". A
        // background workspace may never open its message list, which is the
        // other place that would have triggered this.
        session->fetchUserIfNeeded(ev.msg.author);
    const QString senderName =
        sender ? (sender->displayName.isEmpty() ? sender->name : sender->displayName)
               : tr("Someone");

    // Resolve mentions/channels/emoji codes to friendly names for the OS toast.
    const QString preview = MsgRender::notificationPreview(ev.msg, session);

    QString title, body;
    if (isDm) {
        title = senderName;
        body  = preview;
        // Agent sessions all answer as the same "Agent": name the session.
        if (session->capabilities().agentSessions && conv->dmUser)
            title = session->userDisplayName(*conv->dmUser);
    } else {
        title = "#" + conv->name;
        body  = senderName + ": " + preview;
    }
    // Say which workspace it came from when it isn't the one on screen.
    if (teamId != _activeTeamId) {
        const QString teamName = recordForHandle(teamId).displayName;
        if (!teamName.isEmpty())
            title = teamName + " · " + title;
    }
    if (body.length() > 100)
        body = body.left(97) + "…";

    // Notification image: the message sender's avatar for DMs, the workspace
    // icon for channels (and anything else). We only use an already-cached
    // pixmap — ImageCache::get() kicks off a download but returns null until it
    // lands, so a not-yet-cached image just means no picture this time (and the
    // fetch we triggered makes it available for the next one).
    QPixmap notifPix;
    if (_imgCache) {
        QString iconUrl;
        if (isDm) {
            if (sender && !sender->avatarUrl.isEmpty())
                iconUrl = sender->avatarUrl;
            else if (!ev.msg.botAvatarUrl.isEmpty())
                iconUrl = ev.msg.botAvatarUrl;
        } else {
            iconUrl = iconUrlForHandle(teamId);
        }
        if (!iconUrl.isEmpty())
            notifPix = roundedNotifIcon(_imgCache->get(iconUrl));
    }

    // Prefer the freedesktop notifier on Linux: it puts the avatar into the OS
    // notification banner/centre via the `image-data` hint, which Qt's
    // showMessage can't reach (it only sends the QIcon as `app_icon`, which
    // GNOME/Ubuntu ignore for the picture). Click-to-open is delivered through
    // its "default" action. Anywhere it's unavailable (macOS/Windows, no QtDBus,
    // no notification daemon) we fall back to the tray: showMessage's QIcon
    // overload is honoured on Windows toasts; macOS ignores it and uses the app
    // icon, while the image-less toast is the floor everywhere.
    // A thread reply lives in the thread, not the channel timeline, so carry its
    // root through the click token: opening the channel alone would land on a
    // timeline the reply isn't in ("notification, but nothing there").
    const Ts replyRoot = ev.msg.threadRoot ? *ev.msg.threadRoot : Ts{};

    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable()) {
        const QString token = encodeNotifToken(teamId, ev.conv, replyRoot);
        shown               = _desktopNotifier->notify(
            title, body, notifPix.isNull() ? QImage() : notifPix.toImage(), token, {}, 5000
        );
    }
    if (!shown) {
        setPendingNotifTarget(teamId, ev.conv, replyRoot, {});
        showTrayMessage(title, body, notifPix);
    }

    playNotificationSound(s);
}

// How long a notification waits for users.info to put names to the ids it
// mentions: polled in short steps so the toast fires as soon as the last one
// lands, and capped so a lookup that never answers only delays it by ~1.5 s.
static constexpr int kNotifyResolveStepMs = 150;
static constexpr int kNotifyResolveTries  = 10;

void MainWindow::notifyWhenUsersResolve(
    const QString &teamId, const EvMessageNew &ev, std::vector<UserId> pending, int tries
) {
    QTimer::singleShot(kNotifyResolveStepMs, this, [this, teamId, ev, pending, tries]() mutable {
        // The workspace may be gone (signed out / removed) by now; maybeNotify
        // re-checks that itself, along with every other gate — including the
        // "conversation is on screen" one, which the user may have satisfied
        // while we waited.
        const auto it = _sessions.find(teamId);
        if (it == _sessions.end())
            return;
        Session *session = it->second.session.get();

        std::erase_if(pending, [session](const UserId &id) {
            return session->findUser(id) != nullptr;
        });
        if (pending.empty() || tries + 1 >= kNotifyResolveTries) {
            maybeNotify(teamId, ev, false);
            return;
        }
        notifyWhenUsersResolve(teamId, ev, std::move(pending), tries + 1);
    });
}

void MainWindow::maybeNotifyHuddle(const QString &teamId, const EvHuddleChanged &ev) {
    // No kMaxNotifyAgeDays gate here, deliberately: a huddle carries no start
    // time (conversations.info reports only that a room is live *now*), and its
    // conversation's latestTs is no proxy — a huddle in a channel that has been
    // quiet for months is still happening this second. The false→true dedup
    // below is what keeps a re-fire from replaying an already-seen huddle.
    const QString key = teamId + QChar(0x1f) + ev.conv.value;

    // A huddle ending clears the dedup key so its next start notifies again.
    // EvHuddleChanged also re-fires on edits / history reconcile / conv-info
    // refresh, so notify only on the false→true transition we haven't seen yet.
    if (!ev.active) {
        _notifiedHuddles.remove(key);
        return;
    }
    if (_notifiedHuddles.contains(key))
        return;

    QSettings s("msga", "msga");
    if (!s.value("notifications/enabled", true).toBool() ||
        !s.value("notifications/huddles", true).toBool())
        return;

    // Muted workspace: no OS notification (the false→true dedup above still ran,
    // so an unmute mid-huddle won't retroactively fire for an already-seen one).
    if (_mutedTeams.contains(teamId))
        return;

    const auto it = _sessions.find(teamId);
    if (it == _sessions.end())
        return;
    Session *session = it->second.session.get();
    if (!session->capabilities().huddles)
        return;

    const UserId me   = session->meUserId();
    const auto  *conv = session->findConversation(ev.conv);
    // Member-only, not muted, not a huddle I'm already in, and (for channels)
    // only when set to "All new posts" — see shouldNotifyHuddleStart. A
    // locally-muted person stays silent here too.
    if (!conv || conv->locallyMuted ||
        !shouldNotifyHuddleStart(*conv, ev.participants, me, globalDefaultNotifLevel()))
        return;
    const bool isDm = (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);

    // Already looking at this conversation — the in-window HuddleBanner already
    // offers Join, so a popup would be redundant.
    if (isActiveWindow() && teamId == _activeTeamId && ev.conv == _currentConvId) {
        _notifiedHuddles.insert(key);
        return;
    }

    // Record before delivering so a burst of re-fires can't double-notify.
    _notifiedHuddles.insert(key);

    // Starter = first listed participant (Session fills [host] for a fresh room).
    const User *starter =
        ev.participants.empty() ? nullptr : session->findUser(ev.participants.front());
    const QString starterName =
        starter ? (starter->displayName.isEmpty() ? starter->name : starter->displayName)
                : tr("Someone");

    QString title, body;
    if (isDm) {
        // Title already names the person, so don't repeat it in the body.
        title = starterName;
        body  = tr("Started a huddle");
    } else {
        title = "#" + conv->name;
        body  = tr("%1 started a huddle").arg(starterName);
    }
    if (teamId != _activeTeamId) {
        const QString teamName = recordForHandle(teamId).displayName;
        if (!teamName.isEmpty())
            title = teamName + " · " + title;
    }

    // Notification image: starter avatar for a DM, workspace icon otherwise
    // (only an already-cached pixmap; a miss just means no picture this time).
    QPixmap notifPix;
    if (_imgCache) {
        QString iconUrl;
        if (isDm && starter && !starter->avatarUrl.isEmpty())
            iconUrl = starter->avatarUrl;
        else
            iconUrl = iconUrlForHandle(teamId);
        if (!iconUrl.isEmpty())
            notifPix = roundedNotifIcon(_imgCache->get(iconUrl));
    }

    // The room's own huddle_link is the authoritative join URL; fall back to the
    // ?open=start_huddle deep link (resolved against the huddle's team, which may
    // not be the active workspace).
    const QString joinUrl   = ev.link.isEmpty() ? huddleJoinUrl(teamId, ev.conv) : ev.link;
    const QString bodyToken = key; // body click → open the conversation
    const QString joinToken = QStringLiteral("join") + QChar(0x1f) + joinUrl;

    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable()) {
        const QList<NotifAction> actions{{QStringLiteral("join"), tr("Join"), joinToken}};
        shown = _desktopNotifier->notify(
            title, body, notifPix.isNull() ? QImage() : notifPix.toImage(), bodyToken, actions, 5000
        );
    }
    if (!shown) {
        // The tray balloon has no action button; a click opens the conversation
        // (which surfaces the HuddleBanner's Join pill).
        setPendingNotifTarget(teamId, ev.conv, {}, {}); // a huddle isn't a thread
        showTrayMessage(title, body, notifPix);
    }

    playNotificationSound(s);
}

void MainWindow::notifyReminderDue(const QString &teamId, const EvReminderDue &ev) {
    // Only the global switch gates a reminder: the user explicitly asked for
    // this one, so per-conversation levels, thread mutes and even the workspace
    // mute don't apply (matching the official client, which alarms regardless).
    QSettings s("msga", "msga");
    if (!s.value("notifications/enabled", true).toBool())
        return;

    const auto it = _sessions.find(teamId);
    if (it == _sessions.end())
        return;
    Session *session = it->second.session.get();

    // Where the reminded message lives, in human terms — never a raw id. A DM's
    // conv->name IS the raw peer id (see ConvListWidget), so resolve it through
    // the user cache; an MPDM names itself from its members.
    const auto *conv = session->findConversation(ev.conv);
    QString     where;
    if (conv) {
        if (conv->kind == ConvKind::Im) {
            if (conv->dmUser)
                where = session->userDisplayName(*conv->dmUser);
        } else if (!groupDmCustomName(*conv).isEmpty()) {
            where = groupDmCustomName(*conv);
        } else if (conv->kind == ConvKind::Mpim) {
            QStringList  names;
            const UserId me = session->meUserId();
            for (const auto &uid : conv->members) {
                if (uid == me)
                    continue;
                if (const User *u = session->findUser(uid)) {
                    const QString n = u->displayName.isEmpty() ? u->name : u->displayName;
                    if (!n.isEmpty())
                        names << n;
                } else {
                    session->fetchUserIfNeeded(uid);
                }
                if (names.size() == 3)
                    break;
            }
            where = names.join(QStringLiteral(", "));
        } else {
            where = QStringLiteral("#") + conv->name;
        }
    }

    QString title = where.isEmpty() ? tr("Reminder") : tr("Reminder — %1").arg(where);
    if (teamId != _activeTeamId) {
        const QString teamName = recordForHandle(teamId).displayName;
        if (!teamName.isEmpty())
            title = teamName + " · " + title;
    }

    // Say who wrote the reminded message. The author's name resolves live from
    // the user cache (bot posts carry their name in the event instead); a
    // reminder set from another client has no author — plain snippet then.
    const User *author = ev.author.value.isEmpty() ? nullptr : session->findUser(ev.author);
    if (!author && !ev.author.value.isEmpty())
        session->fetchUserIfNeeded(ev.author);
    QString authorName =
        author ? (author->displayName.isEmpty() ? author->name : author->displayName) : QString();
    if (authorName.isEmpty())
        authorName = ev.botName;

    QString body =
        ev.snippet.isEmpty() ? tr("You asked to be reminded about a message.") : ev.snippet;
    if (!authorName.isEmpty() && !ev.snippet.isEmpty())
        body = authorName + ": " + body;
    if (body.length() > 100)
        body = body.left(97) + "…";

    // Picture: the reminded message's author (user avatar, else bot avatar),
    // falling back to the DM peer and finally the workspace icon. Cache-only,
    // same as maybeNotify — a miss just means no picture this time.
    QPixmap notifPix;
    if (_imgCache) {
        QString iconUrl;
        if (author && !author->avatarUrl.isEmpty())
            iconUrl = author->avatarUrl;
        else if (!ev.botAvatarUrl.isEmpty())
            iconUrl = ev.botAvatarUrl;
        else if (conv && conv->kind == ConvKind::Im && conv->dmUser) {
            if (const User *peer = session->findUser(*conv->dmUser))
                iconUrl = peer->avatarUrl;
        }
        if (iconUrl.isEmpty())
            iconUrl = iconUrlForHandle(teamId);
        if (!iconUrl.isEmpty())
            notifPix = roundedNotifIcon(_imgCache->get(iconUrl));
    }

    // The token carries the exact message ts so the click scrolls to it.
    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable()) {
        const QString token = encodeNotifToken(teamId, ev.conv, ev.threadRoot, ev.ts);
        shown               = _desktopNotifier->notify(
            title, body, notifPix.isNull() ? QImage() : notifPix.toImage(), token, {}, 5000
        );
    }
    if (!shown && _trayIcon) {
        setPendingNotifTarget(teamId, ev.conv, ev.threadRoot, ev.ts);
        showTrayMessage(title, body, notifPix);
    }

    playNotificationSound(s);
}

void MainWindow::showSampleNotification(int kind) {
    // Fire a representative, self-contained notification (no real session/conv)
    // so the user can see how each kind looks with their OS notifier + settings.
    const auto    k             = static_cast<SettingsDialog::SampleNotif>(kind);
    const QString sampleUser    = tr("Sample User");
    const QString sampleChannel = QStringLiteral("#general");

    QString            title, body;
    QList<NotifAction> actions;
    switch (k) {
    case SettingsDialog::SampleNotif::Dm:
        title = sampleUser;
        body  = tr("Hey — do you have a minute?");
        break;
    case SettingsDialog::SampleNotif::Channel:
        title = sampleChannel;
        body  = tr("%1: Heads up, the deploy is going out at 3pm").arg(sampleUser);
        break;
    case SettingsDialog::SampleNotif::Huddle: {
        title                 = sampleChannel;
        body                  = tr("%1 started a huddle").arg(sampleUser);
        const QString joinUrl = (!_activeTeamId.isEmpty() && !_currentConvId.value.isEmpty())
                                    ? huddleJoinUrl(_currentConvId)
                                    : QStringLiteral("https://app.slack.com");
        actions.append(
            {QStringLiteral("join"), tr("Join"), QStringLiteral("join") + QChar(0x1f) + joinUrl}
        );
        break;
    }
    }

    // Picture: the active workspace icon when cached (illustrative only).
    QPixmap notifPix;
    if (_imgCache && !_activeTeamId.isEmpty()) {
        const QString iconUrl = iconUrlForHandle(_activeTeamId);
        if (!iconUrl.isEmpty())
            notifPix = roundedNotifIcon(_imgCache->get(iconUrl));
    }

    // A test-only token correlates native submission status; clicks open no conversation.
#ifdef Q_OS_MACOS
    _settingsDialog->setNotificationTestResult(tr("Submitting notification to macOS…"));
#endif
    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable())
        shown = _desktopNotifier->notify(
            title,
            body,
            notifPix.isNull() ? QImage() : notifPix.toImage(),
            QStringLiteral("msga-notification-test"),
            actions,
            5000
        );
#ifdef Q_OS_MACOS
    if (!shown)
        _settingsDialog->setNotificationTestResult(
            tr("The macOS notification service is unavailable.")
        );
#endif
    if (!shown && _trayIcon) {
        setPendingNotifTarget({}, {}, {}, {});
        showTrayMessage(title, body, notifPix);
    }
}

void MainWindow::notifySessionExpired(const QString &teamId) {
    // Only when the user can't see the login screen the UI is about to show.
    // Minimize-to-tray hides the window outright; a plain minimize leaves it
    // mapped but off screen — both need the nudge. Deliberately NOT gated on
    // the message-notification toggle: this is the app telling the user it has
    // stopped working, not chatter it could reasonably swallow.
    if (isVisible() && !isMinimized())
        return;
    if (!_trayIcon && !(_desktopNotifier && _desktopNotifier->isAvailable()))
        return;

    const QString workspace = recordForHandle(teamId).displayName;
    const QString title     = tr("Session expired");
    const QString body =
        workspace.isEmpty()
            ? tr("Your session has expired. Click to sign in again.")
            : tr("Your %1 session has expired. Click to sign in again.").arg(workspace);

    // Picture: the workspace icon when cached, so the toast reads as that
    // workspace's; the image-less form is the floor.
    QPixmap notifPix;
    if (_imgCache) {
        const QString iconUrl = iconUrlForHandle(teamId);
        if (!iconUrl.isEmpty())
            notifPix = roundedNotifIcon(_imgCache->get(iconUrl));
    }

    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable())
        shown = _desktopNotifier->notify(
            title,
            body,
            notifPix.isNull() ? QImage() : notifPix.toImage(),
            encodeReloginNotifToken(teamId),
            {},
            10000
        );
    if (!shown && _trayIcon) {
        // Tray-balloon click → openNotifTarget with no conversation, which just
        // brings the window forward; the login screen is what it lands on.
        setPendingNotifTarget(teamId, {}, {}, {});
        showTrayMessage(title, body, notifPix, 10000, QSystemTrayIcon::Warning);
    }
}

void MainWindow::updateUnreadBadges(const QString &teamId, const std::vector<Conversation> &convs) {
    // important (red) = DM/MPDM unreads + channel @mentions. normal (blue) = other
    // *allowed* unread activity — only in channels set to "All new posts". A muted
    // conversation is fully silent (no red, no blue); a "Just mentions" channel
    // contributes only its @mentions (red), never blue.
    const NotificationLevel fallback = globalDefaultNotifLevel();
    int                     normal = 0, important = 0;
    const auto              ws            = _sessions.find(teamId);
    const bool              agentSessions = ws != _sessions.end() && ws->second.session &&
                                            ws->second.session->capabilities().agentSessions;
    for (const auto &c : convs) {
        if (!c.isMember)
            continue;
        // A locally-muted person contributes to neither ball nor counter (its
        // in-list bold emphasis is handled by ConvListWidget independently).
        if (c.locallyMuted)
            continue;
        // Everything unread here predates the notification window, so it
        // contributes to neither the switcher counter nor the tray/dock tint
        // this feeds (see kMaxNotifyAgeDays).
        if (tooOldToNotify(c))
            continue;
        const NotificationLevel lvl = effectiveNotifLevel(c, fallback);
        if (lvl == NotificationLevel::Mute)
            continue;
        const bool isDm = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
        if (agentSessions) {
            // An agent session counts only what needs you (its answer, "waiting
            // for you"), never its progress updates.
            important += c.mentionCount;
        } else if (isDm) {
            important += c.unread;
        } else {
            important += c.mentionCount;
            if (lvl == NotificationLevel::All && c.unread > c.mentionCount)
                normal += c.unread - c.mentionCount;
        }
    }
    const QPair<int, int> counts{normal, important};
    if (_wsUnreads.value(teamId) == counts)
        return;
    _wsUnreads[teamId] = counts;
    if (_switcher)
        _switcher->setUnreadCounts(teamId, normal, important);
    updateTrayIcon();
}

void MainWindow::updateTrayIcon() {
    int globalTotal = 0, globalMentions = 0;
    for (auto it = _wsUnreads.cbegin(); it != _wsUnreads.cend(); ++it) {
        // Muted workspaces never tint the tray ball (their in-app unread badges
        // and chat emphasis are untouched — this is a notification-only switch).
        if (_mutedTeams.contains(it.key()))
            continue;
        globalTotal += it.value().first;
        globalMentions += it.value().second;
    }

#ifdef Q_OS_MACOS
    // Dock tile badge: the actionable count (DM unreads + @mentions), matching
    // Slack. Plain channel activity stays off the Dock (it still shows the tray
    // tint and the in-app blue dot). Set before the tray work below, which bails
    // out when there is no tray icon — the Dock badge is independent of it.
    macSetDockBadge(globalMentions);
#endif

    if (!_trayIcon)
        return;

    // The user's own picture (Settings → Appearance → Tray icon) replaces the plane.
    const QImage custom = CustomTrayIcon::current();
#ifdef Q_OS_MACOS
    // NSImage template: follows the menu bar, not the app theme. A custom
    // picture opts out when its monochrome option is off, so a
    // colour logo keeps its colours instead of flattening to a silhouette.
    const bool templ = custom.isNull() || CustomTrayIcon::monochrome();
#endif

    const int sz = 128;
    QPixmap   px(sz, sz);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!custom.isNull()) {
        p.drawImage(QRectF(0, 0, sz, sz), custom);
    } else {
        // Always render via QSvgRenderer so the alpha channel is preserved in static builds.
        QSvgRenderer renderer(QString(":/icon_tray.svg"));
        if (renderer.isValid())
            renderer.render(&p, QRectF(0, 0, sz, sz));
    }
    if (globalTotal > 0 || globalMentions > 0) {
        // Red when anything important (DM/mention) is unread anywhere,
        // blue for plain unread activity.
        const int d = 36;
        p.setPen(Qt::NoPen);
        p.setBrush(globalMentions > 0 ? Th::c().badge.mention : Th::c().badge.activity);
#ifdef Q_OS_MACOS
        if (templ) {
            // Cut a clear halo so the monochrome dot stays distinct from the wing.
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.setBrush(Qt::white);
            p.drawEllipse(sz - d - 4, sz - d - 4, d + 8, d + 8);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setBrush(Qt::white); // template alpha, tinted by macOS with the plane
        }
#endif
        p.drawEllipse(sz - d, sz - d, d, d);
    }
    p.end();
    QIcon icon;
#ifdef Q_OS_MACOS
    for (int scale : {1, 2}) {
        auto sized =
            px.scaled(22 * scale, 22 * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        sized.setDevicePixelRatio(scale);
        icon.addPixmap(sized);
    }
    icon.setIsMask(templ);
#else
    icon.addPixmap(px);
#endif
    _trayIcon->setIcon(icon);
}

// ── Workspace management ──────────────────────────────────────────────────────

void MainWindow::refreshSwitcher() {
    if (!_switcher)
        return;

    const auto                            keys = TokenStore::workspaceKeys();
    std::vector<WorkspaceSwitcher::Entry> entries;
    entries.reserve(keys.size());
    _mutedTeams.clear();
    for (const auto &key : keys) {
        const auto    rec    = TokenStore::loadWorkspace(key);
        const QString handle = key.toString();
        if (TokenStore::isWorkspaceMuted(key))
            _mutedTeams.insert(handle);
        entries.push_back(
            {handle,
             rec ? rec->displayName : QString(),
             rec ? TokenStore::displayIconUrl(*rec) : QString()}
        );
    }
    _switcher->setWorkspaces(entries);
    _switcher->setActive(_activeTeamId);
    // Re-apply unread counts — entries built from TokenStore carry zeros.
    for (auto it = _wsUnreads.cbegin(); it != _wsUnreads.cend(); ++it)
        _switcher->setUnreadCounts(it.key(), it.value().first, it.value().second);
}

void MainWindow::logoutWorkspace(const QString &teamId) {
    const bool wasActive = (teamId == _activeTeamId);

    dropSession(teamId);
    if (wasActive)
        _activeTeamId.clear();
    if (const auto key = WorkspaceKey::fromString(teamId)) {
        CustomWorkspaceIcon::remove(*key); // the override's file goes with the workspace
        TokenStore::removeWorkspace(*key);
    }

    const auto remaining = TokenStore::workspaceKeys();
    if (remaining.empty()) {
        showLoggedOut();
    } else if (wasActive) {
        activateWorkspace(remaining.front().toString());
    } else {
        refreshSwitcher();
    }
}

void MainWindow::changeWorkspaceIcon(const QString &teamId) {
    const auto key = WorkspaceKey::fromString(teamId);
    if (!key)
        return;
    const auto    rec     = recordForHandle(teamId);
    const QString url     = TokenStore::displayIconUrl(rec);
    const QPixmap current = (_imgCache && !url.isEmpty()) ? _imgCache->get(url) : QPixmap();
    auto         *dlg     = new WorkspaceIconDialog(
        teamId, rec.displayName, current, !TokenStore::customWorkspaceIconPath(*key).isEmpty(), this
    );
    connect(dlg, &AppDialog::finished, this, [this, dlg, key = *key](int result) {
        dlg->deleteLater();
        if (result != QDialog::Accepted)
            return;
        if (dlg->resetRequested()) {
            CustomWorkspaceIcon::remove(key);
        } else if (const QImage img = dlg->chosenImage(); !img.isNull()) {
            if (CustomWorkspaceIcon::install(key, img).isEmpty()) {
                QMessageBox::warning(
                    this, tr("Workspace icon"), tr("The icon could not be saved.")
                );
                return;
            }
        }
        // The rail rebuilds its entries from TokenStore; a changed url drops
        // the carried pixmap so the new file is fetched.
        refreshSwitcher();
    });
    dlg->open();
}

void MainWindow::toggleWorkspaceMute(const QString &teamId) {
    const auto key = WorkspaceKey::fromString(teamId);
    if (!key)
        return;
    const bool muted = !_mutedTeams.contains(teamId);
    TokenStore::setWorkspaceMuted(*key, muted);
    if (muted)
        _mutedTeams.insert(teamId);
    else
        _mutedTeams.remove(teamId);
    // The mute flag only changes whether this workspace tints the global tray
    // ball; re-render it now so the change is immediate.
    updateTrayIcon();
}

void MainWindow::showWorkspaceMenu(const QString &teamId, const QPoint &globalPos) {
    const auto rec  = recordForHandle(teamId);
    auto      *menu = new ContextMenu(this);
    menu->setWidthMode(ContextMenu::WidthMode::MinWidth);

    // Workspace admins get a shortcut to the Slack admin settings in their browser.
    const auto it = _sessions.find(teamId);
    if (it != _sessions.end() && it->second.session && it->second.session->meIsAdmin()) {
        QString base = it->second.session->teamUrl();
        if (!base.isEmpty()) {
            if (!base.endsWith('/'))
                base += '/';
            const QString adminUrl = base + QStringLiteral("admin/settings");
            menu->addItem(tr("Workspace admin"), [adminUrl] {
                QDesktopServices::openUrl(QUrl(adminUrl));
            });
        }
    }

    menu->addItem(tr("Change icon…"), [this, teamId] { changeWorkspaceIcon(teamId); });

    menu->addItem(_mutedTeams.contains(teamId) ? tr("Unmute") : tr("Mute"), [this, teamId] {
        toggleWorkspaceMute(teamId);
    });

    menu->addItem(
        rec.displayName.isEmpty() ? tr("Log out") : tr("Log out from %1").arg(rec.displayName),
        [this, teamId] { logoutWorkspace(teamId); },
        /*destructive=*/true
    );
    menu->popup(globalPos);
}

// ── Tray ──────────────────────────────────────────────────────────────────────

void MainWindow::restoreFromTray() {
    // Bring the window to the front whatever its state (minimized to taskbar,
    // tucked into the tray, or just behind other windows).
    //
    // Un-minimizing is the tricky part: Wayland's xdg_toplevel protocol has a
    // set_minimized request but NO un-minimize counterpart, so clearing the
    // minimized state / showNormal() on a still-mapped surface is silently
    // ignored — clicking a tray item did nothing. The reliable cross-platform
    // way is to tear the surface down and recreate it: hide() then show*().
    // On Wayland the compositor can minimize/hide our window without telling Qt:
    // isMinimized() stays false and the state still reads Maximized/Normal, while
    // raise()/activateWindow() are silently ignored (no xdg-activation token — a
    // D-Bus tray click can't grant one). The only reliable way to bring the
    // window back is to destroy and recreate the surface (hide → show*): a freshly
    // mapped toplevel is shown by the compositor. The robust signal for "we need
    // to do this" is that we don't currently hold focus; when already active we
    // skip the cycle to avoid a needless flicker.
    if (!isActiveWindow()) {
        const bool wasMaximized = windowState() & Qt::WindowMaximized;
        hide();
        if (wasMaximized)
            showMaximized();
        else
            showNormal();
    }
    raise();
    activateWindow();
}

void MainWindow::setupTray() {
    _trayIcon = new QSystemTrayIcon(this);
    _trayIcon->setToolTip("MSGA");
    updateTrayIcon();

    auto *menu = new QMenu(this);

    const auto keys = TokenStore::workspaceKeys();
    for (const auto &key : keys) {
        const QString id    = key.toString();
        const auto    rec   = TokenStore::loadWorkspace(key);
        const QString label = (rec && !rec->displayName.isEmpty()) ? rec->displayName : id;
        menu->addAction(label, this, [this, id] {
            restoreFromTray();
            QMetaObject::invokeMethod(
                this, [this, id] { switchToWorkspace(id); }, Qt::QueuedConnection
            );
        });
    }

    menu->addSeparator();
    menu->addAction(tr("Settings"), this, [this] {
        restoreFromTray();
        QMetaObject::invokeMethod(this, [this] { _settingsDialog->open(); }, Qt::QueuedConnection);
    });
    // We are frameless, so a window that ends up off-screen or oversized has no
    // WM affordance to recover it — the titlebar with our own move/resize handles
    // may itself be out of reach. The tray always is reachable.
    menu->addAction(tr("Reset window size"), this, [this] {
        restoreFromTray();
        QMetaObject::invokeMethod(this, [this] { resetWindowGeometry(); }, Qt::QueuedConnection);
    });
    menu->addSeparator();
    auto *quitAct = menu->addAction(tr("Quit"));
    // Defer quit so the menu closes fully before the event loop exits;
    // calling exit() synchronously inside a menu-action handler corrupts Qt's popup state.
    connect(quitAct, &QAction::triggered, this, [] {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    });
    _trayIcon->setContextMenu(menu);

    connect(
        _trayIcon,
        &QSystemTrayIcon::activated,
        this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            // Left click (Trigger): restore the window if it's tucked away in the
            // tray (hidden via closeEvent) or minimized; otherwise do nothing —
            // matching Telegram Desktop. Right click shows the context menu, which
            // Qt handles natively (placed next to the icon). The native popup we
            // used to raise here on Trigger appeared in the wrong style and far
            // from the icon.
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                if (!isVisible() || isMinimized())
                    restoreFromTray();
            }
        }
    );

    connect(_trayIcon, &QSystemTrayIcon::messageClicked, this, [this] {
        openNotifTarget(
            _pendingNotifTeam, _pendingNotifConv, _pendingNotifThreadRoot, _pendingNotifMsgTs
        );
        _pendingNotifConv = {};
        _pendingNotifTeam.clear();
        _pendingNotifThreadRoot.clear();
        _pendingNotifMsgTs.clear();
    });

    _trayIcon->show();

    // Platform notifier (Linux/macOS): clicking a notification fires activated()
    // with the team\x1fconv token we encoded in maybeNotify(). (Windows delivers
    // the same token via msga://notif protocol activation → handleNotifToken.)
    _desktopNotifier = new DesktopNotifier(this);
    connect(
        _desktopNotifier,
        &DesktopNotifier::submissionFinished,
        this,
        [this](const QString &token, const QString &error) {
            if (token == QLatin1String("msga-notification-test"))
                _settingsDialog->setNotificationTestResult(
                    error.isEmpty() ? tr("Accepted by macOS. If no banner appears, check Focus and "
                                         "notification settings.")
                                    : tr("Notification status: %1").arg(error)
                );
        }
    );
    connect(_desktopNotifier, &DesktopNotifier::activated, this, &MainWindow::handleNotifToken);
}

void MainWindow::handleNotifToken(const QString &token) {
    // A huddle "Join" button carries "join\x1f<url>": open the huddle straight
    // in the browser without raising the window (the user asked to join, not to
    // read the chat). URLs never contain the 0x1f unit separator.
    static const QString kJoinPrefix = QStringLiteral("join") + QChar(0x1f);
    if (token.startsWith(kJoinPrefix)) {
        const QString url = token.mid(kJoinPrefix.size());
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
        return;
    }
    // "Session expired" click: bring the window back so the login screen is in
    // view. The expired workspace may be a background one (the window still
    // shows another workspace) — re-activating it runs the same auth attempt
    // the switcher click would, which fails and lands on the logged-out page
    // for that workspace. A workspace logged out meanwhile is skipped.
    if (const auto teamId = decodeReloginNotifToken(token)) {
        restoreFromTray();
        const auto key = WorkspaceKey::fromString(*teamId);
        if (key && *teamId != _activeTeamId && TokenStore::loadWorkspace(*key))
            activateWorkspace(*teamId);
        return;
    }
    if (const auto t = decodeNotifToken(token))
        openNotifTarget(t->teamId, t->conv, t->threadRoot, t->msgTs);
}

void MainWindow::forwardMessage(const ConversationId &sourceConv, const Message &msg) {
    if (!_session)
        return;
    // A backend that allows it (Claude Code) forwards into any live workspace,
    // listed in switcher order; everyone else stays within its own.
    std::vector<ForwardDialog::Workspace> workspaces;
    const auto                            sourceKey = WorkspaceKey::fromString(_session->teamId());
    const auto *desc = sourceKey ? backends::find(sourceKey->service) : nullptr;
    if (desc && desc->forwardAnywhere) {
        for (const auto &key : TokenStore::workspaceKeys()) {
            const auto it = _sessions.find(key.toString());
            if (it == _sessions.end() || !it->second.session)
                continue;
            QString name = recordForHandle(it->first).displayName;
            if (name.isEmpty())
                name = backends::displayName(key.service);
            workspaces.push_back({it->second.session.get(), name});
        }
    }
    auto *dlg = new ForwardDialog(msg, _session, std::move(workspaces), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    _forwardDialog = dlg;
    connect(
        dlg, &AppDialog::accepted, this, [this, dlg, msg, sourceConv, source = _session->teamId()] {
            const ConversationId target = dlg->targetConv();
            // Still live: dropSession closes the dialog before freeing its sessions.
            Session             *ts     = dlg->targetSession();
            if (target.value.isEmpty() || !ts)
                return;
            // Email (Model-D): forwarding to a channel labels the original
            // message rather than re-posting its text (imap-backend-plan §3) —
            // only possible within the message's own workspace.
            const Conversation *tc = ts->findConversation(target);
            const bool          isChannel =
                tc && (tc->kind == ConvKind::PublicChannel || tc->kind == ConvKind::PrivateChannel);
            if (isChannel && ts->teamId() == source && ts->channelsAreLabels()) {
                ts->labelMessage(sourceConv, msg.ts, target, [this](bool ok, QString) {
                    if (!ok)
                        showNetworkError(tr("Couldn't apply the label."));
                });
                return;
            }
            const QString comment = dlg->comment();
            const QString fwd     = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
            const QString full    = comment.isEmpty() ? fwd : (comment + "\n" + fwd);
            ts->sendMessage(target, full);
        }
    );
    dlg->open();
}

void MainWindow::moveMessageToThread(const Message &msg) {
    if (!_session || _currentConvId.value.isEmpty())
        return;
    const ConversationId conv = _currentConvId;

    std::vector<ThreadChoice> choices;
    for (const Message &root : _messageList->threadRoots()) {
        if (root.ts == msg.ts)
            continue;
        ThreadChoice c;
        c.root = root.ts;
        // Readable text, not the parsed plain form: that still carries raw
        // "@U0BK…" mention ids and ":name:" emoji codes.
        c.text = MsgRender::notificationText(root.text, _session);
        if (c.text.trimmed().isEmpty() && !root.files.empty())
            c.text = root.files.front().name;
        c.date       = root.date;
        c.replyCount = root.replyCount;
        if (!root.author.value.isEmpty()) {
            c.author = _session->userDisplayName(root.author);
            if (const User *u = _session->findUser(root.author))
                c.avatarUrl = u->avatarUrl;
        } else {
            c.author    = root.botName;
            c.avatarUrl = root.botAvatarUrl;
        }
        choices.push_back(std::move(c));
    }

    auto *dlg = new MoveToThreadDialog(std::move(choices), _imgCache, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &AppDialog::accepted, this, [this, dlg, msg, conv] {
        const Ts root = dlg->selectedRoot();
        if (root.isEmpty() || !_session)
            return;
        _session->moveMessageToThread(conv, msg, root, dlg->addNote());
        // Show where it went — the copy appears there as it is confirmed.
        openThreadPanel(conv, root);
    });
    dlg->open();
}

void MainWindow::openThreadPanel(const ConversationId &conv, const Ts &rootTs) {
    if (rootTs.isEmpty())
        return;
    const bool wasOpen = _threadPanel->isVisible();
    _threadPanel->setVisible(true);
    _threadPanel->openThread(conv, rootTs);
    _messageList->setOpenThreadRoot(rootTs);
    if (!wasOpen) {
        const int total   = _msgSplitter->width();
        const int desired = QSettings("msga", "msga").value("window/threadWidth", 360).toInt();
        const int width   = std::clamp(desired, 100, std::max(100, total - 200));
        _msgSplitter->setSizes({total - width, width});
    }
}

void MainWindow::leaveConversationForOverview() {
    if (_searchWidget && _searchWidget->isVisible())
        _searchWidget->hide();

    // Stash the leaving conversation's unsent input — the same bookkeeping as
    // openConversation(), which restores it on the way back.
    stashComposerDraft();
    _currentConvId = {};
    // Deliberate leave, not "nothing open yet": the conversations() subscriber
    // must not treat the empty id as a first populate and restore the last chat.
    _contentView   = ContentView::Overview;

    if (_typingIndicator)
        _typingIndicator->clearAll();
    if (_threadPanel && _threadPanel->isVisible()) {
        _threadPanel->close();
        _threadPanel->setVisible(false);
        _messageList->setOpenThreadRoot({});
    }
    // No conversation is open while the overview shows — stop the mark-read
    // cursor and let the realtime safety poll idle (both restored by the next
    // openConversation).
    _session->setReading({});
    _session->setOpenConversation({});

    if (_canvasPage)
        _canvasPage->flushPendingSave();

    // The overview brings its own header; the conversation chrome would all
    // refer to a chat that's no longer on screen.
    if (_msgHeader)
        _msgHeader->hide();
    if (_convTabs)
        _convTabs->hide();
    if (_huddleBanner)
        _huddleBanner->hide();
}

void MainWindow::openThreadsView() {
    if (!_session || !_threadsPage)
        return;

    leaveConversationForOverview();
    // The page brings its own per-thread reply boxes.
    _composer->hide();
    _composer->setEnabled(false);

    _contentStack->setCurrentWidget(_threadsPage);
    _threadsPage->open();
}

void MainWindow::openSavedMessagesView() {
    if (!_session || !_savedPage)
        return;

    leaveConversationForOverview();
    _composer->hide();
    _composer->setEnabled(false);

    _contentStack->setCurrentWidget(_savedPage);
    _savedPage->open();
}

bool MainWindow::teammateViewOpen() const {
    return _teammatePage && _contentView == ContentView::Overview &&
           _contentStack->currentWidget() == _teammatePage;
}

ConversationId MainWindow::teammateDraftConv(const QString &role) {
    return ConversationId{QStringLiteral("teammate:") + role};
}

void MainWindow::openTeammateView(const QString &role) {
    if (!_session || !_teammatePage)
        return;
    AgentRole mate;
    for (const AgentRole &r : _session->backend()->agentRoles())
        if (r.id == role)
            mate = r;
    if (mate.id.isEmpty())
        return;

    // Unlike the other overview pages the composer stays: writing to a
    // teammate starts a session with it.
    leaveConversationForOverview();
    _convList->setSelectedTeammate(mate.id);

    _contentStack->setCurrentWidget(_teammatePage);
    _teammatePage->open(mate);
    _composer->show();
    _composerLockReason.clear(); // re-derived when a chat opens
    _composerSuggestion.clear();
    applyTeammateComposer();
    _composer->restoreDraft(_drafts.value(draftKey(_activeTeamId, teammateDraftConv(mate.id))));
    focusComposerIfActive();
}

void MainWindow::refreshTeammates() {
    if (!_convList || !_session)
        return;
    const auto team = _session->capabilities().agentSessions ? _session->backend()->agentRoles()
                                                             : std::vector<AgentRole>{};
    _convList->setTeammates(team);
    if (!teammateViewOpen())
        return;
    // The page shows the teammate as it is now — or, once it's off the team,
    // the Generalist.
    const QString open = _teammatePage->teammate().id;
    for (const AgentRole &r : team)
        if (r.id == open) {
            _teammatePage->open(r);
            applyTeammateComposer();
            return;
        }
    if (!team.empty())
        openTeammateView(team.front().id);
}

void MainWindow::editTeammate(const QString &id) {
    if (!_session)
        return;
    AgentRole role;
    if (id.isEmpty()) {
        role.glyph = QStringLiteral("pen-tool"); // a starting point, changed in the dialog
        role.color = QStringLiteral("#0e8c9a");
    } else {
        for (const AgentRole &r : _session->backend()->agentRoles())
            if (r.id == id)
                role = r;
        if (role.id.isEmpty())
            return;
    }
    TeammateDialog dlg(role, this);
    if (dlg.exec() != QDialog::Accepted || !_session)
        return;
    if (dlg.restoreRequested()) {
        _session->backend()->restoreAgentRole(id);
        refreshTeammates();
        return;
    }
    QString       error;
    const QString saved = _session->backend()->saveAgentRole(dlg.role(), &error);
    if (saved.isEmpty()) {
        showNetworkError(error);
        return;
    }
    refreshTeammates();
    if (id.isEmpty())
        openTeammateView(saved); // a new teammate: straight to its page, ready to write to
}

void MainWindow::removeTeammate(const QString &id) {
    if (!_session)
        return;
    QString name;
    for (const AgentRole &r : _session->backend()->agentRoles())
        if (r.id == id)
            name = r.name;
    if (name.isEmpty())
        return;
    RemoveTeammateDialog dlg(name, this);
    if (dlg.exec() != QDialog::Accepted || !_session)
        return;
    _session->backend()->removeAgentRole(id);
    refreshTeammates();
}

void MainWindow::applyTeammateComposer() {
    if (!teammateViewOpen())
        return;
    const QString blocker = _teammatePage->blocker();
    _composer->setEnabled(blocker.isEmpty());
    _composer->setPlaceholderText(
        blocker.isEmpty() ? tr("Message %1").arg(_teammatePage->teammate().name) : blocker
    );
}

void MainWindow::startSessionWithTeammate(const QString &text, const QStringList &filePaths) {
    if (!_session || !_teammatePage || (text.trimmed().isEmpty() && filePaths.isEmpty()))
        return;
    const QString role = _teammatePage->teammate().id;
    const QString dir  = _teammatePage->folder();
    QSettings("msga", "msga").setValue("claudeCode/lastDir", dir);
    _session->startAgentSession(
        dir,
        false,
        role,
        [this, text, filePaths](ConversationId id) {
            // Listed by now (the backend announces a session before this):
            // open it, then the text is its first message.
            if (!_session || !_convList || !_convList->selectConversation(id))
                return;
            if (filePaths.isEmpty())
                _session->sendMessage(id, text, std::nullopt, {});
            else
                _session->uploadFiles(id, filePaths, text);
        },
        [this, text](const QString &err) {
            showNetworkError(err);
            applyTeammateComposer();
        }
    );
}

void MainWindow::updateSavedMessagesEntry() {
    if (!_convList || !_session)
        return;
    _convList->setShowSavedMessages(
        _session->capabilities().messageReminders && !_session->messageReminders().empty()
    );
}

void MainWindow::openMessageTarget(const ConversationId &conv, const Ts &ts, const Ts &threadRoot) {
    if (!_convList || conv.value.isEmpty() || ts.isEmpty())
        return;
    // Runs once the conversation is the selected row: make the view follow, then
    // move the focus to the message. A thread reply is never in the channel
    // timeline (conversations.history omits replies), so it can only be focused
    // inside its thread.
    const auto focus = [this, conv, ts, threadRoot] {
        if (_currentConvId != conv) {
            const int row = _convList->rowForId(conv);
            if (row >= 0)
                openConversation(row);
        }
        if (threadRoot.isEmpty()) {
            _messageList->jumpToTs(ts);
        } else {
            openThreadPanel(conv, threadRoot);
            _threadPanel->jumpToTs(ts);
        }
    };
    if (_convList->selectConversation(conv)) {
        focus();
        return;
    }
    // Not in the sidebar — a public channel we never joined. Join it first, the
    // same flow as following a #channel mention.
    if (!_session)
        return;
    _session->joinChannel(
        conv,
        [this, focus](ConversationId joined) {
            _convList->selectConversation(joined);
            focus();
        },
        [this](const QString &err) { showNetworkError(err); }
    );
}

void MainWindow::openConversationIn(const QString &teamId, const ConversationId &conv) {
    // The conversation may belong to a background workspace — bring it up first.
    if (!teamId.isEmpty() && teamId != _activeTeamId) {
        if (!_sessions.count(teamId))
            return; // logged out meanwhile
        activateWorkspace(teamId);
    }
    if (!_convList)
        return;
    // Route through the conv list's selection rather than calling
    // openConversation(row) directly. selectConversation() moves the list's
    // highlight to the target and emits conversationSelected, which drives
    // openConversation() (message list + header) — so the header title, the
    // header avatar AND the highlighted row all land on the target
    // conversation together. Opening the message list directly would switch
    // the messages but leave the header and the list selection on the
    // previously-open conversation. selectConversation() also un-collapses
    // the section and overrides the relevance filter, so a conversation the
    // filter has hidden (a DM with no recent activity) still opens —
    // rowForId() alone would return -1 and silently do nothing.
    _convList->selectConversation(conv);
    // selectConversation() suppresses the signal when the target row is
    // already the selected one (e.g. a stale highlight left over from a
    // workspace switch); drive the open directly so the view still updates.
    if (_currentConvId != conv) {
        const int row = _convList->rowForId(conv);
        if (row >= 0)
            openConversation(row);
    }
}

void MainWindow::openNotifTarget(
    const QString &teamId, const ConversationId &conv, const Ts &threadRoot, const Ts &msgTs
) {
    show();
    raise();
    activateWindow();
    if (conv.value.isEmpty())
        return;
    openConversationIn(teamId, conv);
    // A thread-reply notification: the reply isn't in the channel timeline, so
    // open the thread it belongs to (no-op for a plain message — empty root).
    openThreadPanel(conv, threadRoot);

    // A reminder click carries the exact message — scroll to it and flash it.
    // jumpToTs remembers the target when the history hasn't loaded yet, so the
    // jump still lands once the open above delivers its first page (best-effort:
    // a message buried beyond the first page just leaves the chat open).
    if (!msgTs.isEmpty()) {
        if (threadRoot.isEmpty()) {
            if (_messageList)
                _messageList->jumpToTs(msgTs);
        } else if (_threadPanel) {
            _threadPanel->jumpToTs(msgTs);
        }
    }
}

// ── Event handlers ────────────────────────────────────────────────────────────

static Qt::Edges resizeEdgesAt(const QPoint &pos, const QSize &sz) {
    Qt::Edges edges;
    if (pos.x() < kResizeBorder)
        edges |= Qt::LeftEdge;
    if (pos.x() >= sz.width() - kResizeBorder)
        edges |= Qt::RightEdge;
    if (pos.y() < kResizeBorder)
        edges |= Qt::TopEdge;
    if (pos.y() >= sz.height() - kResizeBorder)
        edges |= Qt::BottomEdge;
    return edges;
}

static Qt::CursorShape cursorForEdges(Qt::Edges edges) {
    if ((edges & Qt::TopEdge) && (edges & Qt::LeftEdge))
        return Qt::SizeFDiagCursor;
    if ((edges & Qt::TopEdge) && (edges & Qt::RightEdge))
        return Qt::SizeBDiagCursor;
    if ((edges & Qt::BottomEdge) && (edges & Qt::LeftEdge))
        return Qt::SizeBDiagCursor;
    if ((edges & Qt::BottomEdge) && (edges & Qt::RightEdge))
        return Qt::SizeFDiagCursor;
    if (edges & (Qt::LeftEdge | Qt::RightEdge))
        return Qt::SizeHorCursor;
    if (edges & (Qt::TopEdge | Qt::BottomEdge))
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e) {
    // Keep the window backdrop's mirrored light region aligned with the content
    // panel when it moves/resizes independently of the window (conv-panel drag,
    // show/hide). Non-consuming — fall through to the rest of the filter.
    if (obj == _rightArea && (e->type() == QEvent::Move || e->type() == QEvent::Resize) && _frame)
        _frame->update();
    // Real input anywhere in the app feeds the presence links (idle clock +
    // Slack's activity signal). Throttled here so the hot path stays a compare;
    // the links throttle their own tickles further.
    switch (e->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::Wheel: {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - _lastActivityNoteMs >= kActivityNoteGapMs) {
            _lastActivityNoteMs = now;
            for (auto &[teamId, ws] : _sessions)
                if (ws.session)
                    ws.session->noteUserActivity();
        }
        break;
    }
    default:
        break;
    }
    // Shift+Del on an idle Claude Code session: "Remove from msga". Filtered
    // here because the composer keeps focus once a session is opened. An editor
    // with a selection keeps the key as Cut; without one Cut is a no-op anyway.
    if (e->type() == QEvent::KeyPress &&
        Ui::Shortcuts::matches(Ui::Shortcut::RemoveIdleSession, static_cast<QKeyEvent *>(e))) {
        auto *w = qobject_cast<QWidget *>(obj);
        if (w && w->window() == this && _session && _session->capabilities().agentSessions &&
            !AppDialog::topmostVisible(this) &&
            !(_settingsDialog && _settingsDialog->isVisible())) {
            const QWidget *focus        = QApplication::focusWidget();
            bool           hasSelection = false;
            if (auto *te = qobject_cast<const QTextEdit *>(focus))
                hasSelection = te->textCursor().hasSelection();
            else if (auto *pe = qobject_cast<const QPlainTextEdit *>(focus))
                hasSelection = pe->textCursor().hasSelection();
            else if (auto *le = qobject_cast<const QLineEdit *>(focus))
                hasSelection = le->hasSelectedText();
            if (!hasSelection && _convList->removeSelectedIdleSession())
                return true;
        }
    }
    // Mouse side buttons anywhere in this window navigate chat history.
    if (e->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::BackButton || me->button() == Qt::ForwardButton) {
            auto *w = qobject_cast<QWidget *>(obj);
            if (w && w->window() == this) {
                navigateHistory(me->button() == Qt::BackButton);
                return true;
            }
        }
    }
    if (windowFlags().testFlag(Qt::FramelessWindowHint) && !isMaximized() && !isFullScreen()) {
        auto *w = qobject_cast<QWidget *>(obj);
        if (w && w->window() == this) {
            if (e->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(e);
                if (me->button() == Qt::LeftButton && !_resizeEdges) {
                    const QPoint    fp    = _frame->mapFromGlobal(me->globalPosition().toPoint());
                    const Qt::Edges edges = resizeEdgesAt(fp, _frame->size());
                    if (edges) {
                        if (_resizeHoverCursor) {
                            QGuiApplication::restoreOverrideCursor();
                            _resizeHoverCursor = false;
                        }
                        if (QGuiApplication::platformName() == "wayland") {
                            if (auto *h = windowHandle())
                                h->startSystemResize(edges);
                        } else {
                            _resizeEdges     = edges;
                            _resizeDragStart = me->globalPosition().toPoint();
                            _resizeWinAtDrag = geometry();
                            _frame->grabMouse(cursorForEdges(edges));
                        }
                        return true;
                    }
                }
            } else if (e->type() == QEvent::MouseMove) {
                auto *me = static_cast<QMouseEvent *>(e);
                if (_resizeEdges) {
                    const QPoint delta = me->globalPosition().toPoint() - _resizeDragStart;
                    QRect        r     = _resizeWinAtDrag;
                    if (_resizeEdges & Qt::LeftEdge)
                        r.setLeft(r.left() + delta.x());
                    if (_resizeEdges & Qt::RightEdge)
                        r.setRight(r.right() + delta.x());
                    if (_resizeEdges & Qt::TopEdge)
                        r.setTop(r.top() + delta.y());
                    if (_resizeEdges & Qt::BottomEdge)
                        r.setBottom(r.bottom() + delta.y());
                    const QSize minS = minimumSize();
                    if (r.width() < minS.width())
                        (_resizeEdges & Qt::LeftEdge) ? r.setLeft(r.right() - minS.width())
                                                      : r.setRight(r.left() + minS.width());
                    if (r.height() < minS.height())
                        (_resizeEdges & Qt::TopEdge) ? r.setTop(r.bottom() - minS.height())
                                                     : r.setBottom(r.top() + minS.height());
                    setGeometry(r);
                    return true;
                }
                const QPoint    fp    = _frame->mapFromGlobal(me->globalPosition().toPoint());
                const Qt::Edges edges = resizeEdgesAt(fp, _frame->size());
                if (edges) {
                    if (_resizeHoverCursor)
                        QGuiApplication::changeOverrideCursor(cursorForEdges(edges));
                    else {
                        QGuiApplication::setOverrideCursor(cursorForEdges(edges));
                        _resizeHoverCursor = true;
                    }
                } else if (_resizeHoverCursor) {
                    QGuiApplication::restoreOverrideCursor();
                    _resizeHoverCursor = false;
                }
            } else if (e->type() == QEvent::MouseButtonRelease) {
                auto *me = static_cast<QMouseEvent *>(e);
                if (me->button() == Qt::LeftButton && _resizeEdges) {
                    _resizeEdges = {};
                    _frame->releaseMouse();
                    return true;
                }
            } else if (e->type() == QEvent::Leave && obj == _frame && _resizeHoverCursor) {
                QGuiApplication::restoreOverrideCursor();
                _resizeHoverCursor = false;
            }
        }
    }
    if (obj == _contentStack && e->type() == QEvent::Resize) {
        if (_searchWidget && _searchWidget->isVisible())
            repositionSearch();
    }
    if (obj == _membersBtn && _membersBtnTooltip) {
        if (e->type() == QEvent::Enter)
            _membersBtnTooltip->showAbove(
                tr("View members"),
                QRect(_membersBtn->mapToGlobal(QPoint(0, 0)), _membersBtn->size())
            );
        else if (e->type() == QEvent::Leave || e->type() == QEvent::MouseButtonPress)
            _membersBtnTooltip->hide();
    }
    // A group DM's stacked avatars are its members button. The press is taken
    // too: left to bubble up, it reaches the macOS title bar the header sits in
    // and starts a window drag instead of a click.
    if (obj == _headerAvatar &&
        (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease) &&
        static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton &&
        !_headerAvatar->group().empty() && _session && _session->capabilities().memberList) {
        if (e->type() == QEvent::MouseButtonRelease)
            openMembersPopup(
                QRect(_headerAvatar->mapToGlobal(QPoint(0, 0)), _headerAvatar->size())
            );
        return true;
    }
    if (obj == _huddleBtn && _huddleBtnTooltip) {
        if (e->type() == QEvent::Enter)
            _huddleBtnTooltip->showAbove(
                tr("Opens the huddle in Slack for web"),
                QRect(_huddleBtn->mapToGlobal(QPoint(0, 0)), _huddleBtn->size())
            );
        else if (e->type() == QEvent::Leave)
            _huddleBtnTooltip->hide();
    }
    if (obj == _starBtn && _starBtnTooltip) {
        if (e->type() == QEvent::Enter) {
            const auto   *conv    = _session ? _session->findConversation(_currentConvId) : nullptr;
            const bool    starred = conv && conv->isStarred;
            const QString text    = starred ? tr("Unstar conversation") : tr("Star conversation");
            _starBtnTooltip->showAbove(
                text, QRect(_starBtn->mapToGlobal(QPoint(0, 0)), _starBtn->size())
            );
        } else if (e->type() == QEvent::Leave) {
            _starBtnTooltip->hide();
        }
    }
    if (obj == _searchBtn && _searchBtnTooltip) {
        if (e->type() == QEvent::Enter)
            _searchBtnTooltip->showAbove(
                tr("Search messages"),
                QRect(_searchBtn->mapToGlobal(QPoint(0, 0)), _searchBtn->size())
            );
        else if (e->type() == QEvent::Leave)
            _searchBtnTooltip->hide();
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::updateRoundedMask() {
    if (!_frame)
        return;
    // Native frames own their corners; only frameless windows need the inset
    // border and client-area mask.
    const bool windowed =
        windowFlags().testFlag(Qt::FramelessWindowHint) && !isMaximized() && !isFullScreen();
    const auto &sp = Th::c().spacing;
    if (_rightPanelLayout)
        _rightPanelLayout->setContentsMargins(0, 0, windowed ? sp.sm : 0, windowed ? sp.sm : 0);
    if (_loggedOutPageLayout)
        _loggedOutPageLayout->setContentsMargins(0, 0, windowed ? sp.sm : 0, windowed ? sp.sm : 0);
    if (!windowed) {
        _frame->clearMask();
        return;
    }
    static constexpr int kRadius = 8;
    QBitmap              bmp(_frame->size());
    bmp.fill(Qt::color0);
    QPainter p(&bmp);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::color1);
    p.drawRoundedRect(_frame->rect(), kRadius, kRadius);
    _frame->setMask(bmp);
}

void MainWindow::resizeEvent(QResizeEvent *e) {
    QMainWindow::resizeEvent(e);
    updateRoundedMask();
}

// ── Screen fit ────────────────────────────────────────────────────────────────

// The screen a window rect belongs to: the one it overlaps most, falling back to
// the one under the pointer (where a not-yet-mapped window will most likely land)
// and finally the primary.
static QScreen *screenForRect(const QRect &r) {
    QScreen *best     = nullptr;
    int      bestArea = 0;
    for (QScreen *s : QGuiApplication::screens()) {
        const QRect i = s->geometry().intersected(r);
        const int   a = i.width() * i.height();
        if (a > bestArea) {
            bestArea = a;
            best     = s;
        }
    }
    if (!best)
        best = QGuiApplication::screenAt(QCursor::pos());
    return best ? best : QGuiApplication::primaryScreen();
}

// Shrink-only fit. A screen roomier than the window is left alone entirely — no
// resize, no recentring, no maximizing on the user's behalf; only a window that
// does NOT fit gets pulled in.
//
// This matters more for us than for a decorated app: frameless means the only
// resize affordances are our own 6px hot border (resizeEdgesAt) and the titlebar
// buttons. A window taller than the work area has its bottom border below the
// screen and, depending on where the WM parks it, its titlebar above the top —
// at which point there is nothing left to grab and the user is stuck with it.
// That was issue #45 ("login window too big for my laptop screen and could not
// resize it": a 1200x800 default against, say, 1920x1080 at 150% scaling, which
// is 1280x720 of logical room).
void MainWindow::fitToScreen() {
    // Maximized/fullscreen geometry is the windowing system's business, and
    // saveGeometry/restoreGeometry carry the normal geometry separately.
    if (isMaximized() || isFullScreen())
        return;

    const QScreen *scr = screenForRect(frameGeometry());
    if (!scr)
        return;
    const QRect avail = scr->availableGeometry();
    if (avail.isEmpty())
        return;

    // The minimum has to give way first: a floor bigger than the screen makes the
    // window unshrinkable by construction, whatever we then do to its size. Both
    // panels have real minimums of their own (kConvMinWidth et al), so the layout
    // squeezes rather than clips.
    setMinimumSize(
        std::min(kPreferredMinSize.width(), avail.width()),
        std::min(kPreferredMinSize.height(), avail.height())
    );

    // Frame margins are zero while frameless, but don't bake that in: resize()
    // and move() speak client coordinates, the fit is about the frame.
    const QSize  frameExtra = frameGeometry().size() - size();
    const QPoint frameOff   = geometry().topLeft() - frameGeometry().topLeft();

    const QSize want(
        std::min(frameGeometry().width(), avail.width()),
        std::min(frameGeometry().height(), avail.height())
    );
    if (want != frameGeometry().size())
        resize(want - frameExtra);

    // Only nudge a window that is actually hanging off the work area; one that
    // fits stays exactly where the user (or the WM) put it. No-op on Wayland,
    // where move() is ignored — but there the compositor keeps us on screen
    // anyway, and it is the resize above that does the real work.
    const QRect  f = frameGeometry();
    const QPoint p(
        std::clamp(f.left(), avail.left(), std::max(avail.left(), avail.right() - f.width() + 1)),
        std::clamp(f.top(), avail.top(), std::max(avail.top(), avail.bottom() - f.height() + 1))
    );
    if (p != f.topLeft())
        move(p + frameOff);
}

// Tray rescue: back to the default size (fitted), centred on the current screen.
// Unlike fitToScreen() this one is a deliberate user request, so it may grow the
// window as well as move it.
void MainWindow::resetWindowGeometry() {
    if (isMaximized() || isFullScreen())
        showNormal();
    resize(kDefaultWindowSize);
    fitToScreen();
    if (const QScreen *scr = screenForRect(frameGeometry())) {
        const QRect avail = scr->availableGeometry();
        const QRect f     = frameGeometry();
        move(
            avail.center() - QPoint(f.width() / 2, f.height() / 2) +
            (geometry().topLeft() - f.topLeft())
        );
    }
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (!_screenFitWired) {
        if (QWindow *h = windowHandle()) {
            _screenFitWired = true;
            // Dragged to a smaller monitor, or the current one changed resolution
            // / gained a panel: re-fit either way.
            connect(h, &QWindow::screenChanged, this, [this](QScreen *) { fitToScreen(); });
            connect(qApp, &QGuiApplication::primaryScreenChanged, this, [this](QScreen *) {
                fitToScreen();
            });
            // A screen can also shrink under us without the window moving: a
            // resolution change, or a panel/taskbar appearing.
            const auto wireScreen = [this](QScreen *s) {
                connect(s, &QScreen::availableGeometryChanged, this, [this](const QRect &) {
                    fitToScreen();
                });
            };
            for (QScreen *s : QGuiApplication::screens())
                wireScreen(s);
            connect(qApp, &QGuiApplication::screenAdded, this, wireScreen);
        }
    }
    // The constructor's fit ran before the window had a handle, so on some
    // platforms it was working from a placeholder geometry. Now it is real.
    fitToScreen();
}

void MainWindow::changeEvent(QEvent *e) {
    if (e->type() == QEvent::WindowStateChange) {
        updateRoundedMask();
        // Minimize delivers neither hideEvent nor paint events to children, so
        // the message lists can't notice on their own — stop GIF decoding here
        // or the QMovies keep burning CPU the whole time the window is down.
        // Un-minimizing repaints, which restarts the visible players.
        if (isMinimized()) {
            if (_messageList)
                _messageList->pauseGifPlayback();
            if (_threadPanel)
                _threadPanel->pauseGifPlayback();
            // Minimize-to-tray (Settings → System, GitHub issue #55): tuck the
            // window away like the close button does, so the taskbar button
            // disappears and only the tray icon remains. The tray icon is the
            // only way back, so never do this without one. Deferred: hiding
            // synchronously inside the state change is unreliable on Windows
            // (the platform plugin is still mid-way through its own minimize).
            // Wayland never reports the minimized state to Qt, so this cannot
            // fire there — the toggle is documented as inert on Wayland.
            if (_trayIcon &&
                QSettings("msga", "msga").value("window/minimizeToTray", false).toBool())
                QTimer::singleShot(0, this, [this] {
                    if (isMinimized())
                        hide();
                });
        }
    } else if (e->type() == QEvent::ActivationChange && _session) {
        if (isActiveWindow()) {
            // Coming back to the app is when "how do I look to others" matters most.
            _session->refreshSelfPresence();
            // The open conversation is visible again — clear and mark it read.
            _session->setReading(_currentConvId);
            // Switching back to an open conversation: put the cursor straight in
            // the composer so the user can start typing without an extra click.
            focusComposerIfActive();
        } else {
            // Window in background: let the open conversation accrue unreads
            // and fire notifications, like the official client does. Clear only
            // the mark-read cursor — NOT setOpenConversation — so the realtime
            // safety poll keeps covering the still-open chat. (A reply to an
            // open-but-unfocused chat must still arrive even when the realtime
            // socket silently stopped routing events; the poll is what recovers
            // it, and it must not go dark just because the window lost focus.)
            _session->setReading({});
        }
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent *e) {
    QSettings("msga", "msga").setValue("window/geometry", saveGeometry());
    hide();
    e->ignore();
}

void MainWindow::populateConversations(const std::vector<Conversation> &convs) {
    _convList->setConversations(convs);
}

void MainWindow::focusComposerIfActive() {
    // Skip when the canvas page is up (composer hidden) or nothing is open.
    if (isActiveWindow() && !_currentConvId.value.isEmpty() && _composer &&
        _composer->isVisible() && _composer->isEnabled()) {
        _composer->focusInput();
    }
}

void MainWindow::stashComposerDraft() {
    if (!_composer)
        return;
    // Always take, even when nothing is open: emptying the composer is the
    // point — whatever it held must not survive into the next conversation.
    const ComposerDraft  draft = _composer->takeDraft();
    // A teammate's page keeps what was being written to it, like a chat.
    const ConversationId under =
        teammateViewOpen() ? teammateDraftConv(_teammatePage->teammate().id) : _currentConvId;
    if (_activeTeamId.isEmpty() || under.value.isEmpty())
        return; // no conversation to file it under: discard
    const QString key = draftKey(_activeTeamId, under);
    if (draft.isEmpty())
        _drafts.remove(key);
    else
        _drafts.insert(key, draft);
}

void MainWindow::openConversation(int row) {
    if (!_session)
        return;

    if (_searchWidget && _searchWidget->isVisible())
        _searchWidget->hide();

    // Stash the outgoing conversation's unsent input (also exits edit mode).
    stashComposerDraft();

    _currentConvId = _convList->conversationId(row);
    if (_currentConvId.value.isEmpty())
        return;
    _contentView = ContentView::Conversation;

    // Typing is per-conversation; forget whoever was typing in the old one.
    if (_typingIndicator)
        _typingIndicator->clearAll();

    // The thread view belongs to the conversation we're leaving; close it so we
    // don't show another chat's replies alongside the newly-opened one.
    if (_threadPanel && _threadPanel->isVisible()) {
        _threadPanel->close();
        _threadPanel->setVisible(false);
    }

    // Track navigation history.  Jumps applied by navigateHistory() keep the
    // forward stack (like editor undo/redo); direct opens discard it.
    if (_navApplying)
        _navHistory.setCurrent({_activeTeamId, _currentConvId});
    else
        _navHistory.recordOpen({_activeTeamId, _currentConvId});

    const QString name = _convList->resolvedName(row);
    const auto   *conv = _session->findConversation(_currentConvId);
    const bool    isDm = conv && (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
    const QString displayName = isDm ? name : name.isEmpty() ? "" : "#" + name;

    // Keep the conversation header title in lock-step with the message list no
    // matter how this open was triggered. Programmatic opens (notification
    // click, search result, history navigation) reach this slot directly rather
    // than through the conversationSelected signal, so setting the title here —
    // the single point every open passes through — is what stops the header
    // from being left on the previously-open conversation.
    if (_convNameLabel)
        _convNameLabel->setText(displayName);

    const bool hasCachedMsgs = !_session->cachedMessages(_currentConvId).empty();

    // Capture the unread boundary before setReading() advances lastRead, so the
    // message list can open scrolled to the first unread message.
    Ts lastReadTs;
    if (conv && (conv->unread > 0 || conv->mentionCount > 0) && !conv->lastRead.isEmpty())
        lastReadTs = conv->lastRead;

    // setReading() reassigns the conversations rpl::variable, reallocating the
    // backing vector and invalidating `conv`. Snapshot everything we still need
    // from it before that point; using `conv` afterwards is a use-after-free.
    const QString  convCanvasFileId = conv ? conv->canvasFileId : QString();
    const ConvKind convKind         = conv ? conv->kind : ConvKind::PublicChannel;
    const QString  convReplySubject = conv ? conv->replySubject : QString();

    _session->setReading(_currentConvId);
    if (_canvasPage)
        _canvasPage->flushPendingSave(); // outgoing conversation's canvas edits
    if (_contentStack)
        _contentStack->setCurrentWidget(_messageList);
    _messageList->openConversation(_currentConvId, lastReadTs);

    // Reset the tab strip to Messages and look up this conversation's canvas.
    // conversations.list often omits "properties", so the cached Conversation
    // only seeds the tab; conversations.info is authoritative.
    if (_convTabs) {
        _convTabs->setActiveTab(ConvTabsWidget::Tab::Messages);
        _currentCanvasFileId = convCanvasFileId;
        _currentCanvasTitle.clear();
        // Bot/app DMs can't own a user-editable channel canvas — any canvas
        // conversations.info advertises for them is app-owned and answers
        // not_visible. Hide the tab and skip the doomed conversations.info +
        // files.info probe entirely.
        const Conversation *convForCanvas = _session->findConversation(_currentConvId);
        if (!_session->capabilities().canvases ||
            (convForCanvas && _session->isAppConversation(*convForCanvas))) {
            _currentCanvasFileId.clear();
            _convTabs->setCanvasTabVisible(false);
        } else {
            _convTabs->setCanvasTabVisible(true);
            _convTabs->setCanvasInfo(!_currentCanvasFileId.isEmpty());
            _session->loadChannelCanvas(
                _currentConvId, [this, convId = _currentConvId](QString fileId, bool) {
                    if (_currentConvId != convId)
                        return;
                    _currentCanvasFileId = fileId;
                    _convTabs->setCanvasInfo(!fileId.isEmpty());
                    if (fileId.isEmpty())
                        return;
                    _session->loadCanvasMeta(
                        fileId,
                        [this, convId, fileId](QString title, QString, CanvasMetaState state) {
                            if (_currentConvId != convId || _currentCanvasFileId != fileId)
                                return;
                            if (state == CanvasMetaState::Gone) {
                                // conversations.info still references a deleted canvas.
                                _currentCanvasFileId.clear();
                                _currentCanvasTitle.clear();
                                _convTabs->setCanvasInfo(false);
                                return;
                            }
                            // NoAccess keeps the tab — the canvas exists, the page
                            // shows the read-only no-access notice when opened.
                            _currentCanvasTitle = title;
                            _convTabs->setCanvasInfo(true, title);
                        }
                    );
                }
            );
        }
    }
    _composer->setEnabled(true);
    _composer->setConvKind(convKind);
    // Email backends compose per-message subjects (decision §3 #3): show the
    // subject line for DMs/MPDMs (new top-level mail). Channels are reply-only.
    _composer->setSubjectVisible(
        _session->capabilities().messageSubjects &&
        (convKind == ConvKind::Im || convKind == ConvKind::Mpim)
    );
    // Prefill the reply subject ("Re: …") so it's never empty and the user sees
    // which thread they're continuing; still editable, and required to send.
    if (_session->capabilities().messageSubjects)
        _composer->setSubjectText(convReplySubject);
    // Schedule-send is Slack-only (chat.scheduleMessage); hide the dropdown on
    // backends that can't honor it so the chevron isn't a dead control.
    _composer->setScheduleVisible(_session->capabilities().scheduledSend);
    _composer->setPlaceholderText(
        displayName.isEmpty() ? tr("Message") : tr("Message %1").arg(displayName)
    );
    _composerLockReason.clear();
    _composerSuggestion.clear(); // takeDraft dropped the last one's
    applyComposerAccess();

    // Restore this conversation's unsent input (text + attachments). Applied
    // wholesale even when there is no stash, so nothing staged elsewhere and
    // missed by a stash call could still be sitting in the composer.
    _composer->restoreDraft(_drafts.value(draftKey(_activeTeamId, _currentConvId)));

    if (hasCachedMsgs) {
        if (_msgHeader)
            _msgHeader->show();
        if (_convTabs)
            _convTabs->show();
        _composer->show();
        focusComposerIfActive();
        updateHuddleBanner();
    } else {
        // Messages are loading; keep header and composer hidden until the first
        // page is ready so the user doesn't see chrome around an empty chat area.
        if (_msgHeader)
            _msgHeader->hide();
        if (_convTabs)
            _convTabs->hide();
        _composer->hide();
        auto reveal = [this, convId = _currentConvId] {
            if (_currentConvId != convId)
                return;
            if (_msgHeader)
                _msgHeader->show();
            if (_convTabs)
                _convTabs->show();
            if (_composer)
                _composer->show();
            focusComposerIfActive();
            updateHuddleBanner();
        };
        // A backend answering synchronously (Claude Code reads local files)
        // already delivered the page inside openConversation() above.
        if (_messageList->initialPageDone())
            reveal();
        else
            connect(
                _messageList,
                &MessageListWidget::initialPageLoaded,
                this,
                reveal,
                Qt::SingleShotConnection
            );
    }

    _session->saveLastConv(_currentConvId, displayName);
    updateHeaderForConv(_currentConvId);
}

void MainWindow::updateStarBtn(bool starred) {
    if (!_starBtn)
        return;
    const QString svg =
        starred ? QStringLiteral(":/ui/star-solid.svg") : QStringLiteral(":/ui/star.svg");
    _starBtn->setIcon(
        svgIcon(svg, QSize(15, 15), starred ? Th::c().icon.starred : Th::c().icon.def)
    );
}

QString MainWindow::huddleJoinUrl(const ConversationId &conv) const {
    return huddleJoinUrl(_activeTeamId, conv);
}

QString MainWindow::huddleJoinUrl(const QString &teamId, const ConversationId &conv) const {
    // Prefer the room's own `huddle_link` (Slack's authoritative "Copy huddle
    // link" URL, delivered on the huddle_thread realtime event / conversations.info
    // room object). It is the only join URL guaranteed to work — a link we build
    // by hand only reliably opens a *channel* huddle; the same /huddle/<team>/<id>
    // shape server-errors for a DM ("D…") id, which is why clicking a live DM
    // huddle used to land on Slack's "Server Error" page. We only fall back to a
    // constructed link when we have no authoritative one (e.g. starting a fresh
    // huddle, or an active huddle we learned about without room detail).
    const Conversation *c = _session ? _session->findConversation(conv) : nullptr;
    if (c && !c->huddleLink.isEmpty())
        return c->huddleLink;
    // Single source of truth for the link shape lives in SlackLinks::huddle().
    // `teamId` here is the app-wide canonical WorkspaceKey ("slack:TCF2J0TSP"),
    // but the URL needs the bare Slack team id ("TCF2J0TSP"), so unwrap it. The
    // team is taken explicitly because a notification may target a background
    // workspace, not the active one.
    const auto    key  = WorkspaceKey::fromString(teamId);
    const QString t    = key ? key->id : teamId;
    // A constructed /huddle/ link can only START a huddle for a channel; for a DM
    // with no live huddle it server-errors (see SlackLinks::huddle). So for a DM
    // fall back to opening the conversation — the user starts the huddle there.
    const bool    isDm = c && (c->kind == ConvKind::Im || c->kind == ConvKind::Mpim);
    return isDm ? SlackLinks::conversation(t, conv.value) : SlackLinks::huddle(t, conv.value);
}

void MainWindow::updateHuddleBanner() {
    if (!_huddleBanner)
        return;
    // Only alongside the conversation chrome — never floating over a loading or
    // empty message area. Huddle state rides on the conversation list (patched by
    // realtime huddle_thread events and re-derived from history on open); the
    // conversations() producer re-fires on any change, which calls here.
    const bool          chromeVisible = _msgHeader && _msgHeader->isVisible();
    const Conversation *conv = (chromeVisible && _session && !_currentConvId.value.isEmpty())
                                   ? _session->findConversation(_currentConvId)
                                   : nullptr;
    _huddleBanner->setVisible(
        conv && conv->huddleActive && _session && _session->capabilities().huddles
    );
}

void MainWindow::updateHeaderForConv(const ConversationId &conv) {
    if (conv.value.isEmpty())
        return;

    if (!_session)
        return;
    const auto *conversation = _session->findConversation(conv);

    // Star button state
    if (_starBtn) {
        _starBtn->setVisible(true);
        updateStarBtn(conversation ? conversation->isStarred : false);
    }
    // The "start huddle" header button only makes sense on a backend that has
    // huddles (Slack); a future service without them shows no dead control.
    if (_huddleBtn)
        _huddleBtn->setVisible(_session->capabilities().huddles);
    const bool isDm = conversation &&
                      (conversation->kind == ConvKind::Im || conversation->kind == ConvKind::Mpim);
    const bool memberList = conversation && _session->capabilities().memberList;
    if (_membersBtn) {
        const bool channel = memberList && (conversation->kind == ConvKind::PublicChannel ||
                                            conversation->kind == ConvKind::PrivateChannel);
        _membersBtn->setVisible(channel);
        // conversations.list's num_members; a large count is shortened so the
        // button fits the header's action column.
        const int n = channel ? conversation->memberCount : 0;
        _membersBtn->setText(
            n <= 0      ? QString()
            : n < 10000 ? QLocale().toString(n)
                        : tr("%1k").arg(n / 1000)
        );
    }

    if (_headerAvatar) {
        _headerAvatar->setVisible(isDm);
        _headerAvatar->clearAvatar();
        // Apps/bots have no presence — they can't go offline, so the dot is
        // meaningless and confusing for them. Likewise services with no presence
        // concept at all (email/IMAP), and group DMs (mpim), which have no single
        // peer whose presence the dot could mean. Decide this BEFORE (and
        // outside) the single-peer DM branch below: clearAvatar() reset the
        // state to the showPresence=true default, and an mpim or an
        // as-yet-unresolved peer would otherwise skip the branch and paint a
        // stray offline ring.
        _headerAvatar->setShowPresence(
            conversation && conversation->kind == ConvKind::Im &&
            _session->capabilities().presence && !_session->isAppConversation(*conversation)
        );
        if (conversation && conversation->kind == ConvKind::Mpim) {
            setHeaderGroupAvatars(*conversation);
            // conversations.list leaves the members out, and a group DM renamed
            // in Slack has no "mpdm-…" name to read them from: ask once, and
            // redraw with them (Session files them into the conversation).
            if (memberList && conversation->members.empty() && !_session->cachedMembers(conv)) {
                Session *s = _session;
                s->loadMembers(conv, [this, s, conv](std::vector<UserId>, QString) {
                    if (_session == s && _currentConvId == conv)
                        updateHeaderForConv(conv);
                });
            }
        }
        _headerAvatar->setCursor(
            memberList && !_headerAvatar->group().empty() ? Qt::PointingHandCursor : Qt::ArrowCursor
        );
        if (_headerAvatar->group().empty() && isDm && conversation->dmUser) {
            const auto *u = _session->findUser(*conversation->dmUser);
            if (u) {
                _headerAvatar->setPresence(u->isActive);
                _headerAvatar->setDnd(u->dndEnabled);
                const bool isSelf = *conversation->dmUser == _session->meUserId();
                const auto sp     = _session->currentSelfPresence();
                _headerAvatar->setPhantomAway((isSelf && sp.phantomAway()) || u->unavailable);
                _headerAvatar->setToolTip(isSelf ? selfPresenceTooltip(sp) : QString{});
                _headerAvatar->setDisplayName(u->displayName.isEmpty() ? u->name : u->displayName);
                _session->requestPresence(*conversation->dmUser);
                if (!u->avatarUrl.isEmpty() && _imgCache) {
                    const QPixmap cached = _imgCache->get(u->avatarUrl);
                    if (!cached.isNull()) {
                        _headerAvatar->setPixmap(cached);
                    } else {
                        // Not yet in cache — subscribe and apply when it arrives.
                        // Persistent (not single-shot): `loaded` fires for every
                        // image, so a single-shot connection would be consumed by
                        // the first unrelated image to finish and we'd miss our
                        // own. Tear down once OUR url arrives.
                        const QString url  = u->avatarUrl;
                        auto          conn = std::make_shared<QMetaObject::Connection>();
                        *conn              = connect(
                            _imgCache,
                            &ImageCache::loaded,
                            this,
                            [this, url, conv, conn](const QString &loadedUrl) {
                                if (loadedUrl != url)
                                    return;
                                QObject::disconnect(*conn);
                                if (conv != _currentConvId)
                                    return;
                                const QPixmap px = _imgCache->get(url);
                                if (!px.isNull() && _headerAvatar)
                                    _headerAvatar->setPixmap(px);
                            }
                        );
                    }
                }
            }
        }
    }
}

void MainWindow::openMembersPopup(const QRect &anchorGlobal) {
    if (!_session || _currentConvId.value.isEmpty())
        return;
    const Conversation *conv = _session->findConversation(_currentConvId);
    if (!conv)
        return;
    if (!_membersPopup) {
        _membersPopup = new MembersPopup(_imgCache, this);
        connect(_membersPopup, &MembersPopup::memberActivated, this, &MainWindow::openDmWith);
    }

    // Names for the ids; someone users.list didn't return (a Slack Connect
    // guest) gets a fetched name later, a placeholder until then.
    Session   *s        = _session;
    const auto resolved = [s](const std::vector<UserId> &ids) {
        std::vector<User> users;
        users.reserve(ids.size());
        for (const auto &id : ids) {
            if (const User *u = s->findUser(id))
                users.push_back(*u);
            else
                users.push_back(User{.id = id, .name = s->userDisplayName(id)});
        }
        return users;
    };

    // Show what is known at once and refresh it: membership changes without an
    // event for most of it (only joins are pushed).
    const auto *cached   = _session->cachedMembers(_currentConvId);
    const int   expected = cached && !cached->empty()     ? int(cached->size())
                           : conv->kind == ConvKind::Mpim ? int(conv->members.size())
                                                          : conv->memberCount;
    _membersPopup->open(anchorGlobal, expected);
    if (cached && !cached->empty())
        _membersPopup->setMembers(resolved(*cached), s->meUserId());

    const ConversationId id = _currentConvId;
    s->loadMembers(id, [this, s, id, resolved](std::vector<UserId> members, QString err) {
        if (_session != s || _currentConvId != id || !_membersPopup->isVisible())
            return;
        if (members.empty() && !err.isEmpty()) {
            _membersPopup->showError(tr("Couldn't load the members (%1).").arg(err));
            return;
        }
        _membersPopup->setMembers(resolved(members), s->meUserId());
    });
}

void MainWindow::openDmWith(UserId user) {
    if (!_session)
        return;
    _session->openDm(
        user,
        [this](ConversationId conv) { _convList->selectConversation(conv); },
        [this](const QString &err) { showNetworkError(err); }
    );
}

void MainWindow::setHeaderGroupAvatars(const Conversation &conv) {
    // Slack-style group DM header: the other members' avatars stacked, then the
    // group's size. Members come from conv.members; an unnamed legacy mpdm may
    // only carry them in its "mpdm-alice--bob-1" name.
    const UserId              me = _session->meUserId();
    std::vector<const User *> others;
    int                       total = 0;
    if (!conv.members.empty()) {
        total = int(conv.members.size());
        for (const auto &uid : conv.members)
            if (uid != me)
                if (const auto *u = _session->findUser(uid))
                    others.push_back(u);
    } else {
        const QStringList unames = parseMpdmUsernames(conv.name);
        total                    = int(unames.size());
        for (const QString &uname : unames)
            for (const auto &u : _session->currentUsers())
                if (u.name == uname) {
                    if (u.id != me)
                        others.push_back(&u);
                    break;
                }
    }
    if (others.empty())
        return;

    std::vector<HeaderAvatarWidget::GroupMember> members;
    QStringList                                  pending;
    for (const User *u : others) {
        if (members.size() == 3)
            break;
        HeaderAvatarWidget::GroupMember m;
        m.url     = u->avatarUrl;
        m.initial = (u->displayName.isEmpty() ? u->name : u->displayName).left(1);
        if (!m.url.isEmpty() && _imgCache) {
            m.pixmap = _imgCache->get(m.url);
            if (m.pixmap.isNull())
                pending.append(m.url);
        }
        members.push_back(std::move(m));
    }
    _headerAvatar->setGroup(std::move(members), std::max(total, int(others.size())));
    if (pending.isEmpty())
        return;
    // Same persistent-until-done subscription as the single-DM avatar: `loaded`
    // fires for every image, so wait until all of OURS have arrived.
    const ConversationId convId = conv.id;
    auto                 conn   = std::make_shared<QMetaObject::Connection>();
    auto                 left   = std::make_shared<QStringList>(pending);
    *conn                       = connect(
        _imgCache, &ImageCache::loaded, this, [this, convId, conn, left](const QString &loadedUrl) {
            if (!left->removeAll(loadedUrl))
                return;
            if (left->isEmpty())
                QObject::disconnect(*conn);
            if (convId != _currentConvId || !_headerAvatar)
                return;
            const QPixmap px = _imgCache->get(loadedUrl);
            if (!px.isNull())
                _headerAvatar->setGroupPixmap(loadedUrl, px);
        }
    );
}

void MainWindow::restoreLastConv() {
    if (!_session)
        return;
    auto [lastConvId, lastConvName] = _session->loadLastConv();
    if (lastConvId.value.isEmpty())
        return;
    const int row = _convList->rowForId(lastConvId);
    if (row < 0)
        return;
    _convList->selectRow(row);
    // selectRow is a no-op when the row is already visually selected (e.g. after
    // rebuildFilteredConvs re-mapped _selectedId without emitting conversationSelected).
    // In that case openConversation was never called, so drive it directly.
    if (_currentConvId.value.isEmpty())
        openConversation(row);
}
