// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>
#include <QStringList>

// One conversation's unsent composer input: the typed text (as mrkdwn), the
// files attached but not yet uploaded, and the email subject line. Moved out of
// the composer with ComposerWidget::takeDraft() when the user leaves a
// conversation and back in with restoreDraft() when they return, so input never
// travels between conversations. Kept in a standalone header so hosts
// (MainWindow, ThreadPanel) can hold draft maps without the widget include.
struct ComposerDraft {
    QString     text;
    QStringList files;
    QString     subject;

    bool isEmpty() const { return text.isEmpty() && files.isEmpty() && subject.isEmpty(); }
};
