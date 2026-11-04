// The managing half of the Claude Code backend: "Remove from msga" (and the
// worktrees of sessions msga started), finding sessions, the team, your
// profile, slash commands, prompt history, deleting messages, reactions and
// search (see backend.h).
#include "app/claude/backend.h"

#include "app/claude/backend_internal.h"
#include "app/claude/catalog.h"
#include "app/claude/common.h"
#include "app/claude/outputs.h"
#include "app/claude/render.h"
#include "app/claude/worktrees.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/str.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace claude {

using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::Message;
using model::Ts;
using model::UserRef;

// ── Remove from msga ────────────────────────────────────────────────────────

void Backend::leave(ConvRef conv) {
    Tracked *t = findRef(conv);
    if (!t)
        return;
    const std::string        convId  = t->convId;
    // Its /btw threads go with it — they would otherwise turn up as sessions.
    // Their worktrees are deleted once all have stopped: a thread works in
    // its parent's, which must not look in use for the parent's worker.
    const auto               cleanup = std::make_shared<Cleanup>();
    std::vector<std::string> forks;
    for (const auto &[id, tp] : _sessions)
        if (tp->forkOf == convId && asThread(*tp))
            forks.push_back(id);
    for (const auto &f : forks)
        hideSession(f, cleanup);
    hideSession(convId, cleanup);
    release(cleanup);
    pumpTyping();
    saveKnown();
}

void Backend::hideSession(const std::string &convId, const std::shared_ptr<Cleanup> &cleanup) {
    const auto it = _sessions.find(convId);
    if (it == _sessions.end())
        return;
    Tracked &t = *it->second;
    // Messages still waiting here are dropped, and a session msga started is
    // deleted with all it runs and every worktree it used: nothing of it goes
    // on once it's out of sight. (A turn being launched right now is dealt
    // with once it has: dispatch.) A terminal's session is the terminal's,
    // and a background one started elsewhere is whoever's started it: it
    // only leaves msga's list.
    // As failSends: what still waited is said to be gone with it.
    if (!t.outbox.empty() || t.flying || t.inFlight)
        reportError(
            i18n::arg(tr("Couldn't send message: %1"), tr("The session was removed from msga."))
        );
    t.outbox.clear();
    t.flying.reset();
    if (auto done = std::exchange(t.inFlight, {}))
        done(false, tr("The session was removed from msga."));
    endWatch(t, tr("The session was removed from msga."));
    // A message being typed is taken back once the session is gone, so its
    // done (run at once by cancel) finds nothing to put back.
    const std::weak_ptr<AttachInput> typing = std::exchange(t.typing, {});
    if (!t.info.sessionId.empty()) {
        std::string transcript = t.transcriptPath;
        if (transcript.empty())
            transcript = _paths.findTranscript(t.info.sessionId);
        Hidden h;
        h.atMs                    = nowMs();
        h.transcript              = transcript;
        h.seenSize                = file::size(transcript);
        _hidden[t.info.sessionId] = h;
        _dropped.erase(t.info.sessionId); // here again: saved again
        _convOf.erase(t.info.sessionId);
        removeIfOwned(
            t.info.sessionId,
            t.info.jobId,
            t.info.cwd,
            t.info.kind == SessionInfo::Kind::Background,
            cleanup
        );
    }
    clearOutputs(convId); // the copies of the files it made
    forgetCaches(t);
    _reactions.erase(convId);
    if (t.ref != kNoConv) {
        for (auto s = _shown.begin(); s != _shown.end();)
            s = s->first.first == t.ref ? _shown.erase(s) : std::next(s);
        for (auto k = _typing.begin(); k != _typing.end();) {
            if (std::get<0>(k->first) == t.ref) {
                _store.setTyping(t.ref, std::get<1>(k->first), std::get<2>(k->first), false);
                k = _typing.erase(k);
            } else {
                ++k;
            }
        }
        _store.updateConversation(t.ref, [](model::Conversation &c) {
            c.member   = false;
            c.unread   = 0;
            c.mentions = 0;
        });
    }
    if (_lastConv == convId)
        _lastConv.clear();
    _sessions.erase(it);
    if (const auto p = typing.lock())
        p->cancel();
}

