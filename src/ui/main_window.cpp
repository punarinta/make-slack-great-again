// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "main_window.h"
#include "theme.h"
#include "header_avatar_widget.h"
#include "image_cache.h"
#include "title_bar/title_bar.h"
#include "message_list/message_list.h"
#include "composer/composer_widget.h"
#include "conv_list/conv_list_widget.h"
#include "context_menu/context_menu.h"
#include "workspace_switcher/workspace_switcher.h"
#include "session/session.h"
#include "auth/token_store.h"
#include "auth/oauth_flow.h"
#include "backend/public_backend/public_backend.h"
#include "settings/settings_dialog.h"
#include "search/search_widget.h"
#include "thread_panel/thread_panel.h"
#include "welcome_tips/welcome_widget.h"
#include "forward_dialog/forward_dialog.h"
#include "update_checker/update_checker.h"
#include "update_bar/update_bar.h"
#include "app_credentials.h"

#include "ui/icon_utils.h"

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
#include <QSettings>
#include <QSplitter>
#include <QWindow>
#include <QBitmap>
#include <QPainter>
#include <QPainterPath>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <QDesktopServices>

static constexpr int kResizeBorder = 6;

MainWindow::~MainWindow() = default;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setMinimumSize(800, 600);
    resize(1200, 800);
    buildUi();
    updateRoundedMask();
    qApp->installEventFilter(this);

    setupTray();

    if (TokenStore::hasAnyWorkspace())
        startSession(TokenStore::activeWorkspaceId());
    else
        showLoggedOut();
}

// ── UI construction ───────────────────────────────────────────────────────────

void MainWindow::buildUi() {
    _frame = new QWidget(this);
    _frame->setObjectName("windowFrame");
    _frame->setMouseTracking(true);

    _frameLayout = new QVBoxLayout(_frame);
    _frameLayout->setContentsMargins(0, 0, 0, 0);
    _frameLayout->setSpacing(0);

    _titleBar = new TitleBar(_frame);
    _frameLayout->addWidget(_titleBar);

    _updateBar = new UpdateBar(_frame);
    _frameLayout->addWidget(_updateBar);

    // Horizontal body: switcher rail always present, stack fills the rest.
    // _stack must be created before buildWorkspaceSwitcher (SettingsDialog parents to it).
    auto *body = new QWidget(_frame);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    _frameLayout->addWidget(body, 1);

    _stack = new QStackedWidget(body);

    _updateChecker = new UpdateChecker(this);

    bodyLayout->addWidget(buildWorkspaceSwitcher(body));
    bodyLayout->addWidget(_stack, 1);

    setCentralWidget(_frame);

    if (_updateChecker->stagedVersion() > AppCredentials::version)
        _updateBar->showUpdateReady(_updateChecker->stagedPath());
    connect(_updateChecker, &UpdateChecker::downloadReady,
            _updateBar,     &UpdateBar::showUpdateReady);
    connect(_updateBar, &UpdateBar::restartRequested,
            this,       &MainWindow::applyUpdateAndRestart);
    QTimer::singleShot(5000, _updateChecker, &UpdateChecker::checkInBackground);

    _loggedOutPage = buildLoggedOutPage();
    _stack->addWidget(_loggedOutPage);
    _stack->setCurrentWidget(_loggedOutPage);
}

QWidget *MainWindow::buildLoggedOutPage() {
    auto *page = new QWidget;
    page->setObjectName("loggedOutPage");

    auto *outer = new QVBoxLayout(page);
    outer->setAlignment(Qt::AlignCenter);
    outer->setContentsMargins(32, 32, 32, 32);

    auto *inner = new QWidget(page);
    inner->setFixedWidth(300);

    auto *layout = new QVBoxLayout(inner);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(16);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *icon = new QLabel(inner);
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(72, 72);
    icon->setPixmap(QIcon(":/icon.svg").pixmap(QSize(72, 72), qApp->devicePixelRatio()));

    auto *titleBlock = new QWidget(inner);
    auto *titleLayout = new QVBoxLayout(titleBlock);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(8);

    auto *title = new QLabel("MSGA", titleBlock);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(
        "font-size: 24px; font-weight: 600; color: #1D1C1D; margin-top: 4px;");

    auto *tagline = new QLabel(titleBlock);
    tagline->setAlignment(Qt::AlignCenter);
    tagline->setText(
        "<span style='font-size:11px; color:#777; letter-spacing:0.06em;'>"
        "[<span style='color:#1D1C1D;'>m</span>ake "
        "<span style='color:#1D1C1D;'>s</span>lack "
        "<span style='color:#1D1C1D;'>g</span>reat "
        "<span style='color:#1D1C1D;'>a</span>gain]"
        "</span>"
    );

    titleLayout->addWidget(title);
    titleLayout->addWidget(tagline);

    auto *loginBtn = new QPushButton(tr("Log in to workspace"), inner);
    loginBtn->setFixedHeight(40);
    loginBtn->setCursor(Qt::PointingHandCursor);
    loginBtn->setStyleSheet(
        "QPushButton {"
        "  background: #007A5A;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "  padding: 0 24px;"
        "}"
        "QPushButton:hover   { background: #148567; }"
        "QPushButton:pressed { background: #005E45; }"
    );
    connect(loginBtn, &QPushButton::clicked, this, [this] {
        if (runLoginFlow())
            startSession(_activeTeamId);
    });

    layout->addWidget(icon, 0, Qt::AlignCenter);
    layout->addWidget(titleBlock);
    layout->addSpacing(12);
    layout->addWidget(loginBtn);

    outer->addWidget(inner, 0, Qt::AlignCenter);
    return page;
}

