// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QLabel;

// Thin strip shown directly above the composer: "<b>Alice</b>, <b>Bob</b> are
// typing…".  Names are bold; the line is small and gray.  Each typing event
// refreshes a per-user deadline; a user drops off when no new event arrives
// within kExpiryMs (Slack re-sends user_typing every few seconds while a person
// is actively typing).  Hidden — and zero-height in the layout — when nobody is
// typing.
class TypingIndicatorWidget : public QWidget {
    Q_OBJECT
public:
    explicit TypingIndicatorWidget(QWidget *parent = nullptr);

    // Record/refresh that `id` (shown as `name`) is typing in the current conv.
    // `isSelf` marks the authed user typing from another client — rendered as a
    // distinct "You're typing on another device…" cue rather than by name.
    void userTyping(const UserId &id, const QString &name, bool isSelf = false);
    // Drop a user immediately (e.g. once their message arrives).
    void userStopped(const UserId &id);
    // Forget everyone (on conversation switch).
    void clearAll();

private:
    void rebuild();
    void purge();
    void applyTheme();

    struct Entry {
        QString id;       // UserId::value
        QString name;     // resolved display label
        qint64  deadline; // _clock ms after which the user is considered idle
        bool    isSelf;   // the authed user typing from another client
    };

    QLabel        *_label = nullptr;
    QVector<Entry> _typers;
    QElapsedTimer  _clock; // monotonic time source for deadlines
    QTimer         _purgeTimer;
};
