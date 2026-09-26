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
// is actively typing).  An agent working on a turn (Claude Code) is "thinking"
// instead, with the turn's elapsed time: "<b>Engineer</b> is thinking (8m 58s)…",
// ticking every second.  Hidden — and zero-height in the layout — when nobody is
// typing.
class TypingIndicatorWidget : public QWidget {
    Q_OBJECT
public:
    explicit TypingIndicatorWidget(QWidget *parent = nullptr);

    // Record/refresh that `id` (shown as `name`) is typing in the current conv.
    // `isSelf` marks the authed user typing from another client — rendered as a
    // distinct "You're typing on another device…" cue rather than by name.
    // `thinkingSinceMs` (epoch ms, 0 = none) marks an agent thinking since then.
    void userTyping(
        const UserId &id, const QString &name, bool isSelf = false, qint64 thinkingSinceMs = 0
    );
    // Drop a user immediately (e.g. once their message arrives).
    void userStopped(const UserId &id);
    // Forget everyone (on conversation switch).
    void clearAll();

    // "35s", "8m 58s", "1h 5m" — elapsed time the way Claude Code's spinner shows it.
    static QString formatElapsed(qint64 ms);

private:
    void rebuild();
    void purge();
    void applyTheme();

    struct Entry {
        QString id;              // UserId::value
        QString name;            // resolved display label
        qint64  deadline;        // _clock ms after which the user is considered idle
        bool    isSelf;          // the authed user typing from another client
        qint64  thinkingSinceMs; // epoch ms an agent began its turn; 0 = typing
    };

    QLabel        *_label = nullptr;
    QVector<Entry> _typers;
    QElapsedTimer  _clock; // monotonic time source for deadlines
    QTimer         _purgeTimer;
};
