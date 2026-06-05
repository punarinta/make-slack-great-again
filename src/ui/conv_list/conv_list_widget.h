// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QPixmap>
#include <QVariantAnimation>
#include <vector>

class QNetworkAccessManager;

// Per-user info cached from setUsers().
struct UserInfo {
    QString displayName;
    QString avatarUrl;
    bool    isDeactivated = false;
};

// Virtual-painted conversation list with animated hover and selection.
// Zero QWidgets per row — scales to thousands of conversations.
class ConvListWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ConvListWidget(QWidget *parent = nullptr);

    void setConversations(std::vector<Conversation> convs);
    // Call with the full user list so DM names and avatars can be resolved.
    void setUsers(const std::vector<User> &users);
    // Resolved display name for a row (DMs → user displayName, channels → conv.name).
    QString resolvedName(int row) const;
    int  selectedIndex() const { return _selected; }
    // Resolved ConversationId for a filtered-list row (-1 safe: returns empty id).
    ConversationId conversationId(int row) const;
    // Row in the filtered list for a given id; -1 if not found or filtered out.
    int rowForId(ConversationId id) const;
    // Programmatically select a row; emits conversationSelected.
    void selectRow(int row);

signals:
    void conversationSelected(int index);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void doPaint(QPaintEvent *e);
    void doMouseMove(QMouseEvent *e);
    void doMousePress(QMouseEvent *e);
    void doMouseRelease(QMouseEvent *e);
    void doLeave(QEvent *e);
    bool isOnScrollThumb(int vpY) const;

    int  rowAt(int viewportY) const;    // -1 if none
    void setHovered(int row);
    void setSelected(int row);          // emits conversationSelected
    void paintRow(QPainter &p, int i, int y) const;
    void updateScrollRange();
    // Rebuild _convs from _allConvs, filtering deactivated / raw-ID DM users.
    void rebuildFilteredConvs();

    // Avatar helpers — trigger is non-const (starts downloads), draw is const.
    void triggerMissingAvatarDownloads();
    void drawUserAvatar(QPainter &p, QRect rect, const QString &userId) const;

    std::vector<Conversation>        _allConvs; // unfiltered; source of truth
    std::vector<Conversation>        _convs;
    // userId → {displayName, avatarUrl}, rebuilt on setUsers().
    QHash<QString, UserInfo>         _userInfos;
    // avatarUrl → scaled pixmap; empty QPixmap = in-flight sentinel.
    mutable QHash<QString, QPixmap>  _avatarCache;
    QNetworkAccessManager           *_nam = nullptr;

    int  _hovered  = -1;
    int  _selected = -1;

    bool _sbDragging        = false;
    int  _sbDragStartY      = 0;
    int  _sbDragStartScroll = 0;

    // Selection slide animation: 0.0 = start of slide, 1.0 = settled
    QVariantAnimation _selAnim;
    int  _selFrom = -1;
    double _selT  = 1.0;

    static constexpr int kRowH        = 40;  // height of each row in logical px
    static constexpr int kPadH        = 12;  // horizontal left padding
    static constexpr int kPadV        =  8;  // vertical padding inside row
    static constexpr int kAvatarSize  = 28;  // size of user avatar square
    static constexpr int kAvatarRadius=  5;  // corner radius
    static constexpr int kAvatarGap   =  8;  // gap between avatar and name
    static constexpr int kScrollW     =  4;  // scrollbar thumb width
};