QWidget *MainWindow::buildMainPage() {
    _imgCache = new ImageCache(this);
    _imgCache->setDiskCache(
        [this](const QString &url) -> QByteArray {
            return _sessionOwner ? _sessionOwner->cachedImage(url) : QByteArray{};
        },
        [this](const QString &url, const QByteArray &data) {
            if (_sessionOwner) _sessionOwner->cacheImage(url, data);
        });

    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);
    root->setSpacing(0);
    root->setContentsMargins(0, 0, 0, 0);

    root->addWidget(buildConvPanel(page));
    root->addWidget(buildRightPanel(page), 1);

    // Apply stored appearance setting and keep conv list in sync when settings are saved.
    _convList->setRelevantDays(QSettings("msga", "msga").value("appearance/relevantDays", 14).toInt());
    connect(_settingsDialog, &SettingsDialog::appearanceChanged,
            _convList,       &ConvListWidget::setRelevantDays);
    connect(_settingsDialog, &SettingsDialog::stateCleared,
            _convList,       &ConvListWidget::resetVisitedAt);

    return page;
}

QWidget *MainWindow::buildWorkspaceSwitcher(QWidget *parent) {
    _switcher = new WorkspaceSwitcher(parent);
    _switcher->setObjectName("workspaceSidebar");
    connect(_switcher, &WorkspaceSwitcher::workspaceClicked,
            this, &MainWindow::switchToWorkspace);
    connect(_switcher, &WorkspaceSwitcher::addWorkspaceClicked, this, [this] {
        if (runLoginFlow()) startSession(_activeTeamId);
    });
    connect(_switcher, &WorkspaceSwitcher::workspaceRightClicked,
            this, &MainWindow::showWorkspaceMenu);

    _settingsDialog = new SettingsDialog(_stack);
    _settingsDialog->setUpdateChecker(_updateChecker);
    connect(_switcher, &WorkspaceSwitcher::settingsClicked,
            _settingsDialog, &SettingsDialog::open);
    return _switcher;
}

QWidget *MainWindow::buildConvPanel(QWidget *parent) {
    _convPanel = new QWidget(parent);
    _convPanel->setObjectName("convPanel");
    _convPanel->setFixedWidth(240);

    auto *convLayout = new QVBoxLayout(_convPanel);
    convLayout->setContentsMargins(0, 0, 0, 0);
    convLayout->setSpacing(0);

    _convList = new ConvListWidget(_imgCache, _convPanel);
    _convList->setObjectName("convList");
    convLayout->addWidget(_convList);

    connect(_convList, &ConvListWidget::conversationSelected,
            this, &MainWindow::openConversation);
    return _convPanel;
}

