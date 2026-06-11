// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QPixmap>
#include <QWidget>

class QLabel;
class QToolButton;

// Full-window in-app viewer for inline file previews (images and prerendered
// document pages such as PDF first pages). Mounted once per top-level window
// and reused via open(). Dark near-opaque backdrop, image centred and scaled
// to fit (never upscaled past 1:1), filename top-left, action buttons
// top-right: download / forward / open-in-browser / more / close. Esc or a
// backdrop click dismisses it.
//
// The viewer holds no Session — the owner supplies the initial pixmap with
// open() and may later swap in a higher-resolution one via updatePixmap().
// Download / forward / more are forwarded to the owner as signals so they
// share the exact code paths of the hover file action bar.
class ImageViewerOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ImageViewerOverlay(QWidget *windowParent);

    // Show the viewer. pixmap may be null — a loading placeholder is painted
    // until updatePixmap() delivers one. Action buttons are hidden for
    // pending (not-yet-sent) messages, mirroring the file action bar.
    void open(const File &file, const Message &msg, const QPixmap &pixmap);

    // Swap in a (higher-resolution) pixmap. Ignored when the viewer is closed
    // or has been reopened for a different file since the request started.
    void updatePixmap(const QString &fileId, const QPixmap &pixmap);

signals:
    void downloadRequested(File file);
    void forwardRequested(Message msg);
    void moreRequested(File file, Message msg, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void  applyTheme();
    QRect imageRect() const; // displayed image rect in widget coords; empty if no pixmap

    static constexpr int kBarH   = 56; // top action bar height
    static constexpr int kMargin = 24; // breathing room around the image

    File    _file;
    Message _msg;
    QPixmap _pixmap;

    QWidget     *_bar         = nullptr;
    QLabel      *_nameLabel   = nullptr;
    QToolButton *_downloadBtn = nullptr;
    QToolButton *_forwardBtn  = nullptr;
    QToolButton *_browserBtn  = nullptr;
    QToolButton *_moreBtn     = nullptr;
    QToolButton *_closeBtn    = nullptr;
};
