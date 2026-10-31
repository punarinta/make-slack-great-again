// The sending half of the Claude Code backend: the outbox, launches through
// the CLI, typing into live workers, /btw branches, permission questions,
// Stop, new sessions (see backend.h).
#include "app/claude/backend.h"
#include "app/claude/approvals.h"

#include "app/claude/backend_internal.h"
#include "app/claude/common.h"
#include "app/claude/roles.h"
#include "app/model/jobs.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <utility>

namespace claude {

using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::Message;
using model::Ts;

namespace {

// A teammate pill arrives as the Slack-style <@claude:role:x> token; Claude
// knows its teammates as the bare "@claude:role:x".
std::string unwrapPills(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '<')
            if (const size_t n = mentionAt(text, i + 1).len;
                n && text.substr(i + 1 + n, 1) == ">") {
                out.append(text.substr(i + 1, n));
                i += n + 2;
                continue;
            }
        out += text[i++];
    }
    return out;
}

} // namespace

// ── Sending ─────────────────────────────────────────────────────────────────

void Backend::send(ConvRef conv, std::string text, Ts threadTs, Done done) {
    sendWithFiles(conv, std::move(text), threadTs, {}, std::move(done));
}

// The files are copied into the cache on a worker (they may be large); a
// send to the same conversation meanwhile waits its turn behind them.
void Backend::sendWithFiles(
    ConvRef conv, std::string text, Ts threadTs, std::vector<std::string> files, Done done
) {
    const bool waits = std::find(_copying.begin(), _copying.end(), conv) != _copying.end() ||
                       std::any_of(_sendQueue.begin(), _sendQueue.end(), [conv](const Queued &q) {
                           return q.conv == conv;
                       });
    if (files.empty() && !waits) {
        Unsent back{conv, threadTs, text, {}};
        sendText(conv, std::move(text), threadTs, std::move(done), std::move(back));
        return;
    }
    _sendQueue.push_back({conv, std::move(text), threadTs, std::move(files), std::move(done)});
    sendNext(conv);
}

void Backend::sendNext(ConvRef conv) {
    while (std::find(_copying.begin(), _copying.end(), conv) == _copying.end()) {
        const auto it = std::find_if(_sendQueue.begin(), _sendQueue.end(), [conv](const Queued &q) {
            return q.conv == conv;
        });
        if (it == _sendQueue.end())
            return;
        Queued q = std::move(*it);
        _sendQueue.erase(it);
        if (q.files.empty()) {
            Unsent back{conv, q.threadTs, q.text, {}};
            sendText(conv, std::move(q.text), q.threadTs, std::move(q.done), std::move(back));
            continue;
        }
        _copying.push_back(conv);
        // Taken before q moves into the callback (argument order is unspecified);
        // q keeps them, for onUnsent.
        std::vector<std::string> files = q.files;
        uploadFiles(
            std::move(files),
            [this,
             conv,
             q = std::move(q)](std::vector<std::string> copies, std::string failed) mutable {
                std::erase(_copying, conv);
                if (!failed.empty()) {
                    const std::string why =
                        i18n::arg(tr("Couldn't read %1."), std::string(file::baseName(failed)));
                    if (q.done)
                        q.done(false, why);
                    reportError(why);
                    giveBack({conv, q.threadTs, std::move(q.text), std::move(q.files)});
                } else {
                    std::string text = withAttachments(q.text, copies);
                    sendText(
                        conv,
                        std::move(text),
                        q.threadTs,
                        std::move(q.done),
                        {conv, q.threadTs, std::move(q.text), std::move(q.files)}
                    );
                }
                sendNext(conv);
            }
        );
        return;
    }
}

void Backend::uploadFiles(
    std::vector<std::string> files, std::function<void(std::vector<std::string>, std::string)> then
) {
    struct Job {
        std::vector<std::string> files, copies;
        std::string              failed; // the file that couldn't be read
    };
    auto job   = std::make_shared<Job>();
    job->files = std::move(files);
    model::runInBackground(
        _app,
        [job] {
            for (const std::string &path : job->files) {
                std::string copy = cacheUpload(path);
                if (copy.empty()) {
                    job->failed = path;
                    job->copies.clear();
                    return;
                }
                job->copies.push_back(std::move(copy));
            }
        },
        [alive = _alive, job, then = std::move(then)] {
            if (*alive)
                then(std::move(job->copies), std::move(job->failed));
        }
    );
}

void Backend::reportError(const std::string &message) {
    LOG_INFO("claude", "%s", message.c_str());
    if (onError)
        onError(message);
}

// Posted: it's said from inside the send machinery, which the composer
// taking it back mustn't run into.
void Backend::giveBack(Unsent back) {
    if (back.conv == kNoConv || (str::trim(back.text).empty() && back.files.empty()))
        return;
    post([this, back = std::move(back)] {
        if (onUnsent)
            onUnsent(back.conv, back.thread, back.text, back.files);
    });
}

