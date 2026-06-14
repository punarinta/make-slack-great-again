// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class PopupTooltip;

// Thin bar shown above the message list when a Slack huddle is live in the open
// conversation. Hidden by default; MainWindow toggles it from the conversation's
// `huddleActive` flag. The Join button hands off to the official Slack web client
// (huddles aren't joinable through the public API), so no desktop install is
// required — see docs/HUDDLES_PLAN.md.
class HuddleBanner : public QWidget {
    Q_OBJECT
public:
    explicit HuddleBanner(QWidget *parent = nullptr);

signals:
    void joinClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    // Shows the "joins via Slack web" tooltip while hovering the Join button.
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void applyTheme();

    QLabel       *_icon        = nullptr;
    QLabel       *_label       = nullptr;
    QPushButton  *_btn         = nullptr;
    PopupTooltip *_joinTooltip = nullptr;
};
