// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

#include <QString>
#include <QVector>

class Session;
class ImageCache;
class EmojiPickerPopup;
class Dropdown;
class QFrame;
class QLineEdit;
class QPushButton;
class QToolButton;
class StyledButton;

// "Set a status" dialog reached from the conversation-list footer avatar menu.
// Mirrors Slack's official status sheet: an emoji + text input, a list of preset
// suggestions for the workspace, an expiry ("Clear after") selector and Save /
// Cancel. Backed by the documented users.profile.set API
// (status_text / status_emoji / status_expiration) via Session::setStatus.
//
// The calendar "Automatically updates" suggestions Slack shows need a Google /
// Outlook integration we don't have, and the "Edit suggestions" link manages
// workspace-admin presets — both are intentionally omitted.
class StatusDialog : public AppDialog {
    Q_OBJECT
public:
    StatusDialog(
        Session       *session,
        ImageCache    *imgCache,
        const QString &workspaceName,
        QWidget       *parent = nullptr
    );

protected:
    void applyTheme() override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    // A built-in preset (emoji + text + default expiry).
    struct Preset {
        QString emoji;      // bare name, e.g. "calendar"
        QString text;       // status text
        int     clearAfter; // index into the "Clear after" combo
    };

    void buildInput();
    void buildPresets(const QString &workspaceName);
    void buildClearAfter();
    void buildButtons();

    void openEmojiPicker();
    void setEmoji(const QString &bareName); // empty → default smiley placeholder
    void applyPreset(const Preset &p);
    void updateEmojiButton();
    void styleInputBox(bool focused);
    void save();

    // Convert the selected "Clear after" combo index to an absolute Unix expiry
    // timestamp (seconds), or 0 for "Don't clear".
    qint64 expirationTs() const;

    Session    *_session  = nullptr;
    ImageCache *_imgCache = nullptr;

    EmojiPickerPopup *_emojiPicker = nullptr;

    QFrame       *_inputBox   = nullptr;
    QToolButton  *_emojiBtn   = nullptr;
    QLineEdit    *_textEdit   = nullptr;
    Dropdown     *_clearAfter = nullptr;
    QPushButton  *_clearBtn   = nullptr; // shown only when a status is already set
    StyledButton *_cancelBtn  = nullptr;
    StyledButton *_saveBtn    = nullptr;

    QString _emoji; // currently chosen emoji bare name ("" = none)
};
