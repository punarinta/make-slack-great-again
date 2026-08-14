// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QObject>

#include <optional>
#include <vector>

class Session;

// One-shot pipeline behind the thread panel's download button: pages through
// the WHOLE thread via Backend::loadThread — the open view may only hold the
// first pages of a long thread, so the export re-fetches from the API until
// the cursor runs dry — then writes a plain-text transcript to `savePath`.
//
// The wait is surfaced through BackgroundTasks (spinner + hover description);
// nothing blocks. Parented to hostWindow so it survives the panel switching
// threads or closing; deletes itself once the file is written (or the fetch
// failed — a failed page aborts the export rather than writing a silently
// truncated transcript).
class ThreadExportJob : public QObject {
    Q_OBJECT
public:
    // convTitle: display label of the hosting conversation ("#general" / a DM
    // peer's name), baked into the transcript header.
    static void start(
        Session       *session,
        ConversationId conv,
        Ts             root,
        const QString &convTitle,
        const QString &savePath,
        QWidget       *hostWindow
    );

    ~ThreadExportJob() override;

private:
    ThreadExportJob(
        Session       *session,
        ConversationId conv,
        Ts             root,
        QString        convTitle,
        QString        savePath,
        QWidget       *hostWindow
    );

    void fetchNextPage();
    void writeFile();
    void fail(const QString &why);

    // Author label for a transcript line (bot name / display name / raw id).
    QString authorLabel(const Message &m) const;
    // Message text with @mention entities resolved to display names.
    QString transcriptText(const Message &m) const;

    Session               *_session;
    ConversationId         _conv;
    Ts                     _root;
    QString                _convTitle;
    QString                _savePath;
    std::vector<Message>   _messages;            // accumulated pages, oldest-first
    std::optional<QString> _nextCursor;          // set by the last page; empty = done
    bool                   _pageArrived = false; // did the in-flight fetch emit?
    int                    _pages       = 0;
    int                    _task        = 0; // BackgroundTasks token
    rpl::lifetime          _lifetime;
};
