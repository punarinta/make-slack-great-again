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
#include <QNetworkAccessManager>
#include <QUrl>
#include <memory>

class Session;
class OAuthFlow;
class MessageListWidget;
class ComposerWidget;
class ConvListWidget;
class WorkspaceSwitcher;
class SettingsDialog;
class SearchWidget;
class TitleBar;
class ThreadPanel;
class HeaderAvatarWidget;
class QSplitter;

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
    QWidget *buildMainPage();    // session-agnostic; built lazily

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

    // Event handlers
    bool eventFilter(QObject *o, QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void updateRoundedMask();
    void populateConversations(const std::vector<Conversation> &convs);
    void openConversation(int row);

    std::unique_ptr<Session> _sessionOwner;
    QString    _activeTeamId;
    OAuthFlow *_activeFlow = nullptr; // non-owning; valid only while runLoginFlow() blocks

    // Window frame
    TitleBar    *_titleBar    = nullptr;
    QWidget     *_frame       = nullptr;   // central widget; hosts titleBar + _stack
    QVBoxLayout *_frameLayout = nullptr;

    QStackedWidget      *_stack         = nullptr;
    QWidget             *_loggedOutPage = nullptr;
    QWidget             *_mainPage      = nullptr;   // built lazily

    WorkspaceSwitcher   *_switcher       = nullptr;
    SettingsDialog      *_settingsDialog = nullptr;
    QWidget             *_convPanel      = nullptr;
    ConvListWidget      *_convList       = nullptr;
    QSplitter           *_msgSplitter    = nullptr;
    MessageListWidget   *_messageList    = nullptr;
    ComposerWidget      *_composer       = nullptr;
    SearchWidget        *_searchWidget   = nullptr;
    ThreadPanel         *_threadPanel    = nullptr;

    std::vector<ConversationId> _convIds;
    ConversationId              _currentConvId;
    ConversationId              _pendingNotifConv;
    rpl::lifetime               _sessionLifetime;

    QHash<QString, QString>     _drafts; // convId.value → unsent draft text

    QSystemTrayIcon     *_trayIcon          = nullptr;
    QPushButton         *_starBtn           = nullptr;
    HeaderAvatarWidget  *_headerAvatar      = nullptr;
    QNetworkAccessManager *_headerNam       = nullptr;

    // Manual resize state (non-Wayland)
    Qt::Edges _resizeEdges      = {};
    QPoint    _resizeDragStart;
    QRect     _resizeWinAtDrag;
    bool      _resizeHoverCursor = false;
};
