// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "main_window.h"
#include "theme.h"
#include "header_avatar_widget.h"
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

#include "ui/icon_utils.h"

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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>

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

    _stack = new QStackedWidget(_frame);
    _frameLayout->addWidget(_stack, 1);

    setCentralWidget(_frame);

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
    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);
    root->setSpacing(0);
    root->setContentsMargins(0, 0, 0, 0);

    // ── Workspace switcher (leftmost column) ──────────────────────
    _switcher = new WorkspaceSwitcher(page);
    _switcher->setObjectName("workspaceSidebar");
    connect(_switcher, &WorkspaceSwitcher::workspaceClicked,
            this, &MainWindow::switchToWorkspace);
    connect(_switcher, &WorkspaceSwitcher::addWorkspaceClicked, this, [this] {
        if (runLoginFlow())
            startSession(_activeTeamId);
    });
    connect(_switcher, &WorkspaceSwitcher::workspaceRightClicked,
            this, &MainWindow::showWorkspaceMenu);

    _settingsDialog = new SettingsDialog(_stack);
    connect(_switcher, &WorkspaceSwitcher::settingsClicked,
            _settingsDialog, &SettingsDialog::open);

    // ── Conversation panel ────────────────────────────────────────
    _convPanel = new QWidget(page);
    _convPanel->setObjectName("convPanel");
    _convPanel->setFixedWidth(240);

    auto *convLayout = new QVBoxLayout(_convPanel);
    convLayout->setContentsMargins(0, 0, 0, 0);
    convLayout->setSpacing(0);

    _convList = new ConvListWidget(_convPanel);
    _convList->setObjectName("convList");
    convLayout->addWidget(_convList);

    connect(_convList, &ConvListWidget::conversationSelected,
            this, &MainWindow::openConversation);

    // ── Right panel: header bar + content stack + composer ───────────
    auto *rightPanel  = new QWidget(page);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // Header bar: conversation name + search button
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

    // Avatar with presence dot (only visible for DM conversations)
    _headerAvatar = new HeaderAvatarWidget(msgHeader);
    _headerAvatar->setVisible(false);
    msgHeaderLayout->addWidget(_headerAvatar);

    auto *convNameLabel = new QLabel("", msgHeader);
    convNameLabel->setObjectName("convNameLabel");
    convNameLabel->setStyleSheet("font-weight: bold; font-size: 15px; color: #1D1C1D;");
    msgHeaderLayout->addWidget(convNameLabel, 1);

    // Star/unstar button
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
    rightLayout->addWidget(msgHeader);

    // Horizontal splitter: channel view (left) + thread panel (right, hidden until opened)
    _msgSplitter = new QSplitter(Qt::Horizontal, rightPanel);
    _msgSplitter->setHandleWidth(1);
    _msgSplitter->setChildrenCollapsible(false);
    rightLayout->addWidget(_msgSplitter, 1);

    // ── Left side: content stack (msg list / search) + channel composer ──
    auto *msgArea   = new QWidget(_msgSplitter);
    auto *msgLayout = new QVBoxLayout(msgArea);
    msgLayout->setContentsMargins(0, 0, 0, 0);
    msgLayout->setSpacing(0);

    // Content stack: message list OR search panel
    auto *contentStack = new QStackedWidget(msgArea);
    msgLayout->addWidget(contentStack, 1);

    _messageList = new MessageListWidget(nullptr, contentStack);
    contentStack->addWidget(_messageList);

    _searchWidget = new SearchWidget(contentStack);
    contentStack->addWidget(_searchWidget);

    _composer = new ComposerWidget(msgArea);
    _composer->setEnabled(false);
    msgLayout->addWidget(_composer);

    _msgSplitter->addWidget(msgArea);

    // ── Right side: thread panel (hidden by default) ──
    _threadPanel = new ThreadPanel(_msgSplitter);
    _threadPanel->setVisible(false);
    _msgSplitter->addWidget(_threadPanel);
    _msgSplitter->setStretchFactor(0, 1);
    _msgSplitter->setStretchFactor(1, 0);

    // Show/hide search
    connect(searchBtn, &QPushButton::clicked, this, [this, contentStack] {
        if (contentStack->currentWidget() == _searchWidget)
            contentStack->setCurrentIndex(0);
        else
            contentStack->setCurrentWidget(_searchWidget);
    });
    connect(_searchWidget, &SearchWidget::closeRequested, this, [contentStack] {
        contentStack->setCurrentIndex(0);
    });
    connect(_searchWidget, &SearchWidget::resultSelected,
            this, [this, convNameLabel](ConversationId conv, Ts /*ts*/) {
        const int row = _convList->rowForId(conv);
        if (row >= 0) openConversation(row);
    });

    // Thread panel open/close
    connect(_messageList, &MessageListWidget::threadClicked,
            this, [this](ConversationId conv, Ts rootTs) {
        _threadPanel->setVisible(true);
        _threadPanel->openThread(conv, rootTs);
        // Give thread panel ~360px on first open; user can resize after that.
        if (_msgSplitter->sizes().at(1) < 100) {
            const int total = _msgSplitter->width();
            _msgSplitter->setSizes({total - 360, 360});
        }
    });
    connect(_threadPanel, &ThreadPanel::closeRequested, this, [this] {
        _threadPanel->close();
        _threadPanel->setVisible(false);
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
    connect(_composer, &ComposerWidget::editLastRequested,
            this, [this] {
        if (!_sessionOwner || !_messageList) return;
        const auto msg = _messageList->lastOwnMessage(_sessionOwner->meUserId());
        if (!msg) return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    connect(_composer, &ComposerWidget::typingStarted,
            this, [this] {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->sendTyping(_currentConvId);
    });
    connect(_composer, &ComposerWidget::scheduleRequested,
            this, [this](const QString &text, qint64 postAt) {
        if (_sessionOwner && !_currentConvId.value.isEmpty())
            _sessionOwner->scheduleMessage(_currentConvId, text, postAt);
    });

    // Keep convNameLabel and header state in sync with the opened conversation
    connect(_convList, &ConvListWidget::conversationSelected,
            this, [this, convNameLabel](int row) {
        const ConversationId id = _convList->conversationId(row);
        if (id.value.isEmpty()) return;
        const auto *conv = _sessionOwner
            ? _sessionOwner->findConversation(id)
            : nullptr;
        if (!conv) return;
        const QString name = _convList->resolvedName(row);
        const bool isDm = conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim;
        convNameLabel->setText(isDm ? name : name.isEmpty() ? "" : "#" + name);
        updateHeaderForConv(id);
    });

    // Star button toggle
    connect(_starBtn, &QPushButton::clicked, this, [this] {
        if (_currentConvId.value.isEmpty()) return;
        QSettings s("msga", "msga");
        const QString key = "starred/" + _activeTeamId + "/" + _currentConvId.value;
        const bool nowStarred = !s.value(key, false).toBool();
        s.setValue(key, nowStarred);
        updateStarBtn(nowStarred);
    });

    root->addWidget(_switcher);
    root->addWidget(_convPanel);
    root->addWidget(rightPanel, 1);

    return page;
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
    _composer->setEnabled(false);

    // Create new session
    const auto creds  = TokenStore::loadWorkspace(teamId);
    const auto appCfg = TokenStore::loadApp();
    const QString xapp = qEnvironmentVariable("SLACK_XAPP_TOKEN", appCfg.xapp);

    auto backend = std::make_unique<PublicBackend>(creds.xoxp, xapp);
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
    if (_titleBar) _titleBar->setTitle({});
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
    _sessionOwner->conversations()
        | rpl::on_next([this](std::vector<Conversation> convs) {
            populateConversations(convs);
            // On first populate (no conversation open yet), jump to the last
            // conversation the user had open in the previous session.
            if (_currentConvId.value.isEmpty())
                restoreLastConv();
        }, _sessionLifetime);

    _sessionOwner->users()
        | rpl::on_next([this](std::vector<User> users) {
            if (_convList) _convList->setUsers(users);
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
            }
        }, _sessionLifetime);
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
    menu->addAction(tr("Show"), this, [this] {
        show();
        raise();
        activateWindow();
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

    _sessionOwner->setReading(_currentConvId);
    _messageList->openConversation(_currentConvId, displayName, description);
    _composer->setEnabled(true);
    _composer->setConvKind(conv ? conv->kind : ConvKind::PublicChannel);
    _composer->setPlaceholderText(
        displayName.isEmpty() ? tr("Message") : tr("Message %1").arg(displayName));

    // Restore any unsent draft for this conversation.
    _composer->setText(_drafts.value(_currentConvId.value));

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
                // Fetch live presence — `isActive` starts false (not loaded by users.list).
                _sessionOwner->requestPresence(*conversation->dmUser);
                if (!u->avatarUrl.isEmpty()) {
                    if (!_headerNam) _headerNam = new QNetworkAccessManager(this);
                    const QString url = u->avatarUrl;
                    auto *reply = _headerNam->get(QNetworkRequest(QUrl(url)));
                    connect(reply, &QNetworkReply::finished, this, [this, reply, conv] {
                        reply->deleteLater();
                        if (conv != _currentConvId) return;
                        if (reply->error() == QNetworkReply::NoError) {
                            QPixmap px;
                            if (px.loadFromData(reply->readAll()) && !px.isNull())
                                if (_headerAvatar) _headerAvatar->setPixmap(px);
                        }
                    });
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
