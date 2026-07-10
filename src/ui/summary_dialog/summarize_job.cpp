// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/summary_dialog/summarize_job.h"

#include "backend/backend.h"
#include "llm/discussion_summary.h"
#include "llm/llm_provider.h"
#include "llm/llm_service.h"
#include "session/session.h"
#include "ui/summary_dialog/summary_dialog.h"
#include "util/background_tasks.h"

#include <QTimer>

namespace {
// Bound on thread fetches (one API call each). A span with more threads keeps
// its roots' own text and notes the omission in the transcript instead of
// firing an unbounded burst of conversations.replies calls.
constexpr int kMaxThreads      = 20;
// If a thread fetch never resolves (its producer can error without a next —
// e.g. a bounded Slack-level failure), proceed with what has arrived rather
// than leaving the background task spinning forever.
constexpr int kFetchDeadlineMs = 15000;
} // namespace

void SummarizeJob::start(
    Session             *session,
    ConversationId       conv,
    std::vector<Message> span,
    bool                 includeThreads,
    QWidget             *hostWindow
) {
    if (!session || span.empty())
        return;
    auto *job =
        new SummarizeJob(session, std::move(conv), std::move(span), includeThreads, hostWindow);
    job->fetchNextThread();
}

SummarizeJob::SummarizeJob(
    Session             *session,
    ConversationId       conv,
    std::vector<Message> span,
    bool                 includeThreads,
    QWidget             *hostWindow
)
    : QObject(hostWindow), _session(session), _conv(std::move(conv)), _span(std::move(span)),
      _window(hostWindow) {
    if (includeThreads) {
        for (const Message &m : _span)
            if (m.replyCount > 0)
                _roots.push_back(m.threadRoot.value_or(m.ts));
        if ((int)_roots.size() > kMaxThreads) {
            _omittedThreads = (int)_roots.size() - kMaxThreads;
            _roots.resize(kMaxThreads);
        }
    }
    _task = BackgroundTasks::instance().begin(tr("Summarizing discussion…"));
    QTimer::singleShot(kFetchDeadlineMs, this, [this] {
        if (_nextRoot < _roots.size()) { // still fetching — give up on the rest
            _lifetime.destroy();
            _nextRoot = _roots.size();
            runLlm();
        }
    });
}

SummarizeJob::~SummarizeJob() {
    // Normally ended in finish(); this covers the host window tearing the job
    // down mid-flight so the spinner doesn't survive its task.
    BackgroundTasks::instance().end(_task);
}

void SummarizeJob::fetchNextThread() {
    if (_nextRoot >= _roots.size()) {
        runLlm();
        return;
    }
    const Ts root = _roots[_nextRoot];
    _session->backend()->loadThread(_conv, root, std::nullopt) |
        rpl::on_next(
            [this, root](MessagePage page) {
                auto &replies = _replies[root];
                replies.clear();
                for (const Message &m : page.messages) {
                    if (m.ts == root || m.pending || isSystemEvent(m))
                        continue;
                    replies.push_back(m);
                }
                ++_nextRoot;
                fetchNextThread();
            },
            _lifetime
        );
}

void SummarizeJob::runLlm() {
    std::vector<DiscussionSummary::Entry> entries;
    for (const Message &m : _span) {
        const QString text = transcriptText(m);
        if (text.isEmpty())
            continue;
        entries.push_back({authorLabel(m), text, false});
        const auto it = _replies.find(m.threadRoot.value_or(m.ts));
        if (it == _replies.end())
            continue;
        for (const Message &r : it->second) {
            const QString rText = transcriptText(r);
            if (!rText.isEmpty())
                entries.push_back({authorLabel(r), rText, true});
        }
    }
    if (_omittedThreads > 0)
        entries.push_back(
            {{},
             QStringLiteral("[replies of %1 more threads not included]").arg(_omittedThreads),
             false}
        );
    if (entries.empty()) {
        finish(tr("Nothing to summarize — no text messages in the selected range."), false);
        return;
    }

    auto &llm = LlmService::instance();
    auto  req = DiscussionSummary::buildRequest(entries, llm.nativeLanguage());
    if (const auto *p = llm.activeProvider())
        req.model = DiscussionSummary::modelForProvider(p->id());

    // The provider outlives the job; guard the callbacks so a torn-down job
    // (host window closing) doesn't get poked.
    QPointer<SummarizeJob> self(this);
    llm.chat(
        req,
        [self](Llm::Response resp) {
            if (self)
                self->finish(resp.text.trimmed(), true);
        },
        [self](QString err) {
            if (self)
                self->finish(SummarizeJob::tr("Couldn't summarize: %1").arg(err), false);
        }
    );
}

void SummarizeJob::finish(const QString &markdown, bool ok) {
    BackgroundTasks::instance().end(_task);
    _task = 0;
    if (_window) {
        auto *dlg = new SummaryDialog(
            markdown, ok ? SummaryDialog::Kind::Report : SummaryDialog::Kind::Failure, _window
        );
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    }
    deleteLater();
}

QString SummarizeJob::authorLabel(const Message &m) const {
    if (!m.botName.isEmpty())
        return m.botName;
    return _session->userDisplayName(m.author);
}

QString SummarizeJob::transcriptText(const Message &m) const {
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
    if (text.trimmed().isEmpty() && !m.files.empty()) {
        QStringList names;
        for (const File &f : m.files)
            if (!f.name.isEmpty())
                names << f.name;
        return names.isEmpty() ? tr("[shared a file]")
                               : tr("[shared a file: %1]").arg(names.join(QStringLiteral(", ")));
    }
    return text.trimmed();
}