void Backend::sendText(ConvRef conv, std::string raw, Ts threadTs, Done done, Unsent back) {
    Tracked          *t    = findRef(conv);
    // The composer's text as typed: Claude reads markdown best as written.
    std::string       text = unwrapPills(raw);
    std::string       reason;
    Tracked          *target    = t; // who gets the message
    Ts                relayRoot = 0; // a reply in a subagent's thread
    std::string       shown;
    const std::string body(str::trim(text));
    const bool        btw  = body == "/btw" || str::startsWith(body, "/btw ") ||
                             str::startsWith(body, "/btw\n") || str::startsWith(body, "/btw\t");
    const auto        fail = [&](const std::string &why) {
        reportError(i18n::arg(tr("Couldn't send message: %1"), why));
        postDone(std::move(done), false, why);
        giveBack(std::move(back));
    };
    if (!t) {
        reason = tr("This session no longer exists.");
    } else if (threadTs && (target = forkFor(t->convId, threadTs))) {
        reason = readOnlyReason(*target); // a /btw thread continues its branch
    } else if (threadTs) {
        // A subagent can't be typed to: the session passes the reply on.
        target                    = t;
        const std::string agentId = subagentOf(*t, threadTs);
        if (agentId.empty()) {
            reason = tr("This subagent can't be written to until it has started.");
        } else {
            reason    = readOnlyReason(*t);
            relayRoot = threadTs;
            shown     = text;
            text      = subagentReplyPrompt(agentId, text);
        }
    } else if (btw) {
        // A side question: a branch of the session, which itself isn't
        // touched — so it can be asked while Claude is busy, or of a
        // terminal's session.
        const std::string question(str::trim(std::string_view(body).substr(4)));
        if (question.empty())
            reason = tr("Type your question after /btw.");
        else if (t->info.sessionId.empty())
            reason = tr("Send the session its first message before asking on the side.");
        else if (_creds.claudePath.empty())
            reason = readOnlyReason(*t);
        if (reason.empty()) {
            ForkOptions opts;
            opts.back = std::move(back);
            startFork(*t, question, std::move(done), std::move(opts));
            return;
        }
    } else {
        reason = readOnlyReason(*t);
    }
    if (!reason.empty()) {
        fail(reason);
        return;
    }
    if (!target->announcedInit)
        sync(*target); // what's there already isn't news
    // How to spawn the teammates it mentions, which the session may not know
    // as subagent types; the chat shows the message without it. Those it
    // doesn't offer (a session started without --agents: in a terminal, or
    // before the team were types) count when merely named, too. A session yet
    // to start gets the team's types with it.
    std::vector<Role> unoffered;
    if (!target->info.sessionId.empty())
        for (const Role &r : _team.listed())
            if (!target->parser.agentTypes().count(r.id))
                unoffered.push_back(r);
    if (!relayRoot) {
        const std::string note =
            teammateNote(text, [this](std::string_view id) { return _team.find(id); }, unoffered);
        if (!note.empty()) {
            if (shown.empty())
                shown = text;
            text += note;
        }
    }
    enqueue(*target, std::move(text), relayRoot, std::move(shown), std::move(back));
    postDone(std::move(done), true);
}

void Backend::enqueue(
    Tracked &target, std::string text, Ts relayRoot, std::string shown, Unsent back
) {
    if (!target.announcedInit)
        sync(target); // what's there already isn't news
    // msga's copy of it, from now on: after everything shown, uniquely timed.
    Ts micros = nowMs() * 1000;
    if (const Ts last = lastShownTs(target))
        micros = std::max(micros, last + 1);
    target.outbox.push_back(
        {std::move(text), micros, relayRoot, std::move(shown), std::move(back)}
    );
    // A record timed before that (clocks, the same millisecond) would
    // otherwise be tie-broken onto the copy's very ts, and its prompt never
    // seen landing.
    target.parser.reserveTs(micros);
    dispatch(target); // at once when Claude is free; otherwise when its turn ends
    sync(target);     // its copy shows
    syncMeta(target);
}

