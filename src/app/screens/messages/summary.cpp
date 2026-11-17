#include "app/screens/messages/summary.h"

#include "app/llm/discussion_summary.h"
#include "app/llm/service.h"
#include "app/model/jobs.h"
#include "app/mrkdwn/markdown.h"
#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "app/screens/messages/rich.h"
#include "base/i18n.h"
#include "base/str.h"
#include "ui/controls.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace screens {

using i18n::arg;
using i18n::tr;
using ui::C;

namespace {

// Bound on thread fetches (one call each): a span with more threads keeps
// its roots' own text and says so in the transcript instead.
constexpr size_t kMaxThreads      = 20;
// A thread fetch that never answers (a workspace switch drops it) must not
// leave the job spinning: go on with what has arrived.
constexpr int    kFetchDeadlineMs = 15000;
// The report card (840 wide at most, a 420-px body
// that leaves room for the card's chrome in a short window).
constexpr float  kCardW = 840, kBodyH = 420, kCardChromeH = 260;

struct SummarizeJob {
    Context                       &ctx;
    ConvRef                        conv;
    std::string                    convId; // the Store may be another workspace's by the end
    bool                           threadMode = false;
    std::vector<llm::SummaryEntry> head;   // the span's own lines, by message
    std::vector<Ts>                spanTs; // …and their ts (a root's replies follow it)
    std::vector<Ts>                roots;  // threads to fetch
    std::vector<std::vector<llm::SummaryEntry>> replies; // by roots index
    size_t                                      next    = 0;
    size_t                                      omitted = 0;
    int                                         job     = 0;
    plat::TimerId                               timer   = 0;
    bool                                        asked   = false; // the LLM has the request

    explicit SummarizeJob(Context &c) : ctx(c) {}
    ~SummarizeJob() { model::jobs().end(job); } // also when the app tears it down mid-flight

    bool sameStore() const {
        return conv < ctx.store().conversationCount() &&
               ctx.store().conversation(conv).id == convId;
    }

    std::string author(const model::Message &m) const {
        return std::string(authorName(ctx.store(), m));
    }

    // The text as read (markup gone, mentions as names); a file-only
    // message as a placeholder.
    std::string text(const model::Message &m) const {
        std::string t(str::trim(plainText(ctx.store(), m.text)));
        if (!t.empty() || m.files().empty())
            return t;
        std::string names;
        for (const model::File &f : m.files())
            if (!f.name.empty())
                names += (names.empty() ? "" : ", ") + f.name;
        return names.empty() ? std::string(tr("[shared a file]"))
                             : arg(tr("[shared a file: %1]"), names);
    }
};

using JobPtr = std::shared_ptr<SummarizeJob>;

void finish(const JobPtr &j, const std::string &markdown, bool ok) {
    model::jobs().end(std::exchange(j->job, 0));
    if (j->timer)
        j->ctx.app.cancelTimer(std::exchange(j->timer, 0));
    if (j->ctx.window)
        showSummaryDialog(
            j->ctx, *j->ctx.window, markdown, ok ? SummaryKind::Report : SummaryKind::Failure
        );
}

void runLlm(const JobPtr &j) {
    if (j->asked)
        return;
    j->asked = true;
    if (j->timer)
        j->ctx.app.cancelTimer(std::exchange(j->timer, 0));
    std::vector<llm::SummaryEntry> entries;
    for (size_t i = 0; i < j->head.size(); ++i) {
        if (j->head[i].text.empty())
            continue;
        entries.push_back(j->head[i]);
        const auto r = std::find(j->roots.begin(), j->roots.end(), j->spanTs[i]);
        if (r != j->roots.end())
            for (const llm::SummaryEntry &e : j->replies[size_t(r - j->roots.begin())])
                entries.push_back(e);
    }
    if (j->omitted > 0)
        entries.push_back(
            {{},
             "[replies of " + str::number(int64_t(j->omitted)) + " more threads not included]",
             false}
        );
    if (entries.empty()) {
        finish(
            j,
            tr("Nothing to summarize \xE2\x80\x94 no text messages in the selected range."),
            false
        );
        return;
    }
    llm::Service &svc = *j->ctx.ai;
    llm::Request  req = llm::summaryRequest(entries, svc.language());
    // Short, low-stakes and potentially frequent: the provider's light model.
    if (const llm::Provider *p = svc.active())
        req.model = p->summaryModel();
    svc.chat(std::move(req), [j](llm::ChatResult r) {
        if (r.ok)
            finish(j, std::string(str::trim(r.response.text)), true);
        else
            finish(j, arg(tr("Couldn't summarize: %1"), r.error), false);
    });
}

// One thread after the other, then the request.
void fetchNext(const JobPtr &j) {
    if (j->asked)
        return;
    if (j->next >= j->roots.size() || !j->sameStore()) {
        runLlm(j);
        return;
    }
    const Ts root = j->roots[j->next];
    j->ctx.backend().loadThread(j->conv, root, [j, root](bool, const std::string &) {
        if (j->asked || j->next >= j->roots.size() || j->roots[j->next] != root)
            return; // the deadline went on without it
        if (j->sameStore())
            if (const auto *list = j->ctx.store().replies(j->conv, root))
                for (const model::Message &m : *list) {
                    if (m.ts == root || m.pending || isSystem(m))
                        continue;
                    if (std::string t = j->text(m); !t.empty())
                        j->replies[j->next].push_back({j->author(m), std::move(t), true});
                }
        ++j->next;
        fetchNext(j);
    });
}

} // namespace

