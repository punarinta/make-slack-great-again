// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

#include <QList>
#include <QString>
#include <utility>
#include <vector>

class QLabel;
class StyledButton;

// Label/value rows about a conversation (Backend::runLocalCommand) — an
// agent session's /status: version, model, account, folder. Values can be
// selected and copied.
class SessionStatusDialog : public AppDialog {
    Q_OBJECT
public:
    explicit SessionStatusDialog(
        const std::vector<std::pair<QString, QString>> &rows, QWidget *parent = nullptr
    );

protected:
    void applyTheme() override;
    // Wider than most dialogs: ids and paths read best on one line.
    int  cardWidth(int availOverlayWidth) const override;

private:
    QList<QLabel *> _labels;
    QList<QLabel *> _values;
    StyledButton   *_closeBtn = nullptr;
};
