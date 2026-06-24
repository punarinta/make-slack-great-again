// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "main_window.h"
#include "theme.h"
#include "theme_manager.h"
#include "header_avatar_widget.h"
#include "image_cache.h"
#include "title_bar/title_bar.h"
#include "popup_tooltip/popup_tooltip.h"
#include "message_list/message_list.h"
#include "composer/composer_widget.h"
#include "typing_indicator/typing_indicator.h"
#include "conv_list/conv_list_widget.h"
#include "conv_footer/conv_footer_widget.h"
#include "context_menu/context_menu.h"
#include "workspace_switcher/workspace_switcher.h"
#include "session/session.h"
#include "cache/cache_evictor.h"
#include "auth/token_store.h"
#include "auth/auth_strategy.h"
#include "auth/auth_strategy_factory.h"
#include "backend/backend.h"
#include "backend/backend_factory.h"
#include "settings/settings_dialog.h"
#include "search/search_widget.h"
#include "thread_panel/thread_panel.h"
#include "message_list/message_render.h"
#include "canvas_page/canvas_page.h"
#include "conv_tabs/conv_tabs_widget.h"
#include "welcome_tips/welcome_widget.h"
#include "forward_dialog/forward_dialog.h"
#include "create_channel_dialog/create_channel_dialog.h"
#include "profile_dialog/profile_dialog.h"
#include "status_dialog/status_dialog.h"
#include "browse_channels_dialog/browse_channels_dialog.h"
#include "update_checker/update_checker.h"
#include "huddle_banner/huddle_banner.h"
#include "update_bar/update_bar.h"
#include "styled_button/styled_button.h"

#include "ui/icon_utils.h"
#include "util/desktop_notifier.h"
#include "util/slack_links.h"
#include "util/sound_player.h"

#include <QDialog>
#include <QEvent>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QApplication>
#include <QEventLoop>
#include <QIcon>
#include <QMessageBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QCursor>
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
#include <QShortcut>

#include <memory>

static constexpr int kResizeBorder  = 6;
static constexpr int kConvMinWidth  = 160;
static constexpr int kConvMaxWidth  = 400;
static constexpr int kConvInitWidth = 240;

// The workspace identifier flowing through MainWindow (_sessions key,
// _activeTeamId, switcher ids, NavHistory) is the composite WorkspaceKey handle
// string, e.g. "slack:T0123ABCD". This helper resolves a handle to its neutral
// registry record (empty record if the handle is malformed or absent).
static TokenStore::WorkspaceRecord recordForHandle(const QString &handle) {
    if (const auto key = WorkspaceKey::fromString(handle))
        if (const auto rec = TokenStore::loadWorkspace(*key))
            return *rec;
    return {};
}