void Backend::failSends(Tracked &t, const std::string &reason) {
    if (!t.inFlight && !t.flying && t.outbox.empty())
        return; // nothing on its way
    // What the composer handed over comes back to it, in the order sent.
    std::vector<Unsent> backs;
    if (t.inFlight)
        backs.push_back(std::exchange(t.inFlightBack, {}));
    if (t.flying)
        backs.push_back(std::move(t.flying->back));
    for (Tracked::Outgoing &o : t.outbox)
        backs.push_back(std::move(o.back));
    if (auto done = std::exchange(t.inFlight, {})) {
        t.watch.reset(); // a branch that never started: its start says so
        done(false, reason);
    } else if (t.watch) {
        endWatch(t, reason);
    }
    const bool copies = t.flying || !t.outbox.empty();
    t.flying.reset();
    t.outbox.clear();
    if (copies)
        sync(t); // msga's copies of them go
    reportError(i18n::arg(tr("Couldn't send message: %1"), reason));
    // The last first: each goes in front of what the composer holds.
    for (auto it = backs.rbegin(); it != backs.rend(); ++it)
        giveBack(std::move(*it));
}

// msga's turn is over once its end is in the transcript — or, failing that,
// once the session has sat idle and silent for a while since our prompt
// landed (its last record, not the landing: a turn goes on writing long after
// its prompt).
void Backend::settleTurn(Tracked &t) {
    if (!t.sending)
        return;
    const int64_t quietSinceMs = std::max(t.promptLandedMs, t.parser.lastActivity() / 1000);
    const bool ended = t.promptLanded && (!t.parser.turnOpen() ||
                                          (!working(t) && nowMs() - quietSinceMs > kQuietTurnMs));
    if (ended) {
        t.sending = false;
        sync(t); // a pending last answer becomes visible now
    } else if (
        !t.promptLanded && !t.launching && nowMs() - t.sendStartedMs > kLaunchTimeoutMs &&
        !(t.handedOver && working(t))
    ) {
        t.sending = false;
        failSends(t, tr("Claude Code didn't pick up the message."));
    }
}

bool Backend::loginKnownGood() const {
    return _loginCheckedMs > 0 && _login != Login::Out && nowMs() - _loginCheckedMs < kLoginFreshMs;
}

void Backend::whenLoggedIn(std::function<void(bool)> then) {
    if (loginKnownGood()) {
        then(true);
        return;
    }
    _loginWaiters.push_back(std::move(then));
    if (std::exchange(_loginChecking, true))
        return; // on its way
    _launcher->checkLogin([this, alive = _alive](Login login) {
        if (!*alive)
            return;
        _loginChecking  = false;
        _login          = login;
        _loginCheckedMs = nowMs();
        if (login == Login::Out)
            LOG_INFO("claude", "not logged in");
        // Unknown lets it through: the turn says so itself if a login is missing.
        for (auto &w : std::exchange(_loginWaiters, {}))
            w(login != Login::Out);
    });
}

bool Backend::typesLive(const Tracked &t) const {
    // A live background worker is typed to (Launcher::sendLive), busy or not:
    // stopping it to resume would end what it runs — a subagent, a background
    // command, a scheduled prompt — and a message would otherwise wait for
    // all of that to finish. A slash command still takes the old way (some
    // open a panel in the terminal UI that keeps its keyboard) — but for one
    // that just runs (/compact) when only a background command holds it up:
    // that may run for hours or never end, and the old way would kill it.
    if (t.info.kind != SessionInfo::Kind::Background || !t.info.running ||
        t.info.sessionId.empty() || t.outbox.empty())
        return false;
    const std::string_view text = str::trim(t.outbox.front().text);
    if (str::startsWith(text, "/") && (!AttachInput::isLiveCommand(text) ||
                                       !statusHasShell(t.info.status) || t.sending || working(t)))
        return false;
    if (nowMs() < _typeLiveOffUntilMs)
        return false; // it hasn't been working lately: see typeLive
    if (nowMs() < t.typeLiveAfterMs || awaitsApproval(t.info))
        return false; // the UI has a question up: nothing typed goes to the prompt
    // One at a time, in order: the last one typed first has to have landed.
    return !t.flying && !(t.sending && !t.promptLanded);
}

