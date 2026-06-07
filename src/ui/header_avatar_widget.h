// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "user_avatar.h"

#include <QWidget>
#include <QPixmap>
#include <QPainter>

// Displays a user avatar (28×28 rounded rect) with a presence/DND indicator dot.
// No Q_OBJECT — state is updated externally via setPixmap/setPresence/setDnd/clearAvatar.
class HeaderAvatarWidget : public QWidget {
public:
    explicit HeaderAvatarWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(36, 36);
    }
    void setPixmap(const QPixmap &px) {
        _pixmap = px;
        update();
    }
    void setPresence(bool active) {
        _state.isActive = active;
        update();
    }
    void setDnd(bool dnd) {
        _state.dndEnabled = dnd;
        update();
    }
    void clearAvatar() {
        _pixmap = {};
        _state  = {};
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter      p(this);
        const QString initial = _displayName.isEmpty() ? QString{} : _displayName.left(1);
        UserAvatar::paint(
            p,
            QRect(0, 0, 28, 28),
            _pixmap,
            initial,
            _state,
            /*cornerRadius=*/4,
            devicePixelRatioF(),
            /*borderColor=*/Qt::white
        );
    }

public:
    // Optional: set display name so the placeholder initial letter is correct.
    void setDisplayName(const QString &name) {
        _displayName = name;
        update();
    }

private:
    QPixmap           _pixmap;
    UserAvatar::State _state;
    QString           _displayName;
};
