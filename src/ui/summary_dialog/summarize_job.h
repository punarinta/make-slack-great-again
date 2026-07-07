// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QObject>
#include <QPointer>

#include <map>
#include <vector>

class Session;

// One-shot pipeline behind the "Summarize down" message action: fetches the
// replies of every thread rooted in the selected span (chat mode only — a
// thread view already holds its replies), builds the transcript, asks the LLM
// service for the report, and shows it in a SummaryDialog over `hostWindow`.
//
// The whole wait is surfaced through BackgroundTasks (spinner + hover
// description) — nothing blocks, and the result appears only when ready.
// Parented to hostWindow so it survives the message list navigating away;
// deletes itself once the dialog (or the failure notice) is up.
class SummarizeJob : public QObject {
    Q_OBJECT
public:
    // span: the messages to summarize, in chronological order (system lines and
    // pending ghosts already filtered out). includeThreads: fetch and inline
    // the replies of thread roots found in the span.
    static void start(
        Session             *session,
        ConversationId       conv,
        std::vector<Message> span,
        bool                 includeThreads,
        QWidget             *hostWindow
    );

    ~SummarizeJob() override;

private:
    SummarizeJob(
        Session             *session,
        ConversationId       conv,
        std::vector<Message> span,
        bool                 includeThreads,
        QWidget             *hostWindow
    );

    void fetchNextThread();
    void runLlm();
    void finish(const QString &markdown, bool ok);

    // Author label for a transcript line (bot name / display name / raw id).
    QString authorLabel(const Message &m) const;
    // Message text with @mention entities resolved to display names, and a
    // placeholder for file-only messages.
    QString transcriptText(const Message &m) const;

    Session                           *_session;
    ConversationId                     _conv;
    std::vector<Message>               _span;
    std::vector<Ts>                    _roots; // thread roots still to fetch (chat mode)
    size_t                             _nextRoot       = 0;
    int                                _omittedThreads = 0; // roots beyond kMaxThreads
    std::map<Ts, std::vector<Message>> _replies;            // fetched thread replies by root ts
    int                                _task = 0;           // BackgroundTasks token
    QPointer<QWidget>                  _window;
    rpl::lifetime                      _lifetime;
};