void Backend::typeLive(Tracked &t) {
    Tracked::Outgoing next = t.outbox.front();
    t.outbox.pop_front();
    // A turn of msga's may be under way (its prompt landed): the new message
    // joins it, and msga's turn now ends once this one's is over.
    const bool    wasSending  = t.sending;
    const bool    wasLanded   = t.promptLanded;
    const int64_t wasLandedMs = t.promptLandedMs;
    t.sending                 = true;
    t.launching               = true;
    t.handedOver              = false;
    t.sendStartedMs           = nowMs();
    t.promptLanded            = false;
    t.flying                  = next;
    if (t.watch)
        t.watch->sent = true;
    const std::string convId = t.convId;
    // Its done may run before sendLive returns (nothing to type, no terminal):
    // then the handle is a finished one, and `t` may be gone by then too.
    const auto        ran    = std::make_shared<bool>(false);
    const auto        handle = _launcher->sendLive(
        t.info.sessionId,
        t.info.cwd,
        next.text,
        [this, alive = _alive, ran, convId, wasSending, wasLanded, wasLandedMs](
            AttachInput::Outcome outcome, std::string detail
        ) {
            *ran = true;
            if (!*alive)
                return;
            Tracked *t = find(convId);
            if (!t)
                return; // removed from msga meanwhile, its worker stopped with it
            t->typing.reset();
            t->launching       = false;
            const bool stopNow = std::exchange(t->stopRequested, false);
            auto       restore = [&] {
                t->sending        = wasSending;
                t->promptLanded   = wasLanded;
                t->promptLandedMs = wasLandedMs;
            };
            if (outcome == AttachInput::Outcome::Unconfirmed)
                // Enter went to the box with the message in it: taken as
                // sent, and the transcript settles it — its prompt landing,
                // or the launch timeout in settleTurn if it never does.
                LOG_INFO(
                    "claude", "typed into %s, not seen taken: %s", convId.c_str(), detail.c_str()
                );
            if (outcome == AttachInput::Outcome::Sent ||
                outcome == AttachInput::Outcome::Unconfirmed) {
                _typeLiveMisses = 0;
                t->handedOver   = true;
                if (stopNow)
                    stopWorker(*t); // "Stop" came while it was being typed
                else
                    scheduleRefresh();
                return;
            }
            if (outcome == AttachInput::Outcome::Failed) {
                // Typed in part, maybe: sending it again could say it twice.
                LOG_WARN("claude", "typing into %s failed: %s", convId.c_str(), detail.c_str());
                restore();
                failSends(
                    *t, i18n::arg(tr("Claude Code's session didn't take the message: %1"), detail)
                );
                refresh();
                return;
            }
            // Nothing was typed: the message waits as it did, for another try
            // or — the session free for it — the old way (stop, resume).
            LOG_INFO("claude", "%s not ready for typing (%s)", convId.c_str(), detail.c_str());
            restore();
            if (stopNow) { // "Stop" took it back before a key was typed
                t->flying.reset();
                stopWorker(*t);
                return;
            }
            // Missed again and again with Claude idle — where the prompt box
            // should just be there (a busy session may rightly have a
            // question up): the terminal UI isn't what it was, or `attach`
            // can't run here. The old way only, for a while: each miss costs
            // a message its wait for the prompt box.
            const bool idle = !working(*t);
            if (idle && ++_typeLiveMisses >= 3) {
                _typeLiveMisses     = 0;
                _typeLiveOffUntilMs = nowMs() + 10 * 60'000;
            }
            if (t->flying)
                t->outbox.push_front(*t->flying);
            t->flying.reset();
            t->typeLiveAfterMs = nowMs() + kTypeRetryMs;
            after(int(kTypeRetryMs) + 50, [this, convId] {
                if (Tracked *t = find(convId))
                    dispatch(*t);
            });
            dispatch(*t);
            sync(*t);
        }
    );
    if (!*ran)
        if (Tracked *still = find(convId))
            still->typing = handle;
    scheduleRefresh(); // the dot and "typing" follow at once
}

void Backend::readApproval(Tracked &t) {
    const auto reset = [&t] {
        t.approvalOptions.clear();
        t.approvalReads       = 0;
        t.approvalReadAfterMs = 0;
        t.approvalAnswered    = false;
        t.approvalSinceMs     = nowMs();
        t.hookQuestion.clear();
        t.hookAnswers.clear();
    };
    if (!awaitsApproval(t.info)) {
        t.approvalNeeds.clear();
        reset();
        return;
    }
    if (!t.answering.expired())
        return; // being read or answered right now
    if (t.approvalNeeds != t.info.needs) {
        t.approvalNeeds = t.info.needs;
        reset();
    }
    if (hookApproval(t))
        return;
    // Never "answer it in a terminal": a read that fails is tried again, a
    // few times quickly, then slowly for as long as the question waits.
    if (!t.approvalOptions.empty() || t.approvalAnswered || nowMs() < t.approvalReadAfterMs)
        return;
    ++t.approvalReads;
    const std::string        convId = t.convId;
    const std::string        needs  = t.info.needs;
    std::string              program;
    std::vector<std::string> argv;
    _launcher->attachCommand(t.info.sessionId, program, argv);
    // Its done may run before choose() returns (`attach` didn't start).
    const auto ran = std::make_shared<bool>(false);
    const auto handle = AttachAnswer::choose( // option 0: the question only read
        _app,
        program,
        argv,
        t.info.cwd,
        [needs](const PermissionQuestion &q) { return questionIsFor(needs, q); },
        0,
        {},
        [this, alive = _alive, ran, convId, needs](
            AttachAnswer::Outcome outcome, std::optional<PermissionQuestion> q, std::string detail
        ) {
            *ran = true;
            if (!*alive)
                return;
            Tracked *t = find(convId);
            if (!t)
                return;
            t->answering.reset(); // it lingers a moment after `attach` is told to end
            if (t->approvalNeeds != needs)
                return;
            if (outcome == AttachAnswer::Outcome::Done && q) {
                t->approvalOptions = q->options;
            } else {
                LOG_INFO(
                    "claude", "couldn't read %s's question (%s)", convId.c_str(), detail.c_str()
                );
                const int wait =
                    t->approvalReads < kApprovalReads ? kApprovalRetryMs : kApprovalSlowRetryMs;
                t->approvalReadAfterMs = nowMs() + wait;
                after(wait, [this] { scheduleRefresh(); });
            }
            sync(*t); // the buttons, or the note
        }
    );
    if (*ran)
        return; // done already: it said what it had to
    t.answering = handle;
    sync(t); // no hint while it's being read
}

