// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/user_avatar.h"

#include <QPixmap>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QWidget>

class ImageCache;
class PopupTooltip;

// Footer pinned to the bottom of the conversation list. Shows the authed user's
// avatar (rounded square with a presence/DND dot, same size as the workspace
// icons) plus a matching rounded-square ghost button that flips the user's own
// presence between:
//   • "visible" — automatic presence (users.setPresence "auto"), circle-user-round icon
//   • "hidden"  — manually away  (users.setPresence "away"),  hat-glasses icon
//
// Slack offers no API to force "active": a user is active only while one of
// Slack's clients holds a socket. The Session holds such a socket itself when
// the presence preference asks for it (PresenceMode / Session::presenceLink());
// the toggle here stays auto vs. away (visible vs. hidden), and the tooltip
// explains a lingering "away" from the link's state.
class ConvFooterWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConvFooterWidget(ImageCache *imgCache, QWidget *parent = nullptr);

    // Identity + avatar of the authed user (avatar loaded lazily via ImageCache).
    void setUser(const QString &displayName, const QString &avatarUrl);
    // Rich self presence: drives the avatar dot and the toggle icon/tooltip.
    void setSelfPresence(const SelfPresence &sp);
    // State of the presence-holding link (Session::presenceLink()) — only used to
    // word the toggle's tooltip while the user still appears away.
    void setPresenceLink(PresenceLinkState link);
    // Whether the active backend has a user-presence concept (Capabilities::presence).
    // When false (e.g. IMAP/email), the visible/hidden presence toggle is dropped
    // entirely — there's nothing to toggle and the avatar shows no presence dot.
    void setPresenceSupported(bool supported);
    // Zen mode (Capabilities::zenMode) takes the toggle's place on a backend
    // without self presence: eye = everything shown, leaf = details hidden.
    void setZenMode(bool supported, bool on);
    // Capabilities::selfStatus: offer "Manage status" in the avatar menu.
    void setStatusSupported(bool supported) { _statusSupported = supported; }
    // Clear on logout / workspace teardown.
    void clear();

signals:
    // Requested new presence: away=true → hidden, away=false → visible.
    void presenceToggleRequested(bool away);
    // The Zen mode toggle was clicked; `on` is the new state (already shown).
    void zenModeToggled(bool on);
    // Picked from the avatar's context menu — open the "Manage profile" dialog.
    void manageProfileRequested();
    // Picked from the avatar's context menu — open the "Set a status" dialog.
    void manageStatusRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    enum class Hot { None, Avatar, Toggle, Tasks };

    QRect   avatarRect() const;
    QRect   toggleRect() const;
    // Rounded-square indicator left of the toggle; only present while background
    // tasks are running (null rect otherwise, so it never gets hit-tested).
    QRect   tasksRect() const;
    bool    hasTasks() const { return _taskCount > 0; }
    Hot     hitTest(const QPoint &) const;
    void    setHot(Hot);
    // Pops the "Manage profile / Manage status" menu anchored above the avatar.
    void    showAvatarMenu();
    void    loadAvatar();
    QString presenceTooltip() const;
    QString zenTooltip() const;
    QString tasksTooltip() const;
    // Shows the task-list popup above the spinner: count header + descriptions.
    void    showTasksTooltip();

    // Cross-fade the toggle icon toward `hidden`. Used both optimistically on
    // click (instant feedback) and to settle on the authoritative state.
    static QString iconFor(bool hidden);
    void           animateTo(bool hidden);
    void           tickAnim();

    ImageCache             *_imgCache = nullptr;
    PopupTooltip           *_tooltip  = nullptr;
    QString                 _displayName;
    QString                 _avatarUrl;
    QPixmap                 _avatar;
    // Live subscription to ImageCache::loaded while our avatar is in flight.
    // Must NOT be single-shot: `loaded` fires for every image, so a single-shot
    // connection gets consumed by the first unrelated image that finishes and we
    // miss our own (a race we reliably lost on Windows — the avatar stayed grey).
    QMetaObject::Connection _avatarConn;
    UserAvatar::State       _state;
    SelfPresence            _sp;
    PresenceLinkState       _link              = PresenceLinkState::Off;
    bool                    _presenceSupported = true; // backend has a presence concept
    bool                    _zenSupported      = false;
    bool                    _statusSupported   = true;
    bool                    _zenOn             = false;
    Hot                     _hot               = Hot::None;
    Hot                     _pressed           = Hot::None;

    // Optimistic toggle-icon cross-fade.
    bool   _displayHidden = false; // which icon is settled/targeted
    bool   _animFrom      = false;
    bool   _animTo        = false;
    qreal  _animProgress  = 1.0; // 1.0 = settled (no cross-fade in flight)
    QTimer _animTimer;
    QTimer _confirmTimer; // safety: revert the optimistic icon if no server confirmation arrives

    // Background-task spinner (rotating cog) shown while BackgroundTasks::count() > 0.
    int    _taskCount = 0;
    qreal  _taskAngle = 0.0; // current cog rotation, degrees
    QTimer _taskTimer;
    void   setTaskCount(int count);
    void   tickTaskSpin();
};