namespace {

// Claude Code records no parent: a session a session started with `claude
// --bg` has the CLI's answer in its parent's transcript, the Bash tool's
// output "backgrounded · <short id>" — the session id's first 8 characters.
constexpr std::string_view kBackgrounded = "backgrounded \xc2\xb7 ";
constexpr size_t           kShortId      = 8;

// The short ids a file names so, as far as it was read (transcripts only
// grow: each look reads what was added since). Kept across removals: a
// removal asks about every session msga knows, hundreds of MB of them.
struct BackgroundedScan {
    int64_t                  size = 0;
    std::vector<std::string> ids;
};
std::mutex                                        gScansLock;
std::unordered_map<std::string, BackgroundedScan> gScans; // by file

// gScansLock held.
const std::vector<std::string> &backgroundedIn(const std::string &path) {
    BackgroundedScan &s    = gScans[path];
    const int64_t     size = file::size(path);
    if (size < s.size)
        s = {}; // rewritten (or gone): read anew
    constexpr size_t kChunk = 1 << 20, kSpan = kBackgrounded.size() + kShortId;
    std::string      bytes;
    // From a little before where the last look ended: a mark it saw cut off.
    for (int64_t at = std::max<int64_t>(0, s.size - int64_t(kSpan)); at < size;
         at += int64_t(kChunk)) {
        if (!file::readRange(path, at, kChunk + kSpan, &bytes))
            break;
        const bool end = bytes.size() <= kChunk; // the file's end is in it
        for (size_t p = bytes.find(kBackgrounded); p != std::string::npos;
             p        = bytes.find(kBackgrounded, p + 1)) {
            if (p + kSpan > bytes.size() && !end)
                break; // whole in the next read
            // (At the file's end, as much of it as there is.)
            std::string id = bytes.substr(p + kBackgrounded.size(), kShortId);
            if (std::find(s.ids.begin(), s.ids.end(), id) == s.ids.end())
                s.ids.push_back(std::move(id));
        }
        if (bytes.size() <= kChunk)
            break;
    }
    s.size = std::max<int64_t>(size, 0);
    return s.ids;
}

// Whether a transcript, or one of its subagents', names session `shortId`
// as backgrounded. gScansLock held.
bool transcriptNames(const std::string &path, std::string_view shortId) {
    if (path.empty())
        return false;
    std::vector<std::string>    files{path};
    const std::string           subagents = Paths::subagentsDir(path);
    std::vector<file::DirEntry> entries;
    if (file::listDir(subagents, &entries))
        for (const auto &e : entries)
            if (!e.isDir && str::endsWith(e.name, ".jsonl"))
                files.push_back(subagents + "/" + e.name);
    for (const std::string &f : files)
        for (const std::string &id : backgroundedIn(f))
            if (str::startsWith(id, shortId)) // an id shorter than 8: what it starts with
                return true;
    return false;
}

// The sessions a removed one may have been started from: every other one
// msga knows (its session id, and its transcript — "" = to be looked for).
struct Candidate {
    std::string sessionId, transcript;
};

// Whether msga started session `sessionId` — or a session it started did,
// any number of steps back (as far as 16 sessions). On a worker: it reads
// the candidates' transcripts.
bool startedByMsga(
    const std::string               &sessionId,
    const std::vector<Candidate>    &candidates,
    const std::vector<std::string>  &launchedHere,
    const Paths                     &paths,
    std::unordered_set<std::string> &seen
) {
    if (std::find(launchedHere.begin(), launchedHere.end(), sessionId) != launchedHere.end())
        return true;
    if (seen.count(sessionId) || seen.size() > 16)
        return false;
    seen.insert(sessionId);
    // Its parent: a session whose transcript says it backgrounded this one.
    const std::string_view shortId = std::string_view(sessionId).substr(0, kShortId);
    for (const Candidate &c : candidates) {
        if (c.sessionId == sessionId)
            continue;
        bool named;
        {
            const std::lock_guard<std::mutex> hold(gScansLock);
            named = transcriptNames(
                c.transcript.empty() ? paths.findTranscript(c.sessionId) : c.transcript, shortId
            );
        }
        if (named && startedByMsga(c.sessionId, candidates, launchedHere, paths, seen))
            return true;
    }
    return false;
}

} // namespace

void Backend::removeIfOwned(
    const std::string              &sessionId,
    const std::string              &jobId,
    const std::string              &cwd,
    bool                            background,
    const std::shared_ptr<Cleanup> &cleanup,
    std::function<void()>           otherwise
) {
    if (_launchedHere.count(sessionId)) {
        removeOwned(sessionId, jobId, cwd, background, cleanup);
        return;
    }
    // Asked on a worker about the sessions msga knows as they are now: the
    // tracked ones, then the hidden ones.
    struct Ask {
        std::string              sessionId, jobId, cwd;
        bool                     background = false, owned = false;
        std::shared_ptr<Cleanup> cleanup;
        std::function<void()>    otherwise;
        std::vector<Candidate>   candidates;
        std::vector<std::string> launched;
        Paths                    paths;
    };
    auto ask        = std::make_shared<Ask>();
    ask->sessionId  = sessionId;
    ask->jobId      = jobId;
    ask->cwd        = cwd;
    ask->background = background;
    ask->cleanup    = cleanup;
    ask->otherwise  = std::move(otherwise);
    ask->launched.assign(_launchedHere.begin(), _launchedHere.end());
    ask->paths = _paths;
    for (const auto &[id, t] : _sessions)
        if (!t->info.sessionId.empty())
            ask->candidates.push_back({t->info.sessionId, t->transcriptPath});
    for (const auto &[sid, h] : _hidden)
        ask->candidates.push_back({sid, h.transcript});
    ++cleanup->pending; // its worktrees wait for the answer
    model::runInBackground(
        _app,
        [ask] {
            std::unordered_set<std::string> seen;
            ask->owned =
                startedByMsga(ask->sessionId, ask->candidates, ask->launched, ask->paths, seen);
        },
        [this, alive = _alive, ask] {
            if (!*alive)
                return;
            if (ask->owned)
                removeOwned(ask->sessionId, ask->jobId, ask->cwd, ask->background, ask->cleanup);
            else if (ask->otherwise)
                ask->otherwise();
            release(ask->cleanup);
        }
    );
}