// The question as its hook (approvals.h) brought it: its options from there.
// True while it is (or may yet be) answered that way — false leaves it to
// the screen: a session without the hook, or a hook that gave up on msga.
bool Backend::hookApproval(Tracked &t) {
    if (!t.info.permissionHook)
        return false;
    for (HookQuestion &q : pendingHookQuestions(_approvalsDir)) {
        if (q.sessionId != t.info.sessionId)
            continue;
        if (q.id != t.hookQuestion) { // a new one (perhaps asking just what the last did)
            t.hookQuestion     = q.id;
            t.approvalAnswered = false;
            t.approvalOptions.clear();
            t.hookAnswers.clear();
            for (auto &o : q.options) {
                t.approvalOptions.push_back({int(t.approvalOptions.size()) + 1, o.label});
                t.hookAnswers.push_back(std::move(o.answer));
            }
            sync(t); // the buttons
        }
        takeHookQuestion(_approvalsDir, q.id);
        return true;
    }
    if (!t.hookQuestion.empty()) {
        if (t.approvalAnswered)
            return true; // answered: Claude Code moves on in a moment
        // Its hook gave up on it (or was ended): the dialog asks.
        t.hookQuestion.clear();
        t.hookAnswers.clear();
        t.approvalOptions.clear();
        sync(t);
        return false;
    }
    // Not come through yet: the hook writes it as the session starts waiting.
    const int64_t left = t.approvalSinceMs + kHookQuestionMs - nowMs();
    if (left <= 0)
        return false;
    after(int(left) + 50, [this] { scheduleRefresh(); });
    return true;
}

void Backend::pressButton(ConvRef conv, Ts, const std::string &buttonId, Done done) {
    const auto fail = [&](const std::string &why) { postDone(std::move(done), false, why); };
    Tracked   *t    = findRef(conv);
    if (!t || !awaitsApproval(t->info) || t->approvalNeeds != t->info.needs)
        return fail(tr("Claude isn't waiting for that approval any more."));
    int number = 0;
    if (!str::startsWith(buttonId, "option:") || buttonId.size() <= 7)
        return fail("unknown button");
    for (char c : std::string_view(buttonId).substr(7)) {
        if (c < '0' || c > '9')
            return fail("unknown button");
        number = number * 10 + (c - '0');
    }
    if (!t->hookQuestion.empty()) {
        // Asked through the hook: its answer is left for it.
        if (t->approvalAnswered || number < 1 || number > int(t->hookAnswers.size()) ||
            !answerHookQuestion(
                _approvalsDir, t->hookQuestion, t->hookAnswers[size_t(number - 1)]
            )) {
            scheduleRefresh();
            return fail(tr("Claude isn't waiting for that approval any more."));
        }
        t->approvalOptions.clear();
        t->hookAnswers.clear();
        t->approvalAnswered = true;
        sync(*t);
        scheduleRefresh();
        postDone(std::move(done), true, {});
        return;
    }
    if (!t->answering.expired())
        return fail(tr("The answer is on its way."));
    std::string label;
    for (const auto &o : t->approvalOptions)
        if (o.number == number)
            label = o.label;
    const std::string        convId = t->convId;
    const std::string        needs  = t->info.needs;
    std::string              program;
    std::vector<std::string> argv;
    _launcher->attachCommand(t->info.sessionId, program, argv);
    // Its done may run before choose() returns (`attach` didn't start); the
    // caller's done never runs from inside this call (model::Backend).
    const auto ran    = std::make_shared<bool>(false);
    const auto handle = AttachAnswer::choose(
        _app,
        program,
        argv,
        t->info.cwd,
        [needs](const PermissionQuestion &q) { return questionIsFor(needs, q); },
        number,
        label,
        [this, alive = _alive, ran, convId, done](
            AttachAnswer::Outcome outcome, std::optional<PermissionQuestion>, std::string detail
        ) {
            *ran = true;
            if (!*alive)
                return;
            const auto reply = [this, &done](bool ok, std::string why) {
                postDone(done, ok, std::move(why));
            };
            Tracked *t = find(convId);
            if (t)
                t->answering.reset();
            if (outcome == AttachAnswer::Outcome::Done) {
                if (t) {
                    // Answered: no buttons for it any more, whatever the job's
                    // state still says until Claude Code rewrites it.
                    t->approvalOptions.clear();
                    t->approvalAnswered = true;
                    sync(*t);
                }
                scheduleRefresh();
                reply(true, {});
                return;
            }
            LOG_WARN(
                "claude", "answering %s's question failed: %s", convId.c_str(), detail.c_str()
            );
            reply(false, detail);
        }
    );
    if (!*ran)
        if (Tracked *still = find(convId))
            still->answering = handle;
}

