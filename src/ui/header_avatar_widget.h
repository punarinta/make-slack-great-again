// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPainter>
#include <QPen>
#include <QPainterPath>

// Displays a user avatar (28×28 rounded rect) with a presence indicator dot.
// No Q_OBJECT — state is updated externally via setPixmap/setPresence/clearAvatar.
class HeaderAvatarWidget : public QWidget {
public:
    explicit HeaderAvatarWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(36, 36);
    }
    void setPixmap(const QPixmap &px) { _pixmap = px; update(); }
    void setPresence(bool active) { _isActive = active; update(); }
    void clearAvatar() { _pixmap = {}; _isActive = false; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect av(0, 0, 28, 28);
        if (!_pixmap.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(QRectF(av), 4, 4);
            p.setClipPath(clip);
            const qreal dpr = devicePixelRatioF();
            QPixmap scaled = _pixmap.scaled(av.size() * dpr,
                Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(dpr);
            p.drawPixmap(av, scaled);
            p.setClipping(false);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#E8E8E8"));
            p.drawRoundedRect(av, 4, 4);
        }
        // Presence dot in bottom-right corner
        const int dotD = 10;
        const QRect dot(av.right() - dotD + 3, av.bottom() - dotD + 3, dotD, dotD);
        p.setPen(QPen(Qt::white, 2));
        p.setBrush(_isActive ? QColor("#2BAC76") : QColor("#B8B8B8"));
        p.drawEllipse(dot);
    }

private:
    QPixmap _pixmap;
    bool    _isActive = false;
};
