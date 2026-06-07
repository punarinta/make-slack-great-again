// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "auth/token_store.h"
#include "rpl/lifetime.h"

#include <QMainWindow>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QSystemTrayIcon>
#include <QPushButton>
#include <QUrl>
#include <memory>

class Session;
class OAuthFlow;
class ImageCache;
class MessageListWidget;
class ComposerWidget;
class ConvListWidget;
class WorkspaceSwitcher;
class SettingsDialog;
class SearchWidget;
class WelcomeWidget;
class TitleBar;
class ThreadPanel;
class HeaderAvatarWidget;
class QSplitter;
class UpdateBar;
class UpdateChecker;

class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
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

    // Session lifecycle
    void startSession(const QString &teamId);
    void switchToWorkspace(const QString &teamId);
    void showLoggedOut();
    bool runLoginFlow();
    void connectToSession();
    void restoreLastConv();

    // Workspace management
    void refreshSwitcher();
    void logoutWorkspace(const QString &teamId);
    void showWorkspaceMenu(const QString &teamId, const QPoint &globalPos);

    // Header helpers
    void updateHeaderForConv(const ConversationId &conv);
    void updateStarBtn(bool starred);

    // Tray
    void setupTray();
    void maybeNotify(const EvMessageNew &ev);
    void updateUnreadBadges(const std::vector<Conversation> &convs);
    void updateTrayIcon();

    // Error banner — shown briefly when a network error occurs with no UI handler.
    void showNetworkError(const QString &message);

    // Update
    void applyUpdateAndRestart(const QString &stagedPath);

    // Event handlers
    bool eventFilter(QObject *o, QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void updateRoundedMask();
    void populateConversations(const std::vector<Conversation> &convs);
    void openConversation(int row);

    std::unique_ptr<Session> _sessionOwner;
    QString                  _activeTeamId;
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

    WorkspaceSwitcher *_switcher       = nullptr;
    SettingsDialog    *_settingsDialog = nullptr;
    QWidget           *_convPanel      = nullptr;
    ConvListWidget    *_convList       = nullptr;
    QLabel            *_convNameLabel  = nullptr;
    QWidget           *_msgHeader      = nullptr;
    QSplitter         *_msgSplitter    = nullptr;
    QStackedWidget    *_contentStack   = nullptr;
    MessageListWidget *_messageList    = nullptr;
    ComposerWidget    *_composer       = nullptr;
    SearchWidget      *_searchWidget   = nullptr;
    WelcomeWidget     *_welcomeTips    = nullptr;
    ThreadPanel       *_threadPanel    = nullptr;

    std::vector<ConversationId> _convIds;
    ConversationId              _currentConvId;
    ConversationId              _pendingNotifConv;
    int                         _totalUnread   = 0;
    int                         _totalMentions = 0; // DM unreads + channel @mentions
    rpl::lifetime               _sessionLifetime;

    QHash<QString, QString> _drafts; // convId.value → unsent draft text

    ImageCache         *_imgCache     = nullptr;
    QLabel             *_errorBanner  = nullptr;
    QSystemTrayIcon    *_trayIcon     = nullptr;
    QPushButton        *_starBtn      = nullptr;
    HeaderAvatarWidget *_headerAvatar = nullptr;

    // Manual resize state (non-Wayland)
    Qt::Edges _resizeEdges = {};
    QPoint    _resizeDragStart;
    QRect     _resizeWinAtDrag;
    bool      _resizeHoverCursor = false;
};