void Backend::dispatch(Tracked &t) {
    if (t.outbox.empty() || t.stopping || t.launching)
        return;
    const bool live = typesLive(t);
    // Otherwise one turn at a time: the next message goes once Claude is done
    // with the last (and a session waiting on an approval takes nothing until
    // it's given). A background command still running waits too: sending
    // stops the worker, which would kill the command.
    if (!live && (t.sending || busy(t) || awaitsApproval(t.info) ||
                  (t.info.running && statusHasShell(t.info.status))))
        return;
    // Without a login the turn only gets "Not logged in" back: that is a
    // failed send, said before anything starts.
    if (!loginKnownGood()) {
        const std::string convId = t.convId;
        whenLoggedIn([this, convId](bool loggedIn) {
            Tracked *t = find(convId);
            if (!t)
                return;
            if (loggedIn)
                dispatch(*t);
            else if (!t->sending && !t->launching && !t->flying)
                failSends(*t, notLoggedInMessage());
        });
        return;
    }
    if (live) {
        typeLive(t);
        return;
    }
    Tracked::Outgoing next = t.outbox.front();
    t.outbox.pop_front();
    t.sending       = true;
    t.launching     = true;
    t.sendStartedMs = nowMs();
    t.promptLanded  = false;
    t.handedOver    = false;
    t.flying        = std::move(next);
    if (t.watch)
        t.watch->sent = true;
    const std::string convId = t.convId;
    const std::string cwd    = t.info.cwd;
    const bool        fresh  = t.info.sessionId.empty(); // a "+" session: msga's
    auto              settled =
        [this, alive = _alive, convId, cwd, fresh](std::string sessionId, std::string error) {
            if (!*alive)
                return;
            Tracked *t = find(convId);
            if (!t) {
                // Removed from msga while the CLI was starting the turn: stop it
                // there, and keep it away — deleted with its worktrees if msga's.
                if (sessionId.empty())
                    return;
                const auto cleanup = std::make_shared<Cleanup>();
                if (fresh)
                    removeOwned(sessionId, {}, cwd, true, cleanup);
                else
                    removeIfOwned(sessionId, {}, cwd, true, cleanup, [this, sessionId, cwd] {
                        stopRemoved(sessionId, cwd);
                    });
                release(cleanup);
                return;
            }
            t->launching = false;
            if (sessionId.empty()) {
                t->sending       = false;
                t->stopRequested = false;
                failSends(*t, error);
                refresh();
                return;
            }
            if (t->info.sessionId.empty()) {
                // A "+" session: Claude Code just picked its id.
                t->info.sessionId  = sessionId;
                t->info.kind       = SessionInfo::Kind::Background;
                _convOf[sessionId] = convId;
                _launchedHere.insert(sessionId);
                t->skipPermissionChecks = false; // saved with the session from here on
            } else if (sessionId != t->info.sessionId) {
                adoptCopy(*t, sessionId); // Claude Code went on in a copy of it
            }
            scheduleSaveKnown();
            if (std::exchange(t->stopRequested, false)) {
                stopWorker(*t); // "Stop" came while the turn was being launched
                return;
            }
            scheduleRefresh();
        };
    if (!t.deniedTools.empty() && !t.info.sessionId.empty()) {
        // A branch that goes without tools: a resume would keep them off
        // (they're saved with it) — unless it raced the old worker's exit and
        // Claude Code started a copy, which runs without its options. A fork
        // passing them again is that copy, made on purpose: adopted the same
        // way (adoptCopy), the original's worker stopped.
        const std::string folder = file::absolute(cwd);
        _forkingIn.insert(folder); // the copy mustn't flash up in the list meanwhile
        _launcher->fork(
            t.info.sessionId,
            cwd,
            t.flying->text,
            [this, alive = _alive, folder, settled](std::string sessionId, std::string error) {
                if (*alive)
                    _forkingIn.erase(folder);
                settled(std::move(sessionId), std::move(error));
            },
            t.deniedTools
        );
    } else if (t.info.sessionId.empty()) {
        _launcher->start(
            t.info.cwd,
            t.flying->text,
            t.skipPermissionChecks,
            appendedPrompt(_team.resolve(roleOf(t))),
            subagentsJson(_team.listed()),
            settled
        );
    } else {
        const bool background = t.info.kind == SessionInfo::Kind::Background;
        // A background session's worker idles on after a turn; it has to be
        // stopped before the session can be resumed (see Launcher).
        _launcher->resume(
            t.info.sessionId,
            t.info.cwd,
            t.flying->text,
            background,
            background && t.info.running,
            settled
        );
    }
    scheduleRefresh(); // the dot and "typing" follow at once
}

