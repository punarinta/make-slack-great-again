// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QWidget>

class BrowseListView;
class ImageCache;
class QLabel;
class QStackedWidget;
class Session;
class StyledButton;

// A teammate's page in an agent workspace (Backend::agentRoles), opened from
// the Team section: who it is, its sessions (click one to open it) and the
// folder a new session with it starts in. Writing to a teammate starts that
// session — MainWindow keeps the composer under this page and hands the text
// to Session::startAgentSession with the teammate's role and folder().
// Lives in MainWindow's content stack like the Threads and Saved pages.
class TeammatePage : public QWidget {
    Q_OBJECT
public:
    explicit TeammatePage(ImageCache *imgCache, QWidget *parent = nullptr);

    void setSession(Session *session);

    // Show `mate` (the list follows the session live while the page is up).
    void             open(const AgentRole &mate);
    // Forget the teammate (workspace switch / logout).
    void             clear();
    const AgentRole &teammate() const { return _mate; }

    // Where a new session starts: the folder last used with this teammate,
    // else the last one used for any session, else home.
    QString folder() const { return _folder; }
    // Why no session can start in folder() right now ("" = one can) — the
    // composer under the page says it, locked, like a read-only chat's.
    QString blocker() const { return _blocker; }

signals:
    void openSessionRequested(ConversationId conv);
    void editRequested(const QString &roleId);
    // folder() or blocker() changed.
    void folderChanged();

private:
    void applyTheme();
    void rebuild();
    void scheduleRebuild();
    void setFolder(const QString &dir);
    void chooseFolder();
    void updateAvatar();

    Session    *_session  = nullptr;
    ImageCache *_imgCache = nullptr;
    AgentRole   _mate;
    QString     _folder;
    QString     _blocker;
    bool        _rebuildQueued = false;

    QWidget        *_header      = nullptr;
    QLabel         *_avatar      = nullptr;
    QLabel         *_name        = nullptr;
    QLabel         *_description = nullptr;
    StyledButton   *_editBtn     = nullptr;
    QLabel         *_listTitle   = nullptr;
    QStackedWidget *_stack       = nullptr;
    QLabel         *_empty       = nullptr;
    BrowseListView *_list        = nullptr;
    QWidget        *_footer      = nullptr;
    QLabel         *_folderLabel = nullptr;
    StyledButton   *_folderBtn   = nullptr;

    rpl::lifetime _sessionLifetime; // conversations()/users() subscriptions
};
