// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

#include <QPixmap>
#include <QString>
#include <QWidget>

class Session;
class ImageCache;
class StyledLineEdit;
class PopupTooltip;
class QLabel;
class QPushButton;

// Round avatar that reveals a "change photo" camera overlay on hover and emits
// clicked() when pressed. Used as the editable avatar at the top of the
// profile dialog.
class ProfileAvatarWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProfileAvatarWidget(int diameter, QWidget *parent = nullptr);

    void setAvatar(const QPixmap &pixmap, const QString &initial);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    int           _diameter;
    QPixmap       _avatar;
    QString       _initial;
    bool          _hover   = false;
    PopupTooltip *_tooltip = nullptr;
};

// "Manage profile" dialog reached from the conversation-list footer avatar.
// Shows the authed user's avatar (with a hover-to-upload overlay) plus editable
// Name / Email / Phone fields, backed by the documented Slack profile APIs
// (users.profile.get/.set, users.setPhoto).
class ProfileDialog : public AppDialog {
    Q_OBJECT
public:
    ProfileDialog(Session *session, ImageCache *imgCache, QWidget *parent = nullptr);

protected:
    void applyTheme() override;

private:
    void loadProfile();
    void setAvatarUrl(const QString &url);
    void pickAndUploadPhoto();
    void save();
    void setStatusMessage(const QString &text, bool error);

    Session    *_session  = nullptr;
    ImageCache *_imgCache = nullptr;

    ProfileAvatarWidget *_avatar = nullptr;
    QString              _avatarUrl;
    QString              _initial;

    QLabel         *_nameLabel  = nullptr;
    QLabel         *_emailLabel = nullptr;
    QLabel         *_phoneLabel = nullptr;
    StyledLineEdit *_nameEdit   = nullptr;
    StyledLineEdit *_emailEdit  = nullptr;
    StyledLineEdit *_phoneEdit  = nullptr;
    QLabel         *_status     = nullptr;
    QPushButton    *_cancelBtn  = nullptr;
    QPushButton    *_saveBtn    = nullptr;

    // Values as loaded from the server — used to send only changed fields
    // (notably: Slack rejects an unchanged-but-present email for self-edits).
    QString _loadedDisplayName;
    QString _loadedEmail;
    QString _loadedPhone;
    bool    _loaded = false;
};