Backend::Hidden &Backend::hide(const std::string &sessionId) {
    auto [it, added] = _hidden.try_emplace(sessionId);
    if (added) {
        _dropped.erase(sessionId); // here again: saved again
        it->second.atMs       = nowMs();
        it->second.transcript = _paths.findTranscript(sessionId);
        it->second.seenSize   = file::size(it->second.transcript);
    }
    return it->second;
}

std::string Backend::noteStopped(const std::string &sessionId) {
    // The worker writes its last records as it exits; that is no new
    // activity to bring the session back for.
    const auto h = _hidden.find(sessionId);
    if (h == _hidden.end())
        return {};
    h->second.stopping = false;
    h->second.atMs     = nowMs();
    if (h->second.transcript.empty())
        h->second.transcript = _paths.findTranscript(sessionId);
    h->second.seenSize     = file::size(h->second.transcript);
    std::string transcript = h->second.transcript;
    saveKnown();
    return transcript;
}

void Backend::stopRemoved(const std::string &sessionId, const std::string &cwd) {
    hide(sessionId).stopping = true;
    _launcher->stop(sessionId, cwd, [this, alive = _alive, sessionId] {
        if (*alive)
            noteStopped(sessionId);
    });
}

void Backend::removeOwned(
    const std::string              &sessionId,
    const std::string              &jobId,
    const std::string              &cwd,
    bool                            background,
    const std::shared_ptr<Cleanup> &cleanup
) {
    hide(sessionId);
    // The job's worktree is read now: `claude rm` drops the job.
    const std::string job  = jobId.empty() ? sessionId.substr(0, 8) : jobId;
    auto              refs = worktreesOfJob(readJobState(_paths, job));
    ++cleanup->pending;
    auto stopped = [this, sessionId, cwd, refs, cleanup]() mutable {
        std::string transcript = noteStopped(sessionId);
        // The transcript's worktrees, read on a worker (all of it is read).
        struct Look {
            std::string              transcript, sessionId, cwd;
            Paths                    paths;
            std::vector<WorktreeRef> refs;
            std::shared_ptr<Cleanup> cleanup;
        };
        auto look = std::make_shared<Look>(
            Look{std::move(transcript), sessionId, cwd, _paths, std::move(refs), cleanup}
        );
        model::runInBackground(
            _app,
            [look] {
                auto entered = worktreesOfTranscript(
                    look->transcript.empty() ? look->paths.findTranscript(look->sessionId)
                                             : look->transcript
                );
                for (auto &ref : entered)
                    addWorktree(look->refs, std::move(ref));
            },
            [this, alive = _alive, look] {
                if (!*alive)
                    return;
                for (auto &ref : look->refs) {
                    if (ref.origin.empty())
                        ref.origin = look->cwd; // where to find its repository, if it's gone
                    addWorktree(look->cleanup->refs, std::move(ref));
                }
                release(look->cleanup);
            }
        );
    };
    if (!background) { // a terminal's: nothing to stop, and a live one's
        stopped();     // worktree is kept (it's in use)
        return;
    }
    _hidden[sessionId].stopping = true;
    // No process of msga's should sit in a worktree being deleted (on Windows
    // that holds the folder), nor in a folder that's gone (it won't start).
    std::string from            = cwd;
    for (const auto &ref : refs)
        if (pathWithin(from, ref.path))
            from = file::isDir(ref.origin) ? ref.origin : std::string(file::dirName(ref.path));
    if (!file::isDir(from))
        from = base::env("HOME");
    _launcher->remove(
        sessionId, jobId, from, [alive = _alive, sessionId, stopped](std::string refusal) mutable {
            if (!*alive)
                return;
            // Claude Code keeps a worktree with changes in it; msga deletes it
            // all the same (stopped), and the session stays removed.
            if (!refusal.empty())
                LOG_INFO(
                    "claude", "rm kept %s: %s", sessionId.substr(0, 8).c_str(), refusal.c_str()
                );
            stopped();
        }
    );
}