void Backend::adoptCopy(Tracked &t, const std::string &copyId) {
    // A resume that raced the old worker's exit: Claude Code started a copy of
    // the session and put the new turn there. The copy holds the whole
    // conversation (its records repeat the original's, uuids and all, which
    // the parser skips), so this chat simply goes on in it — nothing shown
    // changes, no thread appears. The original is put away as if removed from
    // msga, and its worker, if any, stopped.
    const std::string old = t.info.sessionId;
    _convOf.erase(old);
    if (copyId != t.convId)
        _convOf[copyId] = t.convId;
    t.info.sessionId = copyId;
    t.transcriptPath = _paths.findTranscript(copyId); // "" until it's written: tail looks again
    t.offset         = 0;
    if (_launchedHere.count(old))
        _launchedHere.insert(copyId);
    stopRemoved(old, t.info.cwd);
}

bool Backend::canStopSession(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    if (!t || asThread(*t) || t->stopping || t->stopRequested)
        return false;
    if (t->sending || !t->outbox.empty())
        return true; // msga's own turn, or messages waiting for one
    // Any live worker, idle too: an idle one can still wake itself — a
    // subagent's result or a scheduled prompt starts a turn nobody sent.
    return t->info.kind == SessionInfo::Kind::Background && t->info.running;
}

void Backend::stopSession(ConvRef conv) {
    Tracked *t = findRef(conv);
    if (!t || t->stopping)
        return;
    // Messages still waiting are dropped: sending one would resume the session.
    const bool queued = !t->outbox.empty();
    t->outbox.clear();
    if (t->launching) {
        // The CLI is starting the turn right now; stop it the moment it has.
        t->stopRequested         = true;
        // A message still waiting for the prompt box goes with the rest (the
        // worker is stopped from typeLive's answer, which comes at once).
        // cancel() runs typeLive's done at once, which stops the worker
        // (stopWorker) — and may have refreshed the session away.
        const std::string convId = t->convId;
        if (const auto typing = t->typing.lock())
            typing->cancel();
        t = find(convId);
        if (t && queued)
            sync(*t);
        refresh(); // no dot from here on (busy() honours stopRequested)
        return;
    }
    if (t->info.sessionId.empty()) { // a "+" session nothing was sent to yet
        if (queued)
            sync(*t);
        return;
    }
    stopWorker(*t);
}

void Backend::stopWorker(Tracked &t) {
    t.stopping = true;
    t.sending  = false;
    // A prompt not in the transcript yet may never get there now; if it does,
    // it shows from there.
    t.flying.reset();
    const std::string convId = t.convId;
    _launcher->stop(t.info.sessionId, t.info.cwd, [this, alive = _alive, convId] {
        if (!*alive)
            return;
        if (Tracked *t = find(convId))
            t->stopping = false;
        refresh();
    });
    refresh(); // no dot, no typing from here on
}

// ── /btw: a branch of the session ───────────────────────────────────────────

void Backend::startFork(Tracked &parent, const std::string &question, Done done, ForkOptions opts) {
    const std::string parentConv = parent.convId;
    whenLoggedIn([this, parentConv, question, done, opts = std::move(opts)](bool loggedIn) mutable {
        Tracked *t = find(parentConv);
        if (loggedIn && t) {
            launchFork(*t, question, done, std::move(opts));
            return;
        }
        const std::string error =
            !t ? std::string(tr("The session was removed from msga.")) : notLoggedInMessage();
        reportError(i18n::arg(tr("Couldn't send message: %1"), error));
        giveBack(std::move(opts.back));
        if (opts.started)
            opts.started({}, error);
        else if (done)
            done(false, error);
    });
}

