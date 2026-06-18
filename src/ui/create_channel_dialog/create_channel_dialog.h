// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

class QLabel;
class QRadioButton;
class QStackedWidget;
class StyledButton;
class StyledLineEdit;

// Two-step modal for creating a Slack channel.
//   Step 1: enter channel name (max 80 chars; Next enabled only when non-empty)
//   Step 2: choose Public / Private visibility; confirm with Create
//
// After exec() == Accepted: read channelName() and isPrivate().
class CreateChannelDialog : public AppDialog {
    Q_OBJECT
public:
    explicit CreateChannelDialog(const QString &workspaceName, QWidget *parent = nullptr);

    // Normalized channel name: trimmed, lowercased, spaces → hyphens.
    QString channelName() const;
    bool    isPrivate() const;

protected:
    void applyTheme() override;

private:
    void goNext();
    void goBack();
    void updateNextEnabled();

    QStackedWidget *_pages = nullptr;

    // Step 1 widgets
    QLabel         *_nameSectionLabel = nullptr;
    StyledLineEdit *_nameEdit         = nullptr;
    QLabel         *_helperLabel      = nullptr;
    StyledButton   *_nextBtn          = nullptr;

    // Step 2 widgets
    QLabel       *_channelSubtitle = nullptr;
    QLabel       *_visSectionLabel = nullptr;
    QRadioButton *_publicRadio     = nullptr;
    QRadioButton *_privateRadio    = nullptr;
    QLabel       *_privateDesc     = nullptr;
    QLabel       *_stepLabel       = nullptr;
    StyledButton *_backBtn         = nullptr;
    StyledButton *_createBtn       = nullptr;

    QString _workspaceName;
};