void Backend::release(const std::shared_ptr<Cleanup> &cleanup) {
    if (--cleanup->pending > 0)
        return;
    std::vector<WorktreeRef> refs;
    for (const auto &ref : cleanup->refs) {
        // Never anything of Claude Code's own (transcripts, jobs), whatever a
        // record says.
        const std::string clean = cleanPath(ref.path);
        if (!pathWithin(ref.path, _paths.home) && !pathWithin(_paths.home, ref.path) &&
            !_reaping.count(clean)) {
            _reaping.insert(clean);
            refs.push_back(ref);
        }
    }
    if (refs.empty())
        return;
    reapWorktrees(
        _app,
        refs,
        [paths = _paths](const std::string &path) { return worktreeInUse(paths, path); },
        [this, alive = _alive, refs](std::vector<std::string> notDeleted) {
            if (!*alive)
                return;
            for (const auto &ref : refs)
                _reaping.erase(cleanPath(ref.path));
            if (notDeleted.empty())
                return;
            std::string list;
            for (const auto &p : notDeleted)
                list += (list.empty() ? "" : ", ") + p;
            LOG_WARN("claude", "worktrees not deleted: %s", list.c_str());
            reportError(i18n::arg(tr("Couldn't delete the session's worktree: %1"), list));
        }
    );
}

// ── Finding sessions ────────────────────────────────────────────────────────

void Backend::findAgentSessions(std::function<void(std::vector<FoundSession>)> done) {
    auto entries = std::make_shared<std::vector<CatalogEntry>>();
    model::runInBackground(
        _app,
        [entries, dir = _paths.projectsDir()] { *entries = scanCatalog(dir); },
        [this, alive = _alive, entries, done] {
            if (!*alive)
                return;
            std::vector<FoundSession> out;
            for (const CatalogEntry &e : *entries) {
                FoundSession f;
                f.id             = e.sessionId;
                f.title          = e.title;
                f.folder         = e.cwd;
                const Role &mate = _team.resolve(e.role, e.roleName);
                f.role           = mate.id;
                f.avatar         = mate.avatar;
                f.firstPrompt    = e.firstPrompt;
                f.lastPrompt     = e.lastPrompt;
                f.lastActiveMs   = e.modifiedMs;
                if (const Tracked *t = find(convIdFor(e.sessionId))) {
                    // A /btw branch is found in its parent.
                    const Tracked *shown = asThread(*t) ? find(t->forkOf) : t;
                    f.listed             = shown ? shown->ref : kNoConv;
                    if (!asThread(*t))
                        f.title = shownTitle(*t);
                }
                out.push_back(std::move(f));
            }
            if (done)
                done(std::move(out));
        }
    );
}

model::ConvRef Backend::addFoundSession(const std::string &sessionId) {
    if (const Tracked *t = find(convIdFor(sessionId))) {
        const Tracked *shown = asThread(*t) ? find(t->forkOf) : t;
        return shown ? shown->ref : kNoConv;
    }
    const std::string transcript = _paths.findTranscript(sessionId);
    CatalogEntry      e;
    if (transcript.empty() || !readCatalogEntry(transcript, e))
        return kNoConv;
    _hidden.erase(sessionId); // asked for by name: "Remove from msga" is undone
    std::unordered_set<std::string> listedBefore;
    for (const auto &[id, tp] : _sessions)
        if (!asThread(*tp))
            listedBefore.insert(id);

    Tracked   &t      = ensureTracked(sessionId);
    const auto roster = scanSessions(_paths);
    const auto live   = std::find_if(roster.begin(), roster.end(), [&](const SessionInfo &s) {
        return s.sessionId == sessionId;
    });
    if (live != roster.end()) {
        t.info = *live; // running (it was only hidden)
    } else {
        // Ended, like the ones msga saw end. A background session's job would
        // have been in the roster, so this one resumes as a terminal one.
        t.info.sessionId  = sessionId;
        t.info.cwd        = e.cwd;
        t.info.kind       = SessionInfo::Kind::Interactive;
        t.info.entrypoint = "cli";
    }
    t.transcriptPath = e.transcriptPath;
    tail(t);
    // Branch detection can't tell a /btw branch from a copy Claude Code made
    // on resume, and either way what's asked for here — and what was in the
    // list already — stays a session, never turns into a thread.
    detectForks();
    for (auto &[id, tp] : _sessions)
        if ((tp.get() == &t || listedBefore.count(id)) && asThread(*tp))
            tp->standalone = true;
    refresh(); // lists it (its history isn't news)
    saveKnown();
    const Tracked *now = find(convIdFor(sessionId));
    return now ? now->ref : kNoConv;
}

// ── The team ────────────────────────────────────────────────────────────────

std::vector<Backend::AgentRole> Backend::agentRoles() const {
    std::vector<AgentRole> out;
    for (const Role &r : _team.listed()) {
        AgentRole a;
        a.id          = r.id;
        a.name        = r.name;
        a.description = r.description;
        a.avatar      = r.avatar;
        a.glyph       = r.glyph;
        a.color       = r.color;
        a.prompt      = r.prompt;
        a.user        = _store.findUser(
            r.id == kGeneralist ? std::string(kAgentUser) : std::string(kRoleUserPrefix) + r.id
        );
        a.builtIn = r.builtIn;
        a.edited  = r.edited;
        out.push_back(std::move(a));
    }
    return out;
}