QWidget *MainWindow::buildRightPanel(QWidget *parent) {
    auto *rightPanel  = new QWidget(parent);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // ── Header bar: avatar, conv name, star, search ───────────────────
    auto *msgHeader = new QWidget(rightPanel);
    msgHeader->setObjectName("msgHeader");
    msgHeader->setFixedHeight(48);
    msgHeader->setStyleSheet(
        "QWidget#msgHeader {"
        "  background: #FFFFFF;"
        "  border-bottom: 1px solid #E8E8E8;"
        "}");
    auto *msgHeaderLayout = new QHBoxLayout(msgHeader);
    msgHeaderLayout->setContentsMargins(16, 0, 8, 0);
    msgHeaderLayout->setSpacing(6);

    _headerAvatar = new HeaderAvatarWidget(msgHeader);
    _headerAvatar->setVisible(false);
    msgHeaderLayout->addWidget(_headerAvatar);

    _convNameLabel = new QLabel("", msgHeader);
    _convNameLabel->setObjectName("convNameLabel");
    _convNameLabel->setStyleSheet("font-weight: bold; font-size: 15px; color: #1D1C1D;");
    msgHeaderLayout->addWidget(_convNameLabel, 1);

    _starBtn = new QPushButton(msgHeader);
    _starBtn->setFixedSize(28, 28);
    _starBtn->setFlat(true);
    _starBtn->setCursor(Qt::PointingHandCursor);
    _starBtn->setToolTip(tr("Star conversation"));
    _starBtn->setIconSize(QSize(15, 15));
    _starBtn->setStyleSheet(
        "QPushButton { border-radius: 4px; }"
        "QPushButton:hover { background: #F0F0F0; }");
    _starBtn->setVisible(false);
    msgHeaderLayout->addWidget(_starBtn);
    msgHeaderLayout->addSpacing(2);

    auto *searchBtn = new QPushButton(msgHeader);
    searchBtn->setFixedSize(32, 32);
    searchBtn->setFlat(true);
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setToolTip(tr("Search messages"));
    searchBtn->setIconSize(QSize(16, 16));
    searchBtn->setIcon(svgIcon(":/ui/search.svg", QSize(16, 16), QColor("#616061")));
    searchBtn->setStyleSheet(
        "QPushButton { border-radius: 4px; }"
        "QPushButton:hover { background: #F0F0F0; }");
    msgHeaderLayout->addWidget(searchBtn);
    _msgHeader = msgHeader;
    rightLayout->addWidget(msgHeader);

    // ── Error banner — shown briefly when a background network error fires ──
    _errorBanner = new QLabel(rightPanel);
    _errorBanner->setObjectName("errorBanner");
    _errorBanner->setStyleSheet(
        "QLabel#errorBanner {"
        "  background: #C0392B;"
        "  color: #FFFFFF;"
        "  padding: 6px 12px;"
        "  font-size: 13px;"
        "}");
    _errorBanner->setAlignment(Qt::AlignCenter);
    _errorBanner->hide();
    rightLayout->addWidget(_errorBanner);

    // ── Content splitter: message area (left) + thread panel (right) ──
    _msgSplitter = new QSplitter(Qt::Horizontal, rightPanel);
    _msgSplitter->setHandleWidth(1);
    _msgSplitter->setChildrenCollapsible(false);
    rightLayout->addWidget(_msgSplitter, 1);

    auto *msgArea   = new QWidget(_msgSplitter);
    auto *msgLayout = new QVBoxLayout(msgArea);
    msgLayout->setContentsMargins(0, 0, 0, 0);
    msgLayout->setSpacing(0);

    _contentStack = new QStackedWidget(msgArea);
    msgLayout->addWidget(_contentStack, 1);

    _messageList = new MessageListWidget(nullptr, _imgCache, _contentStack);
    _contentStack->addWidget(_messageList);

    _searchWidget = new SearchWidget(_contentStack);
    _contentStack->addWidget(_searchWidget);

    _welcomeTips = new WelcomeWidget(_contentStack);
    _contentStack->addWidget(_welcomeTips);
    _contentStack->setCurrentWidget(_welcomeTips);

    _composer = new ComposerWidget(msgArea);
    _composer->setEnabled(false);
    msgLayout->addWidget(_composer);

    _msgSplitter->addWidget(msgArea);

    _threadPanel = new ThreadPanel(_msgSplitter);
    _threadPanel->setVisible(false);
    _msgSplitter->addWidget(_threadPanel);
    _msgSplitter->setStretchFactor(0, 1);
    _msgSplitter->setStretchFactor(1, 0);

    // ── Signal wiring ─────────────────────────────────────────────────
    connect(searchBtn, &QPushButton::clicked, this, [this] {
        if (_contentStack->currentWidget() == _searchWidget)
            _contentStack->setCurrentWidget(
                _currentConvId.value.isEmpty() ? (QWidget *)_welcomeTips : (QWidget *)_messageList);
        else
            _contentStack->setCurrentWidget(_searchWidget);
    });
    connect(_searchWidget, &SearchWidget::closeRequested, this, [this] {
        _contentStack->setCurrentWidget(
            _currentConvId.value.isEmpty() ? (QWidget *)_welcomeTips : (QWidget *)_messageList);
    });
    connect(_searchWidget, &SearchWidget::resultSelected,
            this, [this](ConversationId conv, Ts /*ts*/) {
        const int row = _convList->rowForId(conv);
        if (row >= 0) openConversation(row);
    });

    connect(_messageList, &MessageListWidget::threadClicked,
            this, [this](ConversationId conv, Ts rootTs) {
        _threadPanel->setVisible(true);
        _threadPanel->openThread(conv, rootTs);
        if (_msgSplitter->sizes().at(1) < 100) {
            const int total = _msgSplitter->width();
            _msgSplitter->setSizes({total - 360, 360});
        }
    });
    connect(_threadPanel, &ThreadPanel::closeRequested, this, [this] {
        _threadPanel->close();
        _threadPanel->setVisible(false);
    });

    connect(_messageList, &MessageListWidget::editMessageRequested,
            this, [this](const Ts &ts, const QString &rawText, const std::vector<File> &files) {
        _composer->enterEditMode(ts, rawText, files);
    });
    connect(_messageList, &MessageListWidget::forwardMessageRequested,
            this, [this](const Message &msg) {
        if (!_sessionOwner) return;
        auto *dlg = new ForwardDialog(msg, _sessionOwner.get(), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QDialog::accepted, this, [this, dlg, msg] {
            const ConversationId target = dlg->targetConv();
            if (target.value.isEmpty()) return;
            const QString comment = dlg->comment();
            const QString fwd = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
            const QString full = comment.isEmpty() ? fwd : (comment + "\n" + fwd);
            _sessionOwner->sendMessage(target, full);
        });
        dlg->open();
    });

    connect(_composer, &ComposerWidget::sendRequested,
            this, [this](const QString &text) {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->sendMessage(_currentConvId, text);
    });
    connect(_composer, &ComposerWidget::uploadRequested,
            this, [this](const QString &filePath) {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->uploadFile(_currentConvId, filePath);
    });
    connect(_composer, &ComposerWidget::editRequested,
            this, [this](const Ts &ts, const QString &newText) {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->editMessage(_currentConvId, ts, newText);
    });
    connect(_composer, &ComposerWidget::editLastRequested, this, [this] {
        if (!_sessionOwner || !_messageList) return;
        const auto msg = _messageList->lastOwnMessage(_sessionOwner->meUserId());
        if (!msg) return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    connect(_composer, &ComposerWidget::typingStarted, this, [this] {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->sendTyping(_currentConvId);
    });
    connect(_composer, &ComposerWidget::scheduleRequested,
            this, [this](const QString &text, qint64 postAt) {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->scheduleMessage(_currentConvId, text, postAt);
    });

    connect(_convList, &ConvListWidget::conversationSelected,
            this, [this](int row) {
        const ConversationId id = _convList->conversationId(row);
        if (id.value.isEmpty()) return;
        const auto *conv = _sessionOwner ? _sessionOwner->findConversation(id) : nullptr;
        if (!conv) return;
        const QString name = _convList->resolvedName(row);
        const bool isDm = conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim;
        _convNameLabel->setText(isDm ? name : name.isEmpty() ? "" : "#" + name);
        updateHeaderForConv(id);
    });

    connect(_starBtn, &QPushButton::clicked, this, [this] {
        if (_currentConvId.value.isEmpty()) return;
        QSettings s("msga", "msga");
        const QString key = "starred/" + _activeTeamId + "/" + _currentConvId.value;
        const bool nowStarred = !s.value(key, false).toBool();
        s.setValue(key, nowStarred);
        updateStarBtn(nowStarred);
    });

    return rightPanel;
}

// ── Session lifecycle ─────────────────────────────────────────────────────────

void MainWindow::startSession(const QString &teamId) {
    if (teamId.isEmpty()) { showLoggedOut(); return; }

    // Build the main page once, lazily
    if (!_mainPage) {
        _mainPage = buildMainPage();
        _stack->addWidget(_mainPage);
    }

    // Tear down existing session
    _sessionLifetime = rpl::lifetime();
    _sessionOwner.reset();
    _currentConvId = {};
    _totalUnread   = 0;
    _totalMentions = 0;
    updateTrayIcon();
    _composer->setEnabled(false);
    _composer->hide();
    if (_msgHeader) _msgHeader->hide();
    if (_contentStack && _messageList) _contentStack->setCurrentWidget(_messageList);

    // Create new session
    const auto creds  = TokenStore::loadWorkspace(teamId);
    const auto appCfg = TokenStore::loadApp();
    const QString xapp = qEnvironmentVariable("SLACK_XAPP_TOKEN", appCfg.xapp);

    auto backend = std::make_unique<PublicBackend>(creds, appCfg, xapp);
    _sessionOwner = std::make_unique<Session>(std::move(backend), teamId);
    _messageList->setSession(_sessionOwner.get());
    if (_searchWidget) _searchWidget->setSession(_sessionOwner.get());
    if (_composer) _composer->setSession(_sessionOwner.get());
    if (_threadPanel) {
        _threadPanel->setSession(_sessionOwner.get());
        _threadPanel->close();
        _threadPanel->setVisible(false);
    }

    _activeTeamId = teamId;
    TokenStore::setActiveWorkspace(teamId);

    // Update switcher + title bar
    refreshSwitcher();
    if (_titleBar)
        _titleBar->setTitle(creds.teamName);

    // start() loads cache into _conversations/_users before connectToSession()
    // subscribes, so the first emission already carries cached data.
    _sessionOwner->start();

    // First load with no cache: hide the conv column and show a spinner in the
    // message area until conversations arrive from the network.
    if (_convPanel && _messageList) {
        const bool hasCached = !_sessionOwner->currentConversations().empty();
        _convPanel->setVisible(hasCached);
        _messageList->setWaiting(!hasCached);
    }

    connectToSession();
    _stack->setCurrentWidget(_mainPage);
}

void MainWindow::switchToWorkspace(const QString &teamId) {
    if (teamId == _activeTeamId) return;
    startSession(teamId);
}

void MainWindow::showLoggedOut() {
    _sessionLifetime = rpl::lifetime();
    _sessionOwner.reset();
    _activeTeamId.clear();
    _currentConvId = {};
    _totalUnread   = 0;
    _totalMentions = 0;
    updateTrayIcon();
    if (_titleBar) _titleBar->setTitle({});
    refreshSwitcher(); // switcher is always visible — deselect the active entry
    _stack->setCurrentWidget(_loggedOutPage);
}

bool MainWindow::runLoginFlow() {
    const auto appCfg = TokenStore::loadApp();
    if (appCfg.clientId.isEmpty()) {
        QMessageBox::critical(this, tr("Missing credentials"),
            tr("App credentials are not configured.\n\n"
               "Copy credentials.cmake.example to credentials.cmake, "
               "fill in your Slack app credentials, and rebuild."));
        return false;
    }

    OAuthFlow flow(appCfg);
    _activeFlow = &flow;
    bool success = false;
    QEventLoop loop;

    QObject::connect(&flow, &OAuthFlow::done,
                     [&](TokenStore::Credentials creds) {
        TokenStore::saveWorkspace(creds);
        _activeTeamId = creds.teamId;
        success = true;
        loop.quit();
    });
    QObject::connect(&flow, &OAuthFlow::failed,
                     [&](const QString &reason) {
        QMessageBox::critical(this, tr("Login failed"), reason);
        loop.quit();
    });

    flow.start();
    loop.exec();
    _activeFlow = nullptr;
    return success;
}

void MainWindow::handleOAuthUri(const QUrl &uri) {
    if (_activeFlow)
        _activeFlow->handleUri(uri);
}

void MainWindow::connectToSession() {
    // If the backend can't refresh the token (no refresh token, or refresh fails),
    // it sets authState → LoggedOut so we show the login screen instead of hanging.
    _sessionOwner->authState()
        | rpl::on_next([this](AuthState state) {
            if (state == AuthState::NotLoggedIn)
                showLoggedOut();
        }, _sessionLifetime);

    _sessionOwner->conversations()
        | rpl::on_next([this](std::vector<Conversation> convs) {
            populateConversations(convs);
            updateUnreadBadges(convs);
            // Reveal the conv column the moment real data arrives.
            if (!convs.empty() && _convPanel && !_convPanel->isVisible()) {
                if (_messageList) _messageList->setWaiting(false);
                _convPanel->show();
            }
            // On first populate (no conversation open yet), jump to the last
            // conversation the user had open in the previous session.
            if (_currentConvId.value.isEmpty()) {
                restoreLastConv();
                // If still no conversation after restore attempt, show tips now
                // that the list is ready (not during initial loading spinner).
                if (_currentConvId.value.isEmpty()
                        && _convPanel && _convPanel->isVisible()
                        && _contentStack && _welcomeTips)
                    _contentStack->setCurrentWidget(_welcomeTips);
            }
        }, _sessionLifetime);

    _sessionOwner->users()
        | rpl::on_next([this](std::vector<User> users) {
            if (_convList) {
                _convList->setUsers(users);
                _convList->setMe(_sessionOwner->meUserId());
            }
        }, _sessionLifetime);

    _sessionOwner->events()
        | rpl::on_next([this](Event e) {
            if (const auto *ev = std::get_if<EvMessageNew>(&e)) {
                maybeNotify(*ev);
            } else if (const auto *ev = std::get_if<EvPresenceChanged>(&e)) {
                if (_headerAvatar && _headerAvatar->isVisible()) {
                    const auto *conv = _sessionOwner->findConversation(_currentConvId);
                    if (conv && conv->dmUser && *conv->dmUser == ev->user)
                        _headerAvatar->setPresence(ev->active);
                }
            } else if (const auto *ev = std::get_if<EvDndChanged>(&e)) {
                if (_headerAvatar && _headerAvatar->isVisible()) {
                    const auto *conv = _sessionOwner->findConversation(_currentConvId);
                    if (conv && conv->dmUser && *conv->dmUser == ev->user)
                        _headerAvatar->setDnd(ev->dndEnabled);
                }
            }
        }, _sessionLifetime);

    _sessionOwner->errors()
        | rpl::on_next([this](QString msg) {
            showNetworkError(msg);
        }, _sessionLifetime);
}

void MainWindow::showNetworkError(const QString &message) {
    if (!_errorBanner) return;
    _errorBanner->setText(message);
    _errorBanner->show();
    QTimer::singleShot(5000, _errorBanner, &QWidget::hide);
}

void MainWindow::applyUpdateAndRestart(const QString &staged) {
#if defined(Q_OS_LINUX)
    const QString target = QCoreApplication::applicationFilePath();
    if (!QFile::rename(staged, target)) {
        showNetworkError(tr("Could not replace binary — check file permissions."));
        return;
    }
    _updateChecker->clearStaged();
    QProcess::startDetached(target, QCoreApplication::arguments());
    QCoreApplication::quit();
#elif defined(Q_OS_MACOS)
    _updateChecker->clearStaged();
    QDesktopServices::openUrl(QUrl::fromLocalFile(staged));
#else
    Q_UNUSED(staged)
#endif
}

void MainWindow::maybeNotify(const EvMessageNew &ev) {
    QSettings s("msga", "msga");
    if (!s.value("notifications/enabled", false).toBool()) return;

    // Skip own messages and bot messages with no author
    const UserId me = _sessionOwner->meUserId();
    if (!me.value.isEmpty() && ev.msg.author == me) return;

    // Skip if the window is focused and this conversation is already open
    if (isActiveWindow() && ev.conv == _currentConvId) return;

    const int level = s.value("notifications/level", 1).toInt();
    const auto *conv = _sessionOwner->findConversation(ev.conv);

    if (conv && conv->isMuted) return;

    if (level == 1) { // DMs and mentions only
        const bool isDm = conv &&
            (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
        const bool isMention = !me.value.isEmpty() &&
            ev.msg.text.text.contains(QString("<@%1>").arg(me.value));
        if (!isDm && !isMention) return;
    }

    // Build title and body
    const auto *sender = _sessionOwner->findUser(ev.msg.author);
    const QString senderName = sender
        ? (sender->displayName.isEmpty() ? sender->name : sender->displayName)
        : tr("Someone");

    QString title, body;
    if (conv && (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim)) {
        title = senderName;
        body  = ev.msg.text.text;
    } else {
        const QString convName = conv ? ("#" + conv->name) : tr("a channel");
        title = convName;
        body  = senderName + ": " + ev.msg.text.text;
    }
    if (body.length() > 100) body = body.left(97) + "…";

    _pendingNotifConv = ev.conv;
    _trayIcon->showMessage(title, body, QSystemTrayIcon::NoIcon, 5000);
}

void MainWindow::updateUnreadBadges(const std::vector<Conversation> &convs) {
    int total = 0, mentions = 0;
    for (const auto &c : convs) {
        if (c.isMuted) continue;
        total += c.unread;
        const bool isDm = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
        mentions += isDm ? c.unread : c.mentionCount;
    }
    if (total == _totalUnread && mentions == _totalMentions) return;
    _totalUnread   = total;
    _totalMentions = mentions;
    if (_switcher)
        _switcher->setUnread(_activeTeamId, _totalUnread);
    updateTrayIcon();
}

void MainWindow::updateTrayIcon() {
    if (!_trayIcon) return;
    if (_totalUnread <= 0) {
        _trayIcon->setIcon(QIcon(":/icon_tray.svg"));
        return;
    }
    // Compose base icon + red notification dot at top-right corner
    const int sz = 128;
    QSvgRenderer renderer(QString(":/icon_tray.svg"));
    QPixmap px(sz, sz);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    if (renderer.isValid())
        renderer.render(&p, QRectF(0, 0, sz, sz));
    const int d = 36;
    p.setBrush(_totalMentions > 0 ? Theme::kMentionBadge : QColor(180, 180, 180));
    p.setPen(Qt::NoPen);
    p.drawEllipse(sz - d, 0, d, d);
    p.end();
    _trayIcon->setIcon(QIcon(px));
}

// ── Workspace management ──────────────────────────────────────────────────────

void MainWindow::refreshSwitcher() {
    if (!_switcher) return;

    const auto ids = TokenStore::workspaceIds();
    std::vector<WorkspaceSwitcher::Entry> entries;
    entries.reserve(ids.size());
    for (const auto &id : ids) {
        const auto c = TokenStore::loadWorkspace(id);
        entries.push_back({c.teamId, c.teamName, c.iconUrl});
    }
    _switcher->setWorkspaces(entries);
    _switcher->setActive(_activeTeamId);
}

void MainWindow::logoutWorkspace(const QString &teamId) {
    const bool wasActive = (teamId == _activeTeamId);

    if (wasActive) {
        _sessionLifetime = rpl::lifetime();
        _sessionOwner.reset();
        _activeTeamId.clear();
    }

    TokenStore::removeWorkspace(teamId);

    const auto remaining = TokenStore::workspaceIds();
    if (remaining.isEmpty()) {
        if (_messageList) _messageList->setSession(nullptr);
        _currentConvId = {};
        showLoggedOut();
    } else if (wasActive) {
        startSession(remaining.first());
    } else {
        refreshSwitcher();
    }
}

void MainWindow::showWorkspaceMenu(const QString &teamId, const QPoint &globalPos) {
    const auto creds = TokenStore::loadWorkspace(teamId);
    auto *menu = new ContextMenu(this);
    menu->addItem(
        creds.teamName.isEmpty()
            ? tr("Log out")
            : tr("Log out from %1").arg(creds.teamName),
        [this, teamId] { logoutWorkspace(teamId); },
        /*destructive=*/true
    );
    menu->popup(globalPos);
}

// ── Tray ──────────────────────────────────────────────────────────────────────

void MainWindow::setupTray() {
    _trayIcon = new QSystemTrayIcon(QIcon(":/icon_tray.svg"), this);
    _trayIcon->setToolTip("MSGA");

    auto *menu = new QMenu(this);

    const auto ids = TokenStore::workspaceIds();
    for (const auto &id : ids) {
        const auto creds = TokenStore::loadWorkspace(id);
        const QString label = creds.teamName.isEmpty() ? id : creds.teamName;
        menu->addAction(label, this, [this, id] {
            show();
            raise();
            activateWindow();
            QMetaObject::invokeMethod(this, [this, id] {
                switchToWorkspace(id);
            }, Qt::QueuedConnection);
        });
    }

    menu->addSeparator();
    menu->addAction(tr("Settings"), this, [this] {
        show();
        raise();
        activateWindow();
        QMetaObject::invokeMethod(this, [this] {
            _settingsDialog->open();
        }, Qt::QueuedConnection);
    });
    menu->addSeparator();
    menu->addAction(tr("Quit"), qApp, &QApplication::quit);
    _trayIcon->setContextMenu(menu);

    connect(_trayIcon, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick) {
            show();
            raise();
            activateWindow();
        }
    });

    connect(_trayIcon, &QSystemTrayIcon::messageClicked, this, [this] {
        show();
        raise();
        activateWindow();
        if (_pendingNotifConv.value.isEmpty()) return;
        const int row = _convList->rowForId(_pendingNotifConv);
        if (row >= 0) openConversation(row);
        _pendingNotifConv = {};
    });

    _trayIcon->show();
}

// ── Event handlers ────────────────────────────────────────────────────────────

static Qt::Edges resizeEdgesAt(const QPoint &pos, const QSize &sz) {
    Qt::Edges edges;
    if (pos.x() < kResizeBorder)                      edges |= Qt::LeftEdge;
    if (pos.x() >= sz.width()  - kResizeBorder)       edges |= Qt::RightEdge;
    if (pos.y() < kResizeBorder)                      edges |= Qt::TopEdge;
    if (pos.y() >= sz.height() - kResizeBorder)       edges |= Qt::BottomEdge;
    return edges;
}

static Qt::CursorShape cursorForEdges(Qt::Edges edges) {
    if ((edges & Qt::TopEdge)    && (edges & Qt::LeftEdge))  return Qt::SizeFDiagCursor;
    if ((edges & Qt::TopEdge)    && (edges & Qt::RightEdge)) return Qt::SizeBDiagCursor;
    if ((edges & Qt::BottomEdge) && (edges & Qt::LeftEdge))  return Qt::SizeBDiagCursor;
    if ((edges & Qt::BottomEdge) && (edges & Qt::RightEdge)) return Qt::SizeFDiagCursor;
    if (edges & (Qt::LeftEdge  | Qt::RightEdge))  return Qt::SizeHorCursor;
    if (edges & (Qt::TopEdge   | Qt::BottomEdge)) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e) {
    if (!isMaximized() && !isFullScreen()) {
        auto *w = qobject_cast<QWidget *>(obj);
        if (w && w->window() == this) {
            if (e->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(e);
                if (me->button() == Qt::LeftButton && !_resizeEdges) {
                    const QPoint fp = _frame->mapFromGlobal(me->globalPosition().toPoint());
                    const Qt::Edges edges = resizeEdgesAt(fp, _frame->size());
                    if (edges) {
                        if (_resizeHoverCursor) {
                            QGuiApplication::restoreOverrideCursor();
                            _resizeHoverCursor = false;
                        }
                        if (QGuiApplication::platformName() == "wayland") {
                            if (auto *h = windowHandle()) h->startSystemResize(edges);
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
                    QRect r = _resizeWinAtDrag;
                    if (_resizeEdges & Qt::LeftEdge)   r.setLeft(  r.left()   + delta.x());
                    if (_resizeEdges & Qt::RightEdge)  r.setRight( r.right()  + delta.x());
                    if (_resizeEdges & Qt::TopEdge)    r.setTop(   r.top()    + delta.y());
                    if (_resizeEdges & Qt::BottomEdge) r.setBottom(r.bottom() + delta.y());
                    const QSize minS = minimumSize();
                    if (r.width()  < minS.width())
                        (_resizeEdges & Qt::LeftEdge) ? r.setLeft(r.right() - minS.width())   : r.setRight( r.left()  + minS.width());
                    if (r.height() < minS.height())
                        (_resizeEdges & Qt::TopEdge)  ? r.setTop( r.bottom()- minS.height())  : r.setBottom(r.top()   + minS.height());
                    setGeometry(r);
                    return true;
                }
                const QPoint fp = _frame->mapFromGlobal(me->globalPosition().toPoint());
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
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::updateRoundedMask() {
    if (!_frame) return;
    if (isMaximized() || isFullScreen()) {
        _frame->clearMask();
        return;
    }
    static constexpr int kRadius = 8;
    QBitmap bmp(_frame->size());
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
    if (e->type() == QEvent::WindowStateChange)
        updateRoundedMask();
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent *e) {
    hide();
    e->ignore();
}

void MainWindow::populateConversations(const std::vector<Conversation> &convs) {
    _convList->setConversations(convs);
}

void MainWindow::openConversation(int row) {
    if (!_sessionOwner) return;

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
    if (_currentConvId.value.isEmpty()) return;

    const QString name = _convList->resolvedName(row);
    const auto *conv = _sessionOwner->findConversation(_currentConvId);
    const bool isDm = conv && (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
    const QString displayName = isDm ? name : name.isEmpty() ? "" : "#" + name;

    // Build the channel/DM intro description for the message list header.
    QString description;
    if (conv) {
        if (isDm) {
            if (!name.isEmpty())
                description = tr("This is the beginning of your direct message history with %1.").arg(name);
        } else if (!conv->description.isEmpty()) {
            description = conv->description;
        }
    }

    const bool hasCachedMsgs = !_sessionOwner->cachedMessages(_currentConvId).empty();

    _sessionOwner->setReading(_currentConvId);
    if (_contentStack) _contentStack->setCurrentWidget(_messageList);
    _messageList->openConversation(_currentConvId, displayName, description);
    _composer->setEnabled(true);
    _composer->setConvKind(conv ? conv->kind : ConvKind::PublicChannel);
    _composer->setPlaceholderText(
        displayName.isEmpty() ? tr("Message") : tr("Message %1").arg(displayName));

    // Restore any unsent draft for this conversation.
    _composer->setText(_drafts.value(_currentConvId.value));

    if (hasCachedMsgs) {
        if (_msgHeader) _msgHeader->show();
        _composer->show();
    } else {
        // Messages are loading; keep header and composer hidden until the first
        // page is ready so the user doesn't see chrome around an empty chat area.
        if (_msgHeader) _msgHeader->hide();
        _composer->hide();
        connect(_messageList, &MessageListWidget::initialPageLoaded,
                this, [this, convId = _currentConvId] {
            if (_currentConvId != convId) return;
            if (_msgHeader) _msgHeader->show();
            if (_composer)  _composer->show();
        }, Qt::SingleShotConnection);
    }

    _sessionOwner->saveLastConv(_currentConvId, displayName);
    updateHeaderForConv(_currentConvId);
}

void MainWindow::updateStarBtn(bool starred) {
    if (!_starBtn) return;
    _starBtn->setIcon(svgIcon(":/ui/star.svg", QSize(15, 15),
                               starred ? QColor("#C6920A") : QColor("#888888")));
}

void MainWindow::updateHeaderForConv(const ConversationId &conv) {
    if (conv.value.isEmpty()) return;

    // Star button state
    if (_starBtn) {
        _starBtn->setVisible(true);
        QSettings s("msga", "msga");
        updateStarBtn(s.value("starred/" + _activeTeamId + "/" + conv.value, false).toBool());
    }

    if (!_sessionOwner) return;
    const auto *conversation = _sessionOwner->findConversation(conv);
    const bool isDm = conversation &&
        (conversation->kind == ConvKind::Im || conversation->kind == ConvKind::Mpim);

    if (_headerAvatar) {
        _headerAvatar->setVisible(isDm);
        _headerAvatar->clearAvatar();
        if (isDm && conversation->dmUser) {
            const auto *u = _sessionOwner->findUser(*conversation->dmUser);
            if (u) {
                _headerAvatar->setPresence(u->isActive);
                _headerAvatar->setDnd(u->dndEnabled);
                _headerAvatar->setDisplayName(u->displayName.isEmpty() ? u->name : u->displayName);
                _sessionOwner->requestPresence(*conversation->dmUser);
                if (!u->avatarUrl.isEmpty() && _imgCache) {
                    const QPixmap cached = _imgCache->get(u->avatarUrl);
                    if (!cached.isNull()) {
                        _headerAvatar->setPixmap(cached);
                    } else {
                        // Not yet in cache — subscribe once and apply when it arrives.
                        const QString url = u->avatarUrl;
                        connect(_imgCache, &ImageCache::loaded,
                                this, [this, url, conv](const QString &loadedUrl) {
                            if (loadedUrl != url || conv != _currentConvId) return;
                            const QPixmap px = _imgCache->get(url);
                            if (!px.isNull() && _headerAvatar)
                                _headerAvatar->setPixmap(px);
                        }, Qt::SingleShotConnection);
                    }
                }
            }
        }
    }
}

void MainWindow::restoreLastConv() {
    if (!_sessionOwner) return;
    auto [lastConvId, lastConvName] = _sessionOwner->loadLastConv();
    if (lastConvId.value.isEmpty()) return;
    const int row = _convList->rowForId(lastConvId);
    if (row >= 0) _convList->selectRow(row);
}