void summarizeDown(Context &ctx, ConvRef conv, std::vector<Ts> span, bool threadMode) {
    if (!ctx.window)
        return;
    // No provider: the notice at once — no job, no thread fetches.
    if (!ctx.ai || !ctx.ai->available()) {
        showSummaryDialog(
            ctx,
            *ctx.window,
            tr(
                "Summaries need an AI provider. Connect one in Settings \xE2\x86\x92 AI assistance."
            ),
            SummaryKind::NoProvider
        );
        return;
    }
    if (span.empty())
        return;
    auto j        = std::make_shared<SummarizeJob>(ctx);
    j->conv       = conv;
    j->convId     = ctx.store().conversation(conv).id;
    j->threadMode = threadMode;
    for (Ts ts : span) {
        const model::Message *m = ctx.store().findMessage(conv, ts);
        if (!m)
            continue;
        j->spanTs.push_back(ts);
        j->head.push_back({j->author(*m), j->text(*m), false});
        // In a channel the roots' threads are fetched and inlined; a thread
        // view's span is the replies already.
        if (!threadMode && m->replyCount > 0 && !m->isReply())
            j->roots.push_back(ts);
    }
    if (j->roots.size() > kMaxThreads) {
        j->omitted = j->roots.size() - kMaxThreads;
        j->roots.resize(kMaxThreads);
    }
    j->replies.resize(j->roots.size());
    j->job = model::jobs().begin(tr("Summarizing discussion\xE2\x80\xA6"));
    if (!j->roots.empty())
        j->timer = ctx.app.addTimer(kFetchDeadlineMs, false, [j] {
            j->timer = 0;
            runLlm(j); // still fetching: give up on the rest
        });
    fetchNext(j);
}

ui::Popup *
showSummaryDialog(Context &ctx, ui::Window &w, const std::string &markdown, SummaryKind kind) {
    const bool report = kind == SummaryKind::Report;
    auto       d      = std::make_unique<ui::Dialog>(
        tr("Discussion summary"),
        report ? kCardW : 0,
        report ? ui::Dialog::Scroll::Disabled : ui::Dialog::Scroll::Enabled
    );
    ui::Dialog *raw = d.get();
    if (report) {
        // The report scrolls instead of growing the card past the window.
        auto       *scroll = d->content()->add<ui::ScrollView>();
        const float hostH  = w.size().h;
        scroll->style().height(
            hostH > 0 ? std::min(kBodyH, std::max(120.f, hostH - kCardChromeH)) : kBodyH
        );
        scroll->content()->style().spacing(6);
        RichOptions o;
        o.color = C::FormText;
        buildBody(ctx, scroll->content(), mrkdwn::convertOutgoing(markdown), o, scroll);
        // Escape and the header's × close it; Copy is the one button.
        auto *copy    = d->makeButton(tr("Copy"), ui::Button::Kind::Primary);
        auto  alive   = std::make_shared<char>(0); // lives as long as the button
        copy->onClick = [&ctx, copy, alive, markdown] {
            ctx.app.platform().setClipboardText(markdown);
            flashCopied(ctx, copy, alive);
        };
        d->addButtonRow(copy, nullptr);
    } else {
        ui::styledLabel(
            d->content(), markdown, ui::pxFont(15, text::Weight::Regular, ui::color(C::FormText))
        );
        if (kind == SummaryKind::NoProvider) {
            auto *open    = d->makeButton(tr("Open settings"), ui::Button::Kind::Primary);
            open->onClick = [raw] { raw->accept(); };
            d->onAccepted = [&ctx] {
                if (ctx.openAiSettings)
                    ctx.openAiSettings();
            };
            d->addButtonRow(open, nullptr);
        }
    }
    return w.showPopup(std::move(d));
}

} // namespace screens