std::string Backend::saveAgentRole(const AgentRole &a, std::string *error) {
    Role r;
    if (const Role *old = _team.find(a.id))
        r = *old;
    r.id          = a.id;
    r.name        = a.name;
    r.description = a.description;
    r.glyph       = a.glyph;
    r.color       = a.color;
    r.prompt      = a.prompt; // new sessions only: a started one keeps the prompt it began with
    const std::string id = _team.save(r, error);
    if (!id.empty())
        teamChanged(id);
    return id;
}

void Backend::removeAgentRole(const std::string &id) {
    if (_team.remove(id))
        teamChanged(id); // its sessions keep its name and picture
}

void Backend::restoreAgentRole(const std::string &id) {
    if (_team.restore(id))
        teamChanged(id);
}

void Backend::teamChanged(const std::string &id) {
    // The teammate's name and picture, on its messages and its sessions.
    putUser(teammateUser(_team.resolve(id)));
    for (auto &[cid, tp] : _sessions)
        if (!asThread(*tp) && roleOf(*tp) == id && _firstScanDone)
            syncMeta(*tp);
}

// ── Your profile ────────────────────────────────────────────────────────────
// Claude Code has no idea who you are beyond the login: the name and picture
// msga shows for you live in its own data (profile.json + a copied image).

void Backend::loadProfile() {
    json::Document doc;
    if (!doc.parseFile(profilePath()))
        return;
    _myName       = doc.root()["name"].str();
    _myAvatarPath = doc.root()["avatar"].str();
    // Earlier versions kept a file:// URL.
    if (str::startsWith(_myAvatarPath, "file://"))
        _myAvatarPath = file::fromFileUrl(_myAvatarPath);
}

void Backend::saveProfile() const {
    json::Writer w;
    w.beginObject();
    w.key("name").value(_myName);
    w.key("avatar").value(_myAvatarPath);
    w.endObject();
    if (!dirs().data.empty())
        file::writeAtomic(profilePath(), w.str(), 0600);
}

void Backend::loadMyProfile(std::function<void(MyProfile)> done) {
    const model::User u = meUser();
    MyProfile         p;
    p.displayName = u.displayName;
    p.realName    = u.displayName;
    p.avatar      = u.avatar;
    post([done = std::move(done), p] {
        if (done)
            done(p);
    });
}

void Backend::updateProfile(std::string name, std::string, std::string, Done done) {
    // Cleared = back to the login name.
    const std::string n(str::trim(name));
    _myName = n == loginName() ? std::string() : n;
    saveProfile();
    putUser(meUser());
    postDone(std::move(done), true);
}

void Backend::setPhoto(std::string path, Done done) {
    // A fresh file name per change: images are cached by path.
    const std::string ext = str::asciiLower(file::extension(path));
    const std::string copy =
        dirs().data + "/avatar-" + str::number(nowMs()) + (ext.empty() ? std::string() : "." + ext);
    if (dirs().data.empty() || !file::copy(path, copy)) {
        postDone(std::move(done), false, tr("Couldn't copy the picture."));
        return;
    }
    if (!_myAvatarPath.empty() && str::startsWith(_myAvatarPath, dirs().data))
        file::remove(_myAvatarPath);
    _myAvatarPath = copy;
    saveProfile();
    putUser(meUser());
    postDone(std::move(done), true);
}

void Backend::postDone(Done done, bool ok, std::string error) {
    post([done = std::move(done), ok, error = std::move(error)] {
        if (done)
            done(ok, error);
    });
}

// ── Commands ────────────────────────────────────────────────────────────────

Backend::CommandList *Backend::commandList(ConvRef conv) {
    // Asked again after a while: skills and commands come and go. A failed
    // ask (Claude Code slow to start, or no answer) is retried sooner.
    constexpr int64_t kFreshMs = 5 * 60'000;
    constexpr int64_t kRetryMs = 10'000;
    const Tracked    *t        = findRef(conv);
    if (!t || t->info.cwd.empty())
        return nullptr;
    const std::string cwd  = t->info.cwd;
    CommandList      &list = _commands[cwd];
    if (!list.rev)
        list.rev = ++_commandsRev;
    if (!_creds.claudePath.empty() && !list.loading && nowMs() >= list.fetchedMs) {
        list.loading = true;
        _launcher->listCommands(
            cwd, [this, alive = _alive, cwd](std::vector<SlashCommand> commands, Account account) {
                if (!*alive)
                    return;
                CommandList &l = _commands[cwd];
                l.loading      = false;
                l.fetchedMs    = nowMs() + (commands.empty() ? kRetryMs : kFreshMs);
                if (!account.empty())
                    _account = account;
                if (commands.empty())
                    return; // keep what we had
                l.commands = std::move(commands);
                l.rev      = ++_commandsRev;
                // Typed "/co" while it was being asked: the list shows it now.
                if (onCommandsChanged)
                    onCommandsChanged();
            }
        );
    }
    return &list;
}

uint64_t Backend::commandsRevision(ConvRef conv) {
    const CommandList *list = commandList(conv);
    return list ? list->rev : 0;
}

