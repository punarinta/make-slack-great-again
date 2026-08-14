// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/thread_panel/thread_export_job.h"

#include "backend/backend.h"
#include "session/session.h"
#include "util/background_tasks.h"
#include "util/time_format.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSet>
#include <QTextStream>
#include <QWidget>

#include <algorithm>

namespace {
// Backstop against a cursor loop (a server bug repeating the same next_cursor
// forever). 400 pages × 50 messages is far beyond any real thread.
constexpr int kMaxPages = 400;
} // namespace

void ThreadExportJob::start(
    Session       *session,
    ConversationId conv,
    Ts             root,
    const QString &convTitle,
    const QString &savePath,
    QWidget       *hostWindow
) {
    if (!session || conv.value.isEmpty() || root.isEmpty() || savePath.isEmpty())
        return;
    auto *job = new ThreadExportJob(
        session, std::move(conv), std::move(root), convTitle, savePath, hostWindow
    );
    job->fetchNextPage();
}

ThreadExportJob::ThreadExportJob(
    Session       *session,
    ConversationId conv,
    Ts             root,
    QString        convTitle,
    QString        savePath,
    QWidget       *hostWindow
)
    : QObject(hostWindow), _session(session), _conv(std::move(conv)), _root(std::move(root)),
      _convTitle(std::move(convTitle)), _savePath(std::move(savePath)) {
    _task = BackgroundTasks::instance().begin(tr("Downloading thread…"));
}

ThreadExportJob::~ThreadExportJob() {
    // Normally ended in writeFile()/fail(); this covers the host window tearing
    // the job down mid-flight so the spinner doesn't survive its task.
    BackgroundTasks::instance().end(_task);
}

void ThreadExportJob::fetchNextPage() {
    if (++_pages > kMaxPages) {
        fail(QStringLiteral("page limit reached — cursor loop?"));
        return;
    }
    const std::optional<QString> cursor = _nextCursor;
    _nextCursor.reset();
    _pageArrived = false;
    _session->backend()->loadThread(_conv, _root, cursor) |
        rpl::on_next_done(
            [this](MessagePage page) {
                _pageArrived = true;
                _nextCursor  = page.olderCursor;
                for (Message &m : page.messages)
                    if (!m.pending)
                        _messages.push_back(std::move(m));
            },
            [this] {
                // The producer completes on both outcomes; emitting no page is
                // the error signal (see PublicBackend::loadThread). Abort then,
                // rather than write a silently truncated transcript.
                if (!_pageArrived)
                    fail(QStringLiteral("thread page fetch failed"));
                else if (_nextCursor)
                    fetchNextPage();
                else
                    writeFile();
            },
            _lifetime
        );
}

void ThreadExportJob::writeFile() {
    // Pages arrive oldest-first, but dedup (Slack repeats the root message on
    // every page) and re-sort by wall clock so the transcript is bulletproof
    // against pagination quirks.
    QSet<QString>        seen;
    std::vector<Message> messages;
    messages.reserve(_messages.size());
    for (Message &m : _messages) {
        if (seen.contains(m.ts))
            continue;
        seen.insert(m.ts);
        messages.push_back(std::move(m));
    }
    std::stable_sort(messages.begin(), messages.end(), [](const Message &a, const Message &b) {
        return a.date < b.date;
    });

    QFile f(_savePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("cannot open %1 for writing").arg(_savePath));
        return;
    }
    QTextStream out(&f);

    if (!_convTitle.isEmpty())
        out << tr("Thread in %1").arg(_convTitle) << "\n";
    out << tr("Messages: %1").arg(messages.size()) << "\n\n";

    for (const Message &m : messages) {
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(m.date / 1000);
        out << authorLabel(m) << " — " << TimeFmt::formatDate(dt.date()) << " "
            << TimeFmt::formatTime(dt.toSecsSinceEpoch());
        if (m.edited)
            out << " " << tr("(edited)");
        out << "\n";
        const QString text = transcriptText(m);
        if (!text.isEmpty())
            out << text << "\n";
        for (const File &file : m.files)
            out << tr("[file: %1]").arg(file.name.isEmpty() ? tr("untitled") : file.name) << "\n";
        out << "\n";
    }
    f.close();

    BackgroundTasks::instance().end(_task);
    _task = 0;
    deleteLater();
}

void ThreadExportJob::fail(const QString &why) {
    qWarning() << "Thread export failed:" << why;
    BackgroundTasks::instance().end(_task);
    _task = 0;
    deleteLater();
}

QString ThreadExportJob::authorLabel(const Message &m) const {
    if (!m.botName.isEmpty())
        return m.botName;
    return _session->userDisplayName(m.author);
}

QString ThreadExportJob::transcriptText(const Message &m) const {
    QString text = m.text.text;
    // Resolve @mention entities to names, back to front so offsets stay valid.
    for (auto it = m.text.entities.rbegin(); it != m.text.entities.rend(); ++it) {
        if (it->type != EntityType::UserMention)
            continue;
        text.replace(
            it->offset,
            it->length,
            QStringLiteral("@") + _session->userDisplayName(UserId{it->data})
        );
    }
    return text.trimmed();
}