void Backend::launchFork(
    Tracked &parent, const std::string &question, Done done, ForkOptions opts
) {
    const std::string parentConv = parent.convId;
    const std::string cwd        = parent.info.cwd;
    const std::string folder     = file::absolute(cwd);
    _forkingIn.insert(folder); // the branch mustn't flash up in the list meanwhile
    std::vector<std::string> denied = opts.deniedTools; // opts moves into the callback
    _launcher->fork(
        parent.info.sessionId,
        cwd,
        question,
        [this, alive = _alive, parentConv, cwd, folder, done, opts = std::move(opts)](
            std::string sessionId, std::string error
        ) mutable {
            if (!*alive)
                return;
            _forkingIn.erase(folder);
            if (sessionId.empty() || !find(parentConv)) {
                if (error.empty())
                    error = tr("The session was removed from msga.");
                reportError(i18n::arg(tr("Couldn't send message: %1"), error));
                giveBack(std::move(opts.back));
                if (opts.started)
                    opts.started({}, error);
                else if (done)
                    done(false, error);
                scheduleRefresh();
                return;
            }
            _launchedHere.insert(sessionId);
            Tracked &f       = ensureTracked(convIdFor(sessionId));
            f.info.sessionId = sessionId;
            f.info.cwd       = cwd;
            f.info.kind      = SessionInfo::Kind::Background;
            f.forkOf         = parentConv; // detectForks confirms it from the transcript
            f.awaitingRoot   = true;
            // Its turn is msga's, like any send: typing, then the answer.
            f.sending        = true;
            f.sendStartedMs  = nowMs();
            f.promptLanded   = false;
            f.inFlight       = done;
            f.inFlightBack   = std::move(opts.back);
            f.deniedTools    = std::move(opts.deniedTools);
            if (opts.turn) {
                f.watch       = std::make_unique<Tracked::TurnWatch>();
                f.watch->fn   = std::move(opts.turn);
                f.watch->send = f.watch->sent = true;
                f.watch->from = std::string::npos; // from its first prompt, once read
                f.watch->held = bool(opts.started);
            }
            if (opts.started)
                f.inFlight = [this,
                              convId  = f.convId,
                              started = std::move(opts.started)](bool ok, const std::string &why) {
                    if (ok)
                        branchStarted(convId, started);
                    else
                        started({}, why);
                };
            // Everything in the thread is news.
            f.announcedAsThread = true;
            f.announcedInit     = true;
            f.announced.clear();
            ++f.announcedGen;
            scheduleSaveKnown();
            scheduleRefresh();
        },
        denied
    );
}

// ── New sessions ────────────────────────────────────────────────────────────

std::string Backend::cannotStartIn(const std::string &dir) const {
    if (_creds.claudePath.empty())
        return notInstalledMessage();
    if (!file::isDir(dir))
        return i18n::arg(tr("%1 isn't a folder."), dir);
    // Background sessions refuse a folder Claude Code hasn't trusted. The
    // launcher trusts the picked one, but Claude Code never saves trust for
    // the home folder: say so now rather than on the first message.
    if (!canTrustFolder(dir))
        return tr(
            "Claude Code doesn't run background sessions in the home folder. Pick a "
            "project folder."
        );
    return {};
}

std::string Backend::agentSessionBlocker(const std::string &dir) const {
    return cannotStartIn(dir);
}

Backend::Tracked &
Backend::createSession(const std::string &dir, bool skipPermissionChecks, const std::string &role) {
    // The session itself starts with the first message (Claude Code picks its
    // id then); until then it is a conversation of its own.
    std::string    id  = crypto::uuid4();
    // No OS randomness: the clock and a counter still keep it apart from
    // every other "+" session (the ids are saved, so across runs too).
    static int64_t seq = 0;
    while (id.empty() || find(kNewPrefix + id))
        id = str::concat({"t", str::number(base::nowMicros()), "-", str::number(++seq)});
    Tracked &t             = ensureTracked(kNewPrefix + id);
    t.info.cwd             = dir;
    t.info.kind            = SessionInfo::Kind::Background;
    t.skipPermissionChecks = skipPermissionChecks;
    t.role                 = _team.find(role) ? role : std::string(kGeneralist);
    t.announcedInit        = true; // everything in it is new, the first prompt included
    syncMeta(t);
    return t;
}

void Backend::startAgentSession(
    const std::string                                &dir,
    bool                                              skipPermissionChecks,
    const std::string                                &role,
    std::function<void(ConvRef, const std::string &)> done
) {
    if (const std::string why = cannotStartIn(dir); !why.empty()) {
        post([done, why] {
            if (done)
                done(kNoConv, why);
        });
        return;
    }
    // Checked now, not on the first message: nothing gets started without it.
    whenLoggedIn([this, dir, skipPermissionChecks, role, done](bool loggedIn) {
        if (!loggedIn) {
            if (done)
                done(kNoConv, notLoggedInMessage());
            return;
        }
        Tracked &t = createSession(dir, skipPermissionChecks, role);
        if (done)
            done(t.ref, {});
    });
}

} // namespace claude