std::vector<Backend::Command> Backend::commands(ConvRef conv) {
    const CommandList *list = commandList(conv);
    if (!list)
        return {};
    std::vector<Command> out;
    // msga's own: Claude Code's /btw is a terminal panel, not something it
    // can run for msga — here a side question opens a thread (a branch of the
    // session).
    out.push_back(
        {"btw",
         tr("Ask a side question in a thread; the session itself isn't touched"),
         tr("<question>"),
         false,
         "msga"}
    );
    // …and /status, a terminal panel too: msga shows what it knows in a dialog.
    out.push_back(
        {"status",
         tr("Show the session's Claude Code version, model, account and folder"),
         {},
         true,
         "msga"}
    );
    // /clear starts over: in the terminal the same window goes on with a new
    // session; here that is a new session chat in the same folder.
    out.push_back(
        {"clear",
         tr("Start a new session in the same folder; this one stays as it is"),
         {},
         true,
         "msga"}
    );
    for (const auto &c : list->commands)
        if (c.name != "btw" && c.name != "status" && c.name != "clear")
            out.push_back({c.name, c.desc, c.usage, c.local, c.source}); // labelled by the launcher
    return out;
}

model::Backend::LocalResult
Backend::runLocalCommand(ConvRef conv, Ts, const std::string &name, const std::string &) {
    LocalResult r;
    Tracked    *t = findRef(conv);
    if (!t)
        return r;
    if (name == "status") {
        r.status = conversationStatus(*t);
    } else if (name == "clear") {
        r.error = cannotStartIn(t->info.cwd);
        if (r.error.empty()) {
            // Same "agent type": a session started without permission checks
            // (bypassPermissions in its transcript) gets a successor like it.
            tail(*t);
            const bool skip =
                t->skipPermissionChecks || t->parser.permissionMode() == "bypassPermissions";
            r.open = createSession(t->info.cwd, skip, roleOf(*t)).ref;
        }
    }
    return r;
}

std::vector<std::pair<std::string, std::string>> Backend::conversationStatus(Tracked &t) {
    std::vector<std::pair<std::string, std::string>> rows;
    tail(t);
    const auto add = [&rows](const char *label, std::string value) {
        if (!value.empty())
            rows.emplace_back(tr(label), std::move(value));
    };
    add(N_("Version"), t.parser.version());
    add(N_("Session name"), shownTitle(t));
    const Role &mate = roleFor(t);
    add(N_("Teammate"),
        mate.removed || mate.former ? i18n::arg(tr("%1 (no longer on the team)"), mate.name)
                                    : mate.name);
    add(N_("Session ID"), t.info.sessionId);
    std::string kind;
    if (t.info.kind == SessionInfo::Kind::Background)
        kind = tr("Background");
    else if (!t.info.running)
        kind = tr("Ended");
    else if (t.info.entrypoint == "cli")
        kind = tr("Interactive, in a terminal");
    else
        kind = tr("Driven by another program");
    add(N_("Session kind"), kind);
    if (t.info.running && !t.info.peerSocket.empty())
        add(N_("Peer address"), "uds:" + t.info.peerSocket);
    add(N_("Folder"), homeRelative(t.info.cwd));
    add(N_("Login method"),
        _account.subscriptionType.empty() ? _account.apiProvider
                                          : i18n::arg(tr("%1 account"), _account.subscriptionType));
    add(N_("Organization"), _account.organization);
    add(N_("Email"), _account.email);
    add(N_("Model"), t.parser.model());
    add(N_("Permission mode"), t.parser.permissionMode());
    return rows;
}

// ── Prompt history ──────────────────────────────────────────────────────────

std::vector<std::string> Backend::promptHistory(ConvRef conv) {
    const Tracked *t = findRef(conv);
    if (!t || t->info.cwd.empty())
        return {};
    return loadedHistory(t->info.cwd, t->info.sessionId);
}

std::vector<std::string> Backend::folderPromptHistory(const std::string &dir) {
    if (dir.empty())
        return {};
    return loadedHistory(dir, {});
}

// What is read of the folder's history already (the UI thread never reads
// all of history.jsonl, nor waits for a worker that does); none yet: it is
// read ahead now, for the next ↑.
std::vector<std::string>
Backend::loadedHistory(const std::string &dir, const std::string &sessionId) {
    std::vector<std::string> out;
    if (!claude::loadedPromptHistory(
            _paths.home + "/history.jsonl", _paths.home + "/paste-cache", dir, sessionId, &out
        ))
        readAheadHistory(dir);
    return out;
}

// ── Deleting ────────────────────────────────────────────────────────────────

