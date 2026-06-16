// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QObject>
#include <QSet>

// App-wide registry of long-running background tasks (e.g. fetching a full-res
// image to put on the clipboard). UI surfaces a spinner while count() > 0.
//
// Usage: token-based so a task is uniquely tracked even if several run at once.
//   const int t = BackgroundTasks::instance().begin();
//   ... async work ...
//   BackgroundTasks::instance().end(t);   // also call on the failure path
//
// Everything runs on the GUI thread (begin/end are invoked from Qt callbacks),
// so no locking is needed.
class BackgroundTasks : public QObject {
    Q_OBJECT
public:
    static BackgroundTasks &instance();

    // Register a running task; returns an opaque id to pass to end().
    int  begin();
    // Mark a task finished. Safe to call with an unknown/already-ended id.
    void end(int id);

    int count() const { return static_cast<int>(_active.size()); }

signals:
    void countChanged(int count);

private:
    using QObject::QObject;

    QSet<int> _active;
    int       _nextId = 1;
};