// Global default notification level (Settings → "Notify me about"): applied to
// conversations whose own level is NotificationLevel::Default. Defaults to "All
// new posts" (0); 1 = "Just mentions".
static NotificationLevel globalDefaultNotifLevel() {
    return QSettings("msga", "msga").value("notifications/level", 0).toInt() == 1
               ? NotificationLevel::Mentions
               : NotificationLevel::All;
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

MainWindow::~MainWindow() = default;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setMinimumSize(800, 600);
    resize(1200, 800);
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
    updateRoundedMask();
    qApp->installEventFilter(this);

    // Back/forward chat navigation: mouse side buttons are handled in
    // eventFilter; these cover the keyboard equivalents — dedicated
    // XF86 Back/Forward keys and the conventional Alt+arrow bindings.
    const auto addNavShortcut = [this](const QKeySequence &seq, bool back) {
        auto *sc = new QShortcut(seq, this);
        connect(sc, &QShortcut::activated, this, [this, back] { navigateHistory(back); });
    };
    addNavShortcut(QKeySequence(Qt::Key_Back), true);
    addNavShortcut(QKeySequence(Qt::Key_Forward), false);
    addNavShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), true);
    addNavShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), false);

    setupTray();

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        for (auto &[teamId, ws] : _sessions)
            ws.session->persistUnreads();
    });

    if (TokenStore::hasAnyWorkspace()) {
        // Connect every logged-in workspace so unread badges and
        // notifications work without clicking each one first.
        const auto keys = TokenStore::workspaceKeys();
        for (const auto &key : keys)
            ensureSession(key.toString());
        const auto    active       = TokenStore::activeWorkspace();
        const QString activeHandle = active ? active->toString() : keys.front().toString();
        activateWorkspace(activeHandle);
    } else {
        showLoggedOut();
    }

    const QByteArray geo = QSettings("msga", "msga").value("window/geometry").toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
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
    wrapper->setStyleSheet(
        QString("QWidget#loggedOutWrapper { background: %1; }").arg(Th::qss(Th::c().nav.bg))
    );
    _loggedOutPageLayout = new QVBoxLayout(wrapper);
    _loggedOutPageLayout->setContentsMargins(0, 0, 0, 0);
    _loggedOutPageLayout->setSpacing(0);

    auto *page = new QWidget(wrapper);
    page->setObjectName("loggedOutPage");
    page->setAttribute(Qt::WA_StyledBackground);
    page->setStyleSheet(
        QString("QWidget { background: %1; }").arg(Th::qss(Th::c().surface.content))
    );
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
    // The global default notification level decides what unconfigured channels
    // notify/badge about; apply it now and re-resolve everything when it changes.
    _convList->setDefaultNotifyLevel(globalDefaultNotifLevel());
    connect(_settingsDialog, &SettingsDialog::notificationsChanged, this, [this] {
        _convList->setDefaultNotifyLevel(globalDefaultNotifLevel());
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
    rightPanel->setStyleSheet(
        QString("QWidget { background: %1; }").arg(Th::qss(Th::c().surface.content))
    );
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // ── Header bar: avatar, conv name, star, search ───────────────────
    auto *msgHeader = new QWidget(rightPanel);
    msgHeader->setObjectName("msgHeader");
    msgHeader->setAttribute(Qt::WA_StyledBackground);
    msgHeader->setFixedHeight(48);
    const auto &sp              = Th::c().spacing;
    auto       *msgHeaderLayout = new QHBoxLayout(msgHeader);
    msgHeaderLayout->setContentsMargins(sp.xl, 0, sp.md, 0);
    msgHeaderLayout->setSpacing(sp.md);

    _headerAvatar = new HeaderAvatarWidget(msgHeader);
    _headerAvatar->setVisible(false);
    msgHeaderLayout->addWidget(_headerAvatar);

    _convNameLabel = new QLabel("", msgHeader);
    _convNameLabel->setObjectName("convNameLabel");
    msgHeaderLayout->addWidget(_convNameLabel, 1);

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
    msgHeaderLayout->addWidget(_huddleBtn);
    msgHeaderLayout->addSpacing(sp.xs);

    _starBtn = new QPushButton(msgHeader);
    _starBtn->setFixedSize(28, 28);
    _starBtn->setFlat(true);
    _starBtn->setCursor(Qt::PointingHandCursor);
    _starBtn->setIconSize(QSize(15, 15));
    _starBtn->setVisible(false);
    _starBtnTooltip = new PopupTooltip(_starBtn);
    _starBtn->installEventFilter(this);
    msgHeaderLayout->addWidget(_starBtn);
    msgHeaderLayout->addSpacing(sp.xs);

    _searchBtn = new QPushButton(msgHeader);
    _searchBtn->setObjectName("headerSearchBtn");
    _searchBtn->setFixedSize(32, 32);
    _searchBtn->setFlat(true);
    _searchBtn->setCursor(Qt::PointingHandCursor);
    _searchBtn->setIconSize(QSize(16, 16));
    _searchBtn->setIcon(svgIcon(":/ui/search.svg", QSize(16, 16), Th::c().icon.def));
    _searchBtnTooltip = new PopupTooltip(_searchBtn);
    _searchBtn->installEventFilter(this);
    msgHeaderLayout->addWidget(_searchBtn);
    _msgHeader = msgHeader;
    rightLayout->addWidget(msgHeader);

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
    auto *searchShortcut = new QShortcut(QKeySequence::Find, this);
    connect(searchShortcut, &QShortcut::activated, this, openSearch);
    connect(_searchWidget, &SearchWidget::closeRequested, this, [this] {
        _searchWidget->closeSearch(); // animated
    });
    connect(_searchWidget, &SearchWidget::resultSelected, this, [this](ConversationId conv, Ts ts) {
        if (conv == _currentConvId) {
            _messageList->scrollToTs(ts);
        } else {
            const int row = _convList->rowForId(conv);
            if (row >= 0)
                openConversation(row);
        }
    });

    connect(
        _messageList,
        &MessageListWidget::threadClicked,
        this,
        [this](ConversationId conv, Ts rootTs) {
            _threadPanel->setVisible(true);
            _threadPanel->openThread(conv, rootTs);
            _messageList->setOpenThreadRoot(rootTs);
            if (_msgSplitter->sizes().at(1) < 100) {
                const int total = _msgSplitter->width();
                _msgSplitter->setSizes({total - 360, 360});
            }
        }
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
    const auto openDmFor = [this](UserId user) {
        if (!_session)
            return;
        _session->openDm(
            user,
            [this](ConversationId conv) { _convList->selectConversation(conv); },
            [this](const QString &err) { showNetworkError(err); }
        );
    };
    connect(_messageList, &MessageListWidget::openDmRequested, this, openDmFor);
    connect(_threadPanel, &ThreadPanel::openDmRequested, this, openDmFor);

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
        [this](const Message &msg) {
            if (!_session)
                return;
            auto *dlg = new ForwardDialog(msg, _session, this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            connect(dlg, &AppDialog::accepted, this, [this, dlg, msg] {
                const ConversationId target = dlg->targetConv();
                if (target.value.isEmpty())
                    return;
                // Email (Model-D): forwarding to a channel labels the original
                // message rather than re-posting its text (imap-backend-plan §3).
                const Conversation *tc        = _session->findConversation(target);
                const bool          isChannel = tc && (tc->kind == ConvKind::PublicChannel ||
                                              tc->kind == ConvKind::PrivateChannel);
                if (isChannel && _session->channelsAreLabels()) {
                    _session->labelMessage(
                        _currentConvId, msg.ts, target, [this](bool ok, QString) {
                            if (!ok)
                                showNetworkError(tr("Couldn't apply the label."));
                        }
                    );
                    return;
                }
                const QString comment = dlg->comment();
                const QString fwd     = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
                const QString full    = comment.isEmpty() ? fwd : (comment + "\n" + fwd);
                _session->sendMessage(target, full);
            });
            dlg->open();
        }
    );

    connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &text) {
        if (_session && !_currentConvId.value.isEmpty())
            _session->sendMessage(_currentConvId, text, std::nullopt, _composer->subjectText());
    });
    connect(
        _composer,
        &ComposerWidget::commandRequested,
        this,
        [this](const QString &name, const QString &args) {
            if (_session && !_currentConvId.value.isEmpty())
                _session->runCommand(_currentConvId, name, args);
        }
    );
    connect(
        _composer,
        &ComposerWidget::uploadRequested,
        this,
        [this](const QStringList &filePaths, const QString &text) {
            if (_session && !_currentConvId.value.isEmpty())
                _session->uploadFiles(_currentConvId, filePaths, text);
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
    connect(_composer, &ComposerWidget::editLastRequested, this, [this] {
        if (!_session || !_messageList)
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

    connect(_convList, &ConvListWidget::conversationSelected, this, [this](int row) {
        const ConversationId id = _convList->conversationId(row);
        if (id.value.isEmpty())
            return;
        const auto *conv = _session ? _session->findConversation(id) : nullptr;
        if (!conv)
            return;
        const QString name = _convList->resolvedName(row);
        const bool    isDm = conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim;
        _convNameLabel->setText(isDm ? name : name.isEmpty() ? "" : "#" + name);
        updateHeaderForConv(id);
    });

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
    // EVERY widget in the application — hundreds of ms on a populated window.
    // globalQss() only styles QToolTip, and its colors come from content tokens
    // (text.*) that are shared across all current themes, so a purple<->blue
    // switch produces a byte-for-byte identical string. Re-applying it then is
    // pure waste: skip unless the string actually changed (e.g. a future theme
    // that retints tooltips), turning the common case into a no-op.
    static QString lastGlobalQss;
    const QString  gqss = Th::globalQss();
    if (gqss != lastGlobalQss) {
        lastGlobalQss = gqss;
        qApp->setStyleSheet(gqss);
    }

    const auto &th = Th::c();

    // The window backdrop (nav tone + mirrored light content region) is painted
    // by BackdropFrame::paintEvent reading the live theme, so a theme switch just
    // needs a repaint rather than a palette/stylesheet change (which would
    // re-polish the whole subtree). _rightArea no longer needs its own opaque
    // nav.bg fill: the backdrop already paints nav tone behind the resize-handle
    // strip, and the right panel paints its own light surface on top.
    _frame->update();

    if (_msgHeader) {
        _msgHeader->setStyleSheet(
            QString("QWidget#msgHeader { background: %1; }").arg(Th::qss(th.surface.raised))
        );
    }
    if (_convNameLabel) {
        _convNameLabel->setStyleSheet(QString("font-weight: 600; font-size: %1px; color: %2;")
                                          .arg(th.fonts.xxl)
                                          .arg(Th::qss(th.text.primary)));
    }
    if (_huddleBtn) {
        _huddleBtn->setStyleSheet("QPushButton { border: none; background: transparent; }");
        _huddleBtn->setIcon(svgIcon(":/ui/headphones.svg", QSize(16, 16), th.icon.def));
    }
    if (_starBtn) {
        _starBtn->setStyleSheet("QPushButton { border: none; background: transparent; }");
    }
    if (_searchBtn) {
        _searchBtn->setStyleSheet("QPushButton { border: none; background: transparent; }");
        _searchBtn->setIcon(svgIcon(":/ui/search.svg", QSize(16, 16), th.icon.def));
    }
    if (_errorBanner) {
        _errorBanner->setStyleSheet(QString(
                                        "QLabel#errorBanner {"
                                        "  background: %1;"
                                        "  color: %2;"
                                        "  padding: 6px 12px;"
                                        "  font-size: %3px;"
                                        "}"
        )
                                        .arg(Th::qss(th.danger.icon), Th::qss(th.surface.raised))
                                        .arg(th.fonts.md));
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

void MainWindow::dropSession(QString teamId) {
    auto it = _sessions.find(teamId);
    if (it == _sessions.end())
        return;
    if (it->second.session.get() == _session) {
        _uiLifetime = rpl::lifetime();
        _session    = nullptr;
        if (_messageList)
            _messageList->setSession(nullptr);
        _currentConvId  = {};
        _pendingNavConv = {};
    }
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

    // Detach the UI from the outgoing session — it stays alive in the
    // background and keeps accumulating unreads / firing notifications.
    _uiLifetime = rpl::lifetime();
    if (_session) {
        _session->setReading({});
        // Debounced: the conv-list serialization + file write must not block the
        // switch. A pending save is flushed on drop / shutdown / destruction.
        _session->scheduleSaveUnreads();
    }
    _currentConvId = {};
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
    _uiLifetime = rpl::lifetime();
    _session    = nullptr;
    if (_messageList)
        _messageList->setSession(nullptr);
    if (_convFooter)
        _convFooter->clear();
    _activeTeamId.clear();
    _currentConvId = {};
    updateTrayIcon();
    if (_titleBar)
        _titleBar->setTitle({});
    refreshSwitcher(); // switcher is always visible — deselect the active entry
    _stack->setCurrentWidget(_loggedOutPage);
}

void MainWindow::promptAddWorkspace(const QPoint &anchorGlobal) {
    const auto services = auth::registeredAuthServices();
    if (services.empty())
        return;
    // One service → no point in a menu; start it directly.
    if (services.size() == 1) {
        loginWithService(services.front());
        return;
    }

    // Several services → our themed ContextMenu anchored at the add button, one
    // row per service. Defer the actual login to the next event-loop turn so the
    // popup finishes dismissing before a browser opens.
    auto *menu = new ContextMenu(this);
    menu->setWidthMode(ContextMenu::WidthMode::MinWidth);
    for (const Service s : services) {
        menu->addItem(serviceDisplayName(s), [this, s] {
            QTimer::singleShot(0, this, [this, s] { loginWithService(s); });
        });
    }
    menu->popup(anchorGlobal);
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

    connect(s, &auth::AuthStrategy::succeeded, this, [this, s](TokenStore::WorkspaceRecord rec) {
        TokenStore::saveWorkspace(rec);
        _activeTeamId = rec.key.toString();
        _activeFlow   = nullptr;
        s->deleteLater();
        activateWorkspace(_activeTeamId);
    });
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
            if (_session)
                _session->leaveConversation(id);
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
    connect(_convList, &ConvListWidget::findChannelRequested, this, [this] {
        openBrowseDialog(0);
    });
    connect(_convList, &ConvListWidget::browsePeopleRequested, this, [this] {
        openBrowseDialog(1);
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

void MainWindow::connectToSession() {
    wireConvList();

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
                // On first populate (no conversation open yet), jump to the last
                // conversation the user had open in the previous session.
                if (_currentConvId.value.isEmpty()) {
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
                                // Also update the message list intro and composer placeholder,
                                // which were set from the (still-unresolved) user ID on open.
                                if (_messageList)
                                    _messageList->updateConvName(
                                        name,
                                        tr("This is the beginning of your direct message history "
                                           "with %1.")
                                            .arg(name)
                                    );
                                if (_composer)
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
                    if (_headerAvatar && _headerAvatar->isVisible()) {
                        const auto *conv = _session->findConversation(_currentConvId);
                        if (conv && conv->dmUser && *conv->dmUser == ev->user)
                            _headerAvatar->setPresence(ev->active);
                    }
                } else if (const auto *ev = std::get_if<EvDndChanged>(&e)) {
                    if (_headerAvatar && _headerAvatar->isVisible()) {
                        const auto *conv = _session->findConversation(_currentConvId);
                        if (conv && conv->dmUser && *conv->dmUser == ev->user)
                            _headerAvatar->setDnd(ev->dndEnabled);
                    }
                } else if (const auto *ev = std::get_if<EvTyping>(&e)) {
                    // NOTE: in practice EvTyping never fires — Slack delivers
                    // user_typing only over the deprecated RTM API, which has no
                    // Events API / Socket Mode equivalent and which a maintainer
                    // confirmed will not be added (node-slack-sdk#1130). See the
                    // dead user_typing branch in socket_mode_realtime.cpp. This
                    // handler + TypingIndicatorWidget are kept ready so the UI
                    // works automatically should such an event ever arrive.
                    //
                    // Show typing for the open conversation only.  Our own id can
                    // arrive here when we type from another client (we never echo
                    // local typing), shown as "You … on another device".
                    if (_typingIndicator && ev->conv == _currentConvId) {
                        const bool isSelf = ev->user == _session->meUserId();
                        _typingIndicator->userTyping(
                            ev->user, _session->userDisplayName(ev->user), isSelf
                        );
                    }
                } else if (const auto *ev = std::get_if<EvMessageNew>(&e)) {
                    // A delivered message means that author has stopped typing.
                    if (_typingIndicator && ev->conv == _currentConvId)
                        _typingIndicator->userStopped(ev->msg.author);
                }
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

void MainWindow::maybeNotify(const QString &teamId, const EvMessageNew &ev) {
    QSettings s("msga", "msga");
    if (!s.value("notifications/enabled", true).toBool())
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

    // Skip if this conversation is on screen right now
    if (isActiveWindow() && teamId == _activeTeamId && ev.conv == _currentConvId)
        return;

    const auto *conv = session->findConversation(ev.conv);

    // Unknown conversation → not a member (or another workspace's event from
    // the shared socket); muted → no notification wanted, mentions included.
    if (!conv || conv->isMuted || !conv->isMember || conv->notifLevel == NotificationLevel::Mute)
        return;

    const bool     isDm        = (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
    const QString &mt          = ev.msg.rawText.isEmpty() ? ev.msg.text.text : ev.msg.rawText;
    const bool     isImportant = isDm || mrkdwnMentions(mt, me);

    // Per-conversation level wins over the global default ("All new posts" unless
    // the user changed it). Muted conversations already returned above, so the
    // effective level here is only ever All or Mentions: "Just mentions" delivers
    // no notification (no tray, no D-Bus) for a non-DM, non-@mention message.
    const NotificationLevel lvl = effectiveNotifLevel(*conv, globalDefaultNotifLevel());
    if (lvl != NotificationLevel::All && !isImportant)
        return;

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
    const QString preview = MsgRender::notificationText(ev.msg.text, session);

    QString title, body;
    if (isDm) {
        title = senderName;
        body  = preview;
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
            iconUrl = recordForHandle(teamId).iconUrl;
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
    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable()) {
        const QString token = teamId + QChar(0x1f) + ev.conv.value;
        shown               = _desktopNotifier->notify(
            title, body, notifPix.isNull() ? QImage() : notifPix.toImage(), token, {}, 5000
        );
    }
    if (!shown) {
        _pendingNotifTeam = teamId;
        _pendingNotifConv = ev.conv;
        if (notifPix.isNull())
            _trayIcon->showMessage(title, body, QSystemTrayIcon::NoIcon, 5000);
        else
            _trayIcon->showMessage(title, body, QIcon(notifPix), 5000);
    }

    if (s.value("notifications/sound", true).toBool())
        Sound::Player::instance().play(
            s.value("notifications/soundId", Sound::Player::defaultId()).toString()
        );
}

void MainWindow::maybeNotifyHuddle(const QString &teamId, const EvHuddleChanged &ev) {
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

    const auto it = _sessions.find(teamId);
    if (it == _sessions.end())
        return;
    Session *session = it->second.session.get();
    if (!session->capabilities().huddles)
        return;

    const UserId me   = session->meUserId();
    const auto  *conv = session->findConversation(ev.conv);
    // Member-only, not muted, not a huddle I'm already in, and (for channels)
    // only when set to "All new posts" — see shouldNotifyHuddleStart.
    if (!conv || !shouldNotifyHuddleStart(*conv, ev.participants, me, globalDefaultNotifLevel()))
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
            iconUrl = recordForHandle(teamId).iconUrl;
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
        _pendingNotifTeam = teamId;
        _pendingNotifConv = ev.conv;
        if (notifPix.isNull())
            _trayIcon->showMessage(title, body, QSystemTrayIcon::NoIcon, 5000);
        else
            _trayIcon->showMessage(title, body, QIcon(notifPix), 5000);
    }

    if (s.value("notifications/sound", true).toBool())
        Sound::Player::instance().play(
            s.value("notifications/soundId", Sound::Player::defaultId()).toString()
        );
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
        const QString iconUrl = recordForHandle(_activeTeamId).iconUrl;
        if (!iconUrl.isEmpty())
            notifPix = roundedNotifIcon(_imgCache->get(iconUrl));
    }

    // Empty body token: clicking just dismisses (no real conversation to open).
    bool shown = false;
    if (_desktopNotifier && _desktopNotifier->isAvailable())
        shown = _desktopNotifier->notify(
            title, body, notifPix.isNull() ? QImage() : notifPix.toImage(), QString(), actions, 5000
        );
    if (!shown && _trayIcon) {
        _pendingNotifTeam.clear();
        _pendingNotifConv = {};
        if (notifPix.isNull())
            _trayIcon->showMessage(title, body, QSystemTrayIcon::NoIcon, 5000);
        else
            _trayIcon->showMessage(title, body, QIcon(notifPix), 5000);
    }
}

void MainWindow::updateUnreadBadges(const QString &teamId, const std::vector<Conversation> &convs) {
    // important (red) = DM/MPDM unreads + channel @mentions. normal (blue) = other
    // *allowed* unread activity — only in channels set to "All new posts". A muted
    // conversation is fully silent (no red, no blue); a "Just mentions" channel
    // contributes only its @mentions (red), never blue.
    const NotificationLevel fallback = globalDefaultNotifLevel();
    int                     normal = 0, important = 0;
    for (const auto &c : convs) {
        if (!c.isMember)
            continue;
        const NotificationLevel lvl = effectiveNotifLevel(c, fallback);
        if (lvl == NotificationLevel::Mute)
            continue;
        const bool isDm = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
        if (isDm) {
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
    if (!_trayIcon)
        return;
    int globalTotal = 0, globalMentions = 0;
    for (auto it = _wsUnreads.cbegin(); it != _wsUnreads.cend(); ++it) {
        globalTotal += it.value().first;
        globalMentions += it.value().second;
    }
    // Always render via QSvgRenderer so the alpha channel is preserved in static builds.
    const int    sz = 128;
    QSvgRenderer renderer(QString(":/icon_tray.svg"));
    QPixmap      px(sz, sz);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    if (renderer.isValid())
        renderer.render(&p, QRectF(0, 0, sz, sz));
    if (globalTotal > 0 || globalMentions > 0) {
        // Red when anything important (DM/mention) is unread anywhere,
        // blue for plain unread activity.
        const int d = 36;
        p.setBrush(globalMentions > 0 ? Th::c().badge.mention : Th::c().badge.activity);
        p.setPen(Qt::NoPen);
        p.drawEllipse(sz - d, sz - d, d, d);
    }
    p.end();
    _trayIcon->setIcon(QIcon(px));
}

// ── Workspace management ──────────────────────────────────────────────────────

void MainWindow::refreshSwitcher() {
    if (!_switcher)
        return;

    const auto                            keys = TokenStore::workspaceKeys();
    std::vector<WorkspaceSwitcher::Entry> entries;
    entries.reserve(keys.size());
    for (const auto &key : keys) {
        const auto    rec    = TokenStore::loadWorkspace(key);
        const QString handle = key.toString();
        entries.push_back(
            {handle, rec ? rec->displayName : QString(), rec ? rec->iconUrl : QString()}
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
    if (const auto key = WorkspaceKey::fromString(teamId))
        TokenStore::removeWorkspace(*key);

    const auto remaining = TokenStore::workspaceKeys();
    if (remaining.empty()) {
        showLoggedOut();
    } else if (wasActive) {
        activateWorkspace(remaining.front().toString());
    } else {
        refreshSwitcher();
    }
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
        openNotifTarget(_pendingNotifTeam, _pendingNotifConv);
        _pendingNotifConv = {};
        _pendingNotifTeam.clear();
    });

    _trayIcon->show();

    // Platform notifier (Linux/macOS): clicking a notification fires activated()
    // with the team\x1fconv token we encoded in maybeNotify(). (Windows delivers
    // the same token via msga://notif protocol activation → handleNotifToken.)
    _desktopNotifier = new DesktopNotifier(this);
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
    const int sep = token.indexOf(QChar(0x1f));
    if (sep < 0)
        return;
    openNotifTarget(token.left(sep), ConversationId{token.mid(sep + 1)});
}

void MainWindow::openNotifTarget(const QString &teamId, const ConversationId &conv) {
    show();
    raise();
    activateWindow();
    if (conv.value.isEmpty())
        return;
    // The notification may belong to a background workspace — bring it up first.
    if (!teamId.isEmpty() && teamId != _activeTeamId)
        activateWorkspace(teamId);
    if (_convList) {
        const int row = _convList->rowForId(conv);
        if (row >= 0)
            openConversation(row);
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
    if (!isMaximized() && !isFullScreen()) {
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
    const bool  windowed = !isMaximized() && !isFullScreen();
    const auto &sp       = Th::c().spacing;
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

void MainWindow::changeEvent(QEvent *e) {
    if (e->type() == QEvent::WindowStateChange) {
        updateRoundedMask();
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
            // and fire notifications, like the official client does.
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

void MainWindow::openConversation(int row) {
    if (!_session)
        return;

    if (_searchWidget && _searchWidget->isVisible())
        _searchWidget->hide();

    // Save draft / exit edit mode for the outgoing conversation.
    if (!_currentConvId.value.isEmpty() && _composer) {
        _composer->exitEditMode();
        const QString draft = _composer->currentText();
        if (draft.isEmpty())
            _drafts.remove(_currentConvId.value);
        else
            _drafts[_currentConvId.value] = draft;
    }

    _currentConvId = _convList->conversationId(row);
    if (_currentConvId.value.isEmpty())
        return;

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

    // Build the channel/DM intro description for the message list header.
    QString description;
    if (conv) {
        if (isDm) {
            if (!name.isEmpty())
                description =
                    tr("This is the beginning of your direct message history with %1.").arg(name);
        } else if (!conv->description.isEmpty()) {
            description = conv->description;
        }
    }

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

    _session->setReading(_currentConvId);
    if (_canvasPage)
        _canvasPage->flushPendingSave(); // outgoing conversation's canvas edits
    if (_contentStack)
        _contentStack->setCurrentWidget(_messageList);
    _messageList->openConversation(_currentConvId, displayName, description, lastReadTs);

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
    _composer->setPlaceholderText(
        displayName.isEmpty() ? tr("Message") : tr("Message %1").arg(displayName)
    );

    // Restore any unsent draft for this conversation.
    _composer->setText(_drafts.value(_currentConvId.value));

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
        connect(
            _messageList,
            &MessageListWidget::initialPageLoaded,
            this,
            [this, convId = _currentConvId] {
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
            },
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
    // Single source of truth for the link shape lives in SlackLinks::huddle().
    // `teamId` here is the app-wide canonical WorkspaceKey ("slack:TCF2J0TSP"),
    // but the URL needs the bare Slack team id ("TCF2J0TSP"), so unwrap it. The
    // team is taken explicitly because a notification may target a background
    // workspace, not the active one.
    const auto    key = WorkspaceKey::fromString(teamId);
    const QString t   = key ? key->id : teamId;
    return SlackLinks::huddle(t, conv.value);
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

    if (_headerAvatar) {
        _headerAvatar->setVisible(isDm);
        _headerAvatar->clearAvatar();
        if (isDm && conversation->dmUser) {
            const auto *u = _session->findUser(*conversation->dmUser);
            if (u) {
                // Apps/bots have no presence — they can't go offline, so the dot
                // is meaningless and confusing for them. Likewise services with no
                // presence concept at all (email/IMAP).
                _headerAvatar->setShowPresence(
                    _session->capabilities().presence && !_session->isAppConversation(*conversation)
                );
                _headerAvatar->setPresence(u->isActive);
                _headerAvatar->setDnd(u->dndEnabled);
                const bool isSelf = *conversation->dmUser == _session->meUserId();
                const auto sp     = _session->currentSelfPresence();
                _headerAvatar->setPhantomAway(isSelf && sp.phantomAway());
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