// A prompt or an answer in a session of its own that nothing is writing to:
// Claude Code reads the transcript back when the session goes on, so what's
// taken out of it is gone for Claude too. Not while the session is working or
// driven from a terminal — its running process keeps the message — and not
// the history a branched session shares with this one (the thread is found
// by it).
const TranscriptItem *Backend::deletableItem(Tracked &t, Ts ts) {
    const bool drivenElsewhere = t.info.running && t.info.kind == SessionInfo::Kind::Interactive;
    if (t.parsing || asThread(t) || t.info.sessionId.empty() || drivenElsewhere || busy(t) ||
        awaitsApproval(t.info) || !t.outbox.empty() ||
        (t.info.running && statusHasShell(t.info.status)))
        return nullptr;
    tail(t);
    const auto &items = t.parser.items();
    const auto  it    = std::find_if(items.begin(), items.end(), [&](const TranscriptItem &i) {
        return i.ts == ts;
    });
    if (it == items.end() || it->uuid.empty() || t.transcriptPath.empty())
        return nullptr;
    const size_t index = size_t(it - items.begin());
    for (auto &[id, op] : _sessions) {
        Tracked &other = *op;
        if (&other != &t && (other.forkOf == t.convId || t.forkOf == other.convId) &&
            index < sharedStart(items, other.parser.items()))
            return nullptr;
    }
    return &*it;
}

// A message still waiting for its turn can be taken back: it's only msga's.
Backend::Tracked *Backend::queuedHolder(const std::string &convId, Ts ts, size_t *index) {
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if ((asThread(t) ? t.forkOf : t.convId) != convId)
            continue;
        for (size_t i = 0; i < t.outbox.size(); ++i)
            if (t.outbox[i].ts == ts) {
                *index = i;
                return &t;
            }
    }
    return nullptr;
}

bool Backend::canDeleteMessage(ConvRef conv, Ts ts) const {
    auto          *self = const_cast<Backend *>(this);
    const Tracked *t    = findRef(conv);
    if (!t)
        return false;
    size_t index = 0;
    if (self->queuedHolder(t->convId, ts, &index))
        return true;
    return self->deletableItem(*self->findRef(conv), ts) != nullptr;
}

void Backend::remove(ConvRef conv, Ts ts) {
    Tracked *t = findRef(conv);
    if (!t)
        return;
    size_t index = 0;
    if (Tracked *q = queuedHolder(t->convId, ts, &index)) {
        q->outbox.erase(q->outbox.begin() + ptrdiff_t(index));
        sync(*q);
        return;
    }
    const TranscriptItem *item = deletableItem(*t, ts);
    if (!item) {
        LOG_WARN("claude", "%s can't be deleted now", model::formatTs(ts).c_str());
        return;
    }
    // Rewritten, then read afresh (it was rewritten, not appended to), on a
    // worker: the whole transcript goes through both. Nothing is read here
    // meanwhile.
    struct Job {
        std::string      convId, path, uuid, error;
        bool             ok = false;
        TranscriptParser parser;
        int64_t          offset = -1;
    };
    auto job    = std::make_shared<Job>();
    job->convId = t->convId;
    job->path   = t->transcriptPath;
    job->uuid   = item->uuid;
    t->parsing  = true;
    model::runInBackground(
        _app,
        [job] {
            job->ok = removeFromTranscript(job->path, job->uuid, &job->error);
            if (job->ok)
                job->offset = feedTranscript(job->path, job->parser);
        },
        [this, alive = _alive, job] {
            if (!*alive)
                return;
            Tracked *t = find(job->convId);
            if (!t)
                return;
            t->parsing = false;
            if (!job->ok) {
                LOG_WARN("claude", "delete failed: %s", job->error.c_str());
                reportError(job->error);
            } else {
                t->restart(std::move(job->parser), std::max<int64_t>(job->offset, 0));
            }
            tail(*t);
            sync(*t);
            syncMeta(*t);
            for (auto &then : std::exchange(t->afterParse, {}))
                then();
        }
    );
}

// ── Reactions ───────────────────────────────────────────────────────────────
// Only msga has them: nothing goes to Claude or into the transcript.

void Backend::react(ConvRef conv, Ts ts, std::string_view name, bool add) {
    const Tracked *t = findRef(conv);
    if (!t)
        return;
    auto      &reactions = _reactions[t->convId][ts];
    const auto me        = _store.me;
    auto       r = std::find_if(reactions.begin(), reactions.end(), [&](const model::Reaction &x) {
        return x.name == name;
    });
    if (add) {
        if (r == reactions.end())
            reactions.push_back(model::Reaction{std::string(name), 1, {me}});
        else if (std::find(r->users.begin(), r->users.end(), me) == r->users.end()) {
            r->users.push_back(me);
            r->count = uint32_t(r->users.size());
        } else
            return;
    } else {
        if (r == reactions.end() || !std::erase(r->users, me))
            return;
        r->count = uint32_t(r->users.size());
        if (r->count == 0)
            reactions.erase(r);
    }
    // What the Store shows takes it at once; the next sync then has no news.
    _store.setReaction(conv, ts, name, me, add);
    for (auto &[key, shown] : _shown)
        if (key.first == conv)
            if (const auto f = shown.fp.find(ts); f != shown.fp.end())
                if (const Message *m = _store.findMessage(conv, ts))
                    f->second = fingerprint(*m);
}

// ── Search ──────────────────────────────────────────────────────────────────

