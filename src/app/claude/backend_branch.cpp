// Agent branches: forks of a session that run with tools turned off, asked
// and answered on someone else's behalf (agent thread links), their turns
// followed from the branch's transcript (see backend.h).
//
// A branch is a /btw underneath — launchFork, a thread under its session —
// with two things more: the tools it goes without (Tracked::deniedTools,
// passed again on every launch, see dispatch) and someone following its
// turn (Tracked::watch, told from each refresh by watchTurn).
//
// A session forked mid-turn (Claude Code 2.1.289's own code): the copy may
// end on a tool call without its result. Claude Code mends that before the
// request goes out (ensureToolResultPairing: "[Tool use interrupted]"), so
// the branch runs; it sees that call as cut short, and nothing of the turn
// after it. Its fork point lies mid-turn too, so the session reads as moved
// on at once (agentSessionMovedOn) and the next turn branches afresh.
#include "app/claude/backend.h"

#include "app/claude/backend_internal.h"
#include "app/claude/cli.h"
#include "app/claude/outputs.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"

#include <utility>

namespace claude {

using i18n::tr;
using model::Ts;
using Phase = model::Backend::AgentPhase;
using Turn  = model::Backend::AgentTurn;

namespace {

constexpr size_t kNone = std::string::npos;

// How far back a branch's own records before its first prompt can reach
// (what Claude Code writes as a session starts): the fork point is further.
constexpr size_t kForkLookBack = 64;

std::string removedMessage() {
    return tr("The session was removed from msga.");
}

Turn failedTurn(std::string error) {
    Turn t;
    t.done  = true;
    t.error = std::move(error);
    return t;
}

} // namespace

Phase toolPhase(std::string_view name) {
    struct Kind {
        const char *name;
        Phase       phase;
    };
    static constexpr Kind kKinds[] = {
        {"Read", Phase::Reading},
        {"NotebookRead", Phase::Reading},
        {"LS", Phase::Reading},
        {"Grep", Phase::Searching},
        {"Glob", Phase::Searching},
        {"ToolSearch", Phase::Searching},
        {"WebFetch", Phase::Browsing},
        {"WebSearch", Phase::Browsing},
        {"Bash", Phase::Running},
        {"BashOutput", Phase::Running},
        {"PowerShell", Phase::Running},
        {"Monitor", Phase::Running},
        {"Agent", Phase::Delegating},
        {"Task", Phase::Delegating},
        {"SendMessage", Phase::Delegating},
    };
    for (const Kind &k : kKinds)
        if (name == k.name)
            return k.phase;
    return Phase::Working;
}

std::string phaseStatus(Phase phase) {
    switch (phase) {
    case Phase::Thinking:
        return tr("Thinking…");
    case Phase::Reading:
        return tr("Reading files…");
    case Phase::Searching:
        return tr("Searching…");
    case Phase::Browsing:
        return tr("Looking things up online…");
    case Phase::Running:
        return tr("Running a command…");
    case Phase::Delegating:
        return tr("Asking a subagent…");
    case Phase::Working:
        return tr("Working…");
    case Phase::WaitingForApproval:
        return tr("Waiting for approval…");
    }
    return {};
}

// ── Starting and continuing ─────────────────────────────────────────────────

void Backend::startAgentBranch(
    const std::string       &session,
    std::string              prompt,
    std::vector<std::string> files,
    std::vector<std::string> deniedTools,
    AgentBranchDone          started,
    AgentTurnFn              turn
) {
    const Tracked *s = find(session);
    std::string    why;
    if (!s)
        why = removedMessage();
    else if (s->info.sessionId.empty())
        why = tr("Send the session its first message before asking on the side.");
    else if (!_host->hasCli())
        why = notInstalledMessage();
    if (!why.empty()) {
        post([started = std::move(started), why] {
            if (started)
                started({}, why);
        });
        return;
    }
    uploadFiles(
        std::move(files),
        [this,
         session,
         prompt = std::move(prompt),
         opts   = ForkOptions{std::move(deniedTools), std::move(turn), std::move(started)}](
            std::vector<std::string> copies, std::string failed
        ) mutable {
            Tracked          *s = find(session);
            const std::string error =
                !failed.empty()
                    ? i18n::arg(tr("Couldn't read %1."), std::string(file::baseName(failed)))
                : !s ? removedMessage()
                     : std::string();
            if (!error.empty()) {
                if (opts.started)
                    opts.started({}, error);
                return;
            }
            startFork(*s, withAttachments(prompt, copies), {}, std::move(opts));
        }
    );
}

void Backend::continueAgentBranch(
    const std::string &branch, std::string prompt, std::vector<std::string> files, AgentTurnFn turn
) {
    uploadFiles(
        std::move(files),
        [this, branch, prompt = std::move(prompt), turn = std::move(turn)](
            std::vector<std::string> copies, std::string failed
        ) mutable {
            Tracked    *t = find(branch);
            std::string error;
            if (!failed.empty())
                error = i18n::arg(tr("Couldn't read %1."), std::string(file::baseName(failed)));
            else if (!t || t->info.sessionId.empty())
                error = removedMessage();
            else
                error = readOnlyReason(*t);
            if (!error.empty()) {
                if (turn)
                    turn(failedTurn(std::move(error)));
                return;
            }
            auto w   = std::make_unique<Tracked::TurnWatch>();
            w->fn    = std::move(turn);
            w->send  = true;
            // Its prompt is a new item, after everything read so far.
            w->from  = t->parser.items().size();
            t->watch = std::move(w);
            enqueue(*t, withAttachments(prompt, copies), 0, {});
        }
    );
}

void Backend::watchAgentBranch(const std::string &branch, AgentTurnFn turn) {
    Tracked *t = find(branch);
    if (!t) {
        post([turn = std::move(turn)] {
            if (turn)
                turn(failedTurn(removedMessage()));
        });
        return;
    }
    auto w   = std::make_unique<Tracked::TurnWatch>();
    w->fn    = std::move(turn);
    t->watch = std::move(w);
    // Told at once when no turn runs: its last answer.
    post([this, branch] {
        if (Tracked *t = find(branch); t && t->watch)
            watchTurn(*t);
    });
}

void Backend::agentSessionMovedOn(
    const std::string &session, const std::string &forkPoint, std::function<void(bool)> done
) {
    const Tracked *s    = find(session);
    std::string    path = s ? s->transcriptPath : std::string();
    if (path.empty() || forkPoint.empty()) {
        post([done = std::move(done)] {
            if (done)
                done(true);
        });
        return;
    }
    auto moved = std::make_shared<bool>(true);
    model::runInBackground(
        _app,
        [moved, path = std::move(path), forkPoint] { *moved = hasTurnAfter(path, forkPoint); },
        [alive = _alive, moved, done = std::move(done)] {
            if (*alive && done)
                done(*moved);
        }
    );
}

void Backend::setAgentBranchLabel(const std::string &branch, std::string label) {
    Tracked *t = find(branch);
    if (!t || t->branchLabel == label)
        return;
    t->branchLabel = std::move(label);
    if (t->forkAt >= 0 && size_t(t->forkAt) < t->rendered.size())
        t->rendered[size_t(t->forkAt)].rev = 0; // rendered again, with it
    if (Tracked *s = find(t->forkOf))
        sync(*s);
}

// The branch's first prompt is in its transcript: the records just before
// it are read off its end, and the newest its session has too is where it
// was forked (the session's parser knows every record it read).
void Backend::branchStarted(const std::string &branchConv, AgentBranchDone started) {
    const Tracked *f = find(branchConv);
    if (!f || f->forkAt < 0 || f->transcriptPath.empty()) {
        if (started)
            started({}, removedMessage());
        return;
    }
    auto before = std::make_shared<std::vector<std::string>>();
    model::runInBackground(
        _app,
        [before, path = f->transcriptPath, root = f->parser.items()[size_t(f->forkAt)].uuid] {
            *before = recordsBefore(path, root, kForkLookBack);
        },
        [this, alive = _alive, branchConv, before, started = std::move(started)] {
            if (!*alive)
                return;
            Tracked *f = find(branchConv);
            Tracked *s = f ? find(f->forkOf) : nullptr;
            if (!s) {
                if (f)
                    f->watch.reset();
                if (started)
                    started({}, removedMessage());
                return;
            }
            whenParsed(*s, [this, branchConv, before, started] {
                Tracked *f = find(branchConv);
                Tracked *s = f ? find(f->forkOf) : nullptr;
                if (!s) {
                    if (f)
                        f->watch.reset();
                    if (started)
                        started({}, removedMessage());
                    return;
                }
                AgentBranch b;
                b.id   = branchConv;
                b.conv = s->ref;
                b.root = f->forkRoot;
                for (const std::string &uuid : *before)
                    if (s->parser.hasRecord(uuid)) {
                        b.forkPoint = uuid;
                        break;
                    }
                if (started)
                    started(std::move(b), {});
                // Its turn is told from here on (it may have ended meanwhile).
                if (f->watch) {
                    f->watch->held = false;
                    watchTurn(*f);
                }
            });
        }
    );
}

// ── Following a turn ────────────────────────────────────────────────────────

void Backend::watchTurn(Tracked &t) {
    Tracked::TurnWatch *w = t.watch.get();
    if (!w || w->held)
        return;
    const auto &items = t.parser.items();
    if (w->from == kNone) {
        if (t.forkAt < 0)
            return; // the branch's first prompt isn't read yet
        w->from = size_t(t.forkAt);
    }
    // Following msga's own turn: over once it's settled, nothing more of it
    // waiting. Else: the turn under way, whoever started it.
    const bool waiting = t.launching || t.flying || !t.outbox.empty();
    const bool ended   = w->send ? w->sent && !t.sending && !waiting : !busy(t) && !waiting;
    size_t     prompt  = kNone; // the turn's prompt: the last one it has
    for (size_t i = items.size(); i-- > w->from;)
        if (items[i].kind == TranscriptItem::Kind::UserPrompt) {
            prompt = i;
            break;
        }
    if (!ended) {
        Phase phase = Phase::Thinking;
        if (awaitsApproval(t.info)) {
            phase = Phase::WaitingForApproval;
        } else if (prompt != kNone && prompt + 1 < items.size()) {
            const TranscriptItem &last = items.back();
            if (last.kind == TranscriptItem::Kind::Subagent)
                phase = Phase::Delegating;
            else if (last.kind == TranscriptItem::Kind::ToolGroup && !last.tools.empty())
                phase = toolPhase(last.tools.back().name);
        }
        if (int(phase) == w->phase)
            return;
        w->phase = int(phase);
        Turn update;
        update.phase  = phase;
        update.status = phaseStatus(phase);
        post([fn = w->fn, update = std::move(update)] {
            if (fn)
                fn(update);
        });
        return;
    }
    // Over: its answer — the turn's last text that isn't a remark on the way
    // to a tool call, else its last text of any kind.
    const std::unique_ptr<Tracked::TurnWatch> done   = std::move(t.watch);
    const TranscriptItem                     *answer = nullptr;
    for (size_t i = items.size(); prompt != kNone && i-- > prompt + 1;) {
        const TranscriptItem &item = items[i];
        if (item.kind != TranscriptItem::Kind::AssistantText)
            continue;
        if (item.state != TranscriptItem::State::Progress) {
            answer = &item;
            break;
        }
        if (!answer)
            answer = &item;
    }
    std::string error;
    if (prompt == kNone)
        error = tr("Claude Code didn't pick up the message.");
    else if (!answer)
        error = tr("Claude Code stopped before it answered.");
    else if (answer->loginError)
        error = notLoggedInMessage();
    if (!error.empty()) {
        post([fn = done->fn, error] {
            if (fn)
                fn(failedTurn(error));
        });
        return;
    }
    // The files it made, as the thread shows them (attachOutputs): copied
    // once, on a worker.
    OutputContext ctx;
    ctx.convId       = t.convId;
    ctx.messageKey   = outputKey(*answer, {});
    ctx.cwd          = t.info.cwd;
    ctx.date         = answer->ts;
    ctx.turnStart    = items[prompt].ts;
    auto result      = std::make_shared<Turn>();
    result->done     = true;
    result->phase    = Phase(std::max(done->phase, 0));
    result->answer   = answer->text;
    result->answerId = ctx.messageKey;
    model::runInBackground(
        _app,
        [result, ctx, local = _host->isLocal()] {
            // Another machine's files aren't here to attach.
            if (local && !cachedOutputs(ctx, &result->files)) {
                makeOutputs(result->answer, ctx);
                cachedOutputs(ctx, &result->files);
            }
        },
        [alive = _alive, result, fn = done->fn] {
            if (*alive && fn)
                fn(*result);
        }
    );
}

void Backend::endWatch(Tracked &t, const std::string &error) {
    const std::unique_ptr<Tracked::TurnWatch> w = std::move(t.watch);
    if (!w || w->held)
        return; // nobody, or a branch whose start says it failed
    post([fn = w->fn, error] {
        if (fn)
            fn(failedTurn(error));
    });
}

} // namespace claude
