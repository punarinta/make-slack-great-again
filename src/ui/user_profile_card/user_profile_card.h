// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QPixmap>
#include <QTimer>
#include <QWidget>

// Floating profile card shown when hovering a user @mention in the message list.
// Shows: role header (owner/admin/app/deactivated), avatar, name + presence dot,
// status, job title, the user's local time and a "Message" button.
//
// Lifetime of one appearance: showFor() positions and reveals the card;
// scheduleHide() starts a short grace timer so the cursor can travel from the
// mention into the card (entering the card cancels the timer); leaving the card
// re-arms it. hideNow() dismisses immediately.
class UserProfileCard : public QWidget {
    Q_OBJECT
public:
    explicit UserProfileCard(QWidget *parent = nullptr);

    // Show the card near targetGlobalRect (the mention chip), above when there
    // is room, otherwise below. avatar may be null — an initial placeholder is
    // painted until updateAvatar() delivers the real pixmap.
    void showFor(const User &user, const QPixmap &avatar, const QRect &targetGlobalRect);

    void updateAvatar(const QPixmap &avatar);
    void setActive(bool active); // live presence update while visible

    void scheduleHide(); // hide after a short grace period unless cursor enters the card
    void cancelHide();
    void hideNow();

    UserId         userId() const { return _user.id; }
    const QString &avatarUrl() const { return _user.avatarUrl; }

signals:
    // "Message" button clicked — caller opens/navigates to the DM.
    void messageRequested(UserId user);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    // Role line shown in the header strip; empty → strip is omitted.
    QString roleLabel() const;
    QString localTimeText() const; // "6:29 PM local time", empty when tz unknown
    bool    showMessageButton() const { return !_user.isDeactivated; }

    // Content rows recomputed by relayout(): heights depend on which optional
    // rows (role header, status, title, clock) the user actually has.
    void  relayout();
    QRect cardRect() const; // card body inside the shadow padding
    QRect messageButtonRect() const;

    User    _user;
    QPixmap _avatar;

    // Vertical metrics computed by relayout() (card-local coordinates)
    int _headerH = 0; // role strip height, 0 when absent
    int _statusH = 0; // status emoji+text line, 0 when absent
    int _titleH  = 0; // job-title line, 0 when absent
    int _clockH  = 0; // local-time row, 0 when absent
    int _bodyH   = 0; // avatar + name block height (including its padding)
    int _cardH   = 0; // full card height (excluding shadow)

    bool   _btnHovered = false;
    QTimer _hideTimer;  // grace period after the cursor leaves mention/card
    QTimer _clockTimer; // refreshes the local-time row while visible

    static constexpr int kCardW     = 320; // card body width
    static constexpr int kShadow    = 8;   // transparent margin for the drop shadow
    static constexpr int kRadius    = 8;   // card corner radius
    static constexpr int kPad       = 16;  // inner padding
    static constexpr int kAvSize    = 72;  // avatar square
    static constexpr int kAvRadius  = 8;   // avatar corner radius
    static constexpr int kAvGap     = 14;  // gap between avatar and text column
    static constexpr int kBtnH      = 36;  // "Message" button height
    static constexpr int kGap       = 6;   // gap between target rect and card
    static constexpr int kHideDelay = 260; // ms grace period before hiding
};