void Backend::searchTexts(Tracked &t, std::vector<SearchText> &out) {
    const bool thread = asThread(t);
    tail(t);
    if (thread && t.forkAt < 0)
        return;
    const auto &items  = t.parser.items();
    const bool  isBusy = busy(t);
    // Item i's text as renderedAt makes it — the one made already, if any.
    const auto  add    = [&out](Tracked &s, size_t i, Ts threadTs) {
        SearchText x;
        x.ts     = s.parser.items()[i].ts;
        x.thread = threadTs;
        if (i < s.rendered.size() && s.rendered[i].rev == s.parser.revision(i))
            x.text = s.rendered[i].msg.text;
        else if (int(i) == s.forkAt && !s.branchLabel.empty())
            x.text = escapeMrkdwn(s.branchLabel);
        else
            x.text = messageSource(s.parser.items()[i], &x.markdown);
        out.push_back(std::move(x));
    };
    const auto outgoing = [&out](const Tracked &s, Ts threadRoot) {
        const auto one = [&](const Tracked::Outgoing &o) {
            out.push_back(
                {o.ts,
                 o.threadRoot ? o.threadRoot : threadRoot,
                 takeAttachments(o.shown.empty() ? o.text : o.shown, nullptr),
                 true}
            );
        };
        if (s.flying)
            one(*s.flying);
        for (const auto &o : s.outbox)
            one(o);
    };
    const auto shown = [&](const TranscriptItem &item) {
        return isVisible(item, isBusy) && !(_zen && item.kind == TranscriptItem::Kind::ToolGroup);
    };
    if (thread) { // as threadList
        for (size_t i = size_t(t.forkAt) + 1; i < items.size(); ++i)
            if (shown(items[i]))
                add(t, i, t.forkRoot);
        outgoing(t, t.forkRoot);
        return;
    }
    // As visibleList: hand-backs point to their subagent's thread, replies
    // relayed to a subagent are in its thread.
    std::unordered_map<std::string, size_t> subagentItem; // by agentId
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == TranscriptItem::Kind::Subagent && !items[i].agentId.empty())
            subagentItem[items[i].agentId] = i;
    const UserRef assistant = roleUser(roleOf(t));
    Ts            last      = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!shown(item))
            continue;
        last = item.ts;
        if (item.kind == TranscriptItem::Kind::PeerMessage)
            if (const auto r = subagentItem.find(item.agentId); r != subagentItem.end()) {
                out.push_back(
                    {item.ts, 0, handbackPointer(t, items[r->second], item.ts, assistant).text}
                );
                continue;
            }
        Ts threadTs = 0;
        if (!item.relayTo.empty())
            if (const auto r = subagentItem.find(item.relayTo); r != subagentItem.end())
                threadTs = items[r->second].ts;
        add(t, i, threadTs);
    }
    if (const Ts waitTs = waitingTs(t, last))
        out.push_back({waitTs, 0, waitingMessage(t, waitTs).text});
    outgoing(t, 0);
    for (auto &[id, fp] : _sessions) { // the /btw threads' roots
        Tracked &f = *fp;
        if (f.forkOf != t.convId || !asThread(f))
            continue;
        tail(f);
        if (f.forkAt < 0)
            continue;
        add(f, size_t(f.forkAt), 0);
    }
}

void Backend::search(std::string query, std::function<void(std::vector<SearchHit>)> done) {
    // What every session shows is gathered here, unrendered; its Markdown is
    // rendered (as the lists render it) and matched on a worker.
    struct Session {
        ConvRef                 conv = kNoConv;
        std::vector<SearchText> texts;
    };
    struct Job {
        std::string            q;
        std::vector<Session>   sessions;
        std::vector<SearchHit> hits;
    };
    auto job = std::make_shared<Job>();
    job->q   = str::asciiLower(str::trim(query));
    if (!job->q.empty())
        for (auto &[id, tp] : _sessions) {
            Tracked       &t     = *tp;
            const Tracked *owner = asThread(t) ? find(t.forkOf) : &t;
            if (!owner || owner->ref == kNoConv)
                continue;
            job->sessions.push_back({owner->ref, {}});
            searchTexts(t, job->sessions.back().texts);
        }
    model::runInBackground(
        _app,
        [job] {
            std::vector<std::pair<Ts, SearchHit>> hits;
            for (Session &s : job->sessions) {
                if (hits.size() >= 200)
                    break;
                for (SearchText &x : s.texts) {
                    if (x.markdown)
                        x.text = renderMarkdown(x.text);
                    if (str::asciiLower(x.text).find(job->q) == std::string::npos)
                        continue;
                    SearchHit h;
                    h.conv   = s.conv;
                    h.ts     = x.ts;
                    h.thread = x.thread;
                    h.text   = std::move(x.text);
                    hits.emplace_back(x.ts, std::move(h));
                }
            }
            std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
                return a.first > b.first;
            });
            for (auto &[ts, h] : hits)
                job->hits.push_back(std::move(h));
        },
        [alive = _alive, job, done = std::move(done)] {
            if (*alive && done)
                done(std::move(job->hits));
        }
    );
}

} // namespace claude
