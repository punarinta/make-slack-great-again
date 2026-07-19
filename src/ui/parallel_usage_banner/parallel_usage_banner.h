// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

// Persistent, dismissable bar shown when Slack keeps evicting our shared Socket
// Mode connection because the same compiled-in app keys are running on another
// device (see Session::parallelUsageNotice / EvRealtimeContended). Unlike the
// transient error banner it stays until the user closes it — the condition
// lasts until the app is closed on the other device. Carries a "How to solve
// this?" link to the setup docs explaining per-device app keys.
class ParallelUsageBanner : public QWidget {
    Q_OBJECT
public:
    explicit ParallelUsageBanner(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void applyTheme();

    QLabel      *_label    = nullptr;
    QPushButton *_closeBtn = nullptr;
};
