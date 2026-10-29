// Claude Code as a messaging backend: every Claude Code session on this
// machine is a DM with its own assistant user, so each session gets its own
// status dot, unread state and notifications.
//
//   • Roster: ~/.claude/sessions/*.json (interactive sessions and background
//     workers) and ~/.claude/jobs/*/state.json (background sessions), watched,
//     plus the ended sessions msga has seen, kept until Claude Code drops their
//     transcript (known-sessions.json in msga's data).
//   • Messages: the session transcripts, tailed while a session is live, and
//     written into the Store: a conversation's list once its history was
//     loaded, its unread counts always.
//   • Writing: a session a terminal or another program drives is read-only.
//     Everything msga sends goes through BACKGROUND sessions (Launcher), which
//     Claude Code's daemon runs — they survive msga crashing or quitting. A
//     message sent while Claude is still on a turn waits for the turn to end,
//     or is typed into the live worker (`claude attach`).
//
// The implementation is split by concern (one class, several files):
//   backend.cpp         state, roster scans, transcripts → Store, typing,
//                       history and threads, lifecycle
//   backend_send.cpp    sending: the outbox, launches, typing live, /btw
//                       branches, permission questions, Stop
//   backend_manage.cpp  "Remove from msga" (and the worktrees of sessions msga
//                       started), finding sessions, the team, your profile,
//                       commands, prompt history, deleting, reactions, search
//   backend_branch.cpp  agent branches: forks of a session run with tools
//                       off, their turns followed for others (thread links)
//
// Nothing runs until this backend is constructed, i.e. until a Claude Code
// workspace exists.
#pragma once

#include "app/claude/cli.h"
#include "app/claude/launcher.h"
#include "app/claude/outputs.h"
#include "app/claude/roles.h"
#include "app/claude/roster.h"
#include "app/claude/transcript.h"
#include "app/model/backend.h"
#include "app/model/timers.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace plat {
class App;
}

namespace claude {

class AttachInput;
class AttachAnswer;
struct WorktreeRef;

// A message as the composer handed it over (its text and files as given):
// Backend::onUnsent gets it back should it never go out.
struct Unsent {
    model::ConvRef           conv   = model::kNoConv;
    model::Ts                thread = 0;
    std::string              text;
    std::vector<std::string> files;
};

class Backend final : public model::Backend {
public:
    Backend(model::Store &store, plat::App &app, Credentials creds);
    ~Backend() override; // stops its timers; no callback runs afterwards

    // The conversation open when the workspace was last shown (kNoConv).
    model::ConvRef lastConversation() const;
    // Saves what it knows and stops looking: before the Store is cleared
    // (switch, sign-out) — nothing is written into it afterwards.
    void           close();

    // ── model::Backend: reading (backend.cpp) ───────────────────────────────
    void           connect(Done done) override;
    void           loadHistory(model::ConvRef conv, model::Ts before, Done done) override;
    void           loadThread(model::ConvRef conv, model::Ts root, Done done) override;
    void           setActiveConversation(model::ConvRef conv, model::Ts thread) override;
    Capabilities   capabilities() const override;
    bool           isAgentSession(model::ConvRef conv) const override;
    std::string    promptSuggestion(model::ConvRef conv) const override;
    void           markRead(model::ConvRef conv, model::Ts ts) override;
    void           markUnread(model::ConvRef conv, model::Ts ts) override;
    void           setStarred(model::ConvRef conv, bool starred) override;
    void           setMuted(model::ConvRef conv, bool muted) override;
    void           setNotifyLevel(model::ConvRef conv, model::NotifyLevel level) override;
    void           setLocalName(model::ConvRef conv, std::string name) override;
    void           setZenMode(bool on) override;
    bool           threadAcceptsReplies(model::ConvRef conv, model::Ts root) const override;
    bool           threadOpensAsSession(model::ConvRef conv, model::Ts root) const override;
    bool           threadFollowed(model::ConvRef conv, model::Ts root) const override;
    model::ConvRef openThreadAsSession(model::ConvRef conv, model::Ts root) override;
    std::string    agentSessionFolder(model::ConvRef conv) const override;
    // The teammate's role id: the transcript's record, else the one msga
    // started it with, else the Generalist ("" when it's no session).
    std::string    agentSessionRole(model::ConvRef conv) const override;

    // ── Sending (backend_send.cpp) ──────────────────────────────────────────
    void send(model::ConvRef conv, std::string text, model::Ts threadTs, Done done) override;
    // Files go with the prompt as @mentions of copies in msga's cache: Claude
    // Code attaches them itself.
    void sendWithFiles(
        model::ConvRef           conv,
        std::string              text,
        model::Ts                threadTs,
        std::vector<std::string> files,
        Done                     done
    ) override;
    // "Stop": a background session's worker is stopped (`claude stop`) with
    // all it runs, mid-turn, waiting on an approval or idle. Sessions a
    // terminal or another program drives are theirs to stop.
    bool canStopSession(model::ConvRef conv) const override;
    void stopSession(model::ConvRef conv) override;
    // The buttons on "Waiting for your approval": each one is an option of
    // the permission question on Claude Code's screen, picked there.
    void
    pressButton(model::ConvRef conv, model::Ts ts, const std::string &buttonId, Done done) override;
    std::string agentSessionBlocker(const std::string &dir) const override;
    void        startAgentSession(
        const std::string                                       &dir,
        bool                                                     skipPermissionChecks,
        const std::string                                       &role,
        std::function<void(model::ConvRef, const std::string &)> done
    ) override;

    // ── Agent branches (backend_branch.cpp) ─────────────────────────────────
    // Forks of a session (--fork-session) with tools turned off, shown as its
    // /btw threads are; each turn followed from the branch's transcript.
    void startAgentBranch(
        const std::string       &session,
        std::string              prompt,
        std::vector<std::string> files,
        std::vector<std::string> deniedTools,
        AgentBranchDone          started,
        AgentTurnFn              turn
    ) override;
    void continueAgentBranch(
        const std::string       &branch,
        std::string              prompt,
        std::vector<std::string> files,
        AgentTurnFn              turn
    ) override;
    void watchAgentBranch(const std::string &branch, AgentTurnFn turn) override;
    void agentSessionMovedOn(
        const std::string &session, const std::string &forkPoint, std::function<void(bool)> done
    ) override;
    void setAgentBranchLabel(const std::string &branch, std::string label) override;

    // ── Managing (backend_manage.cpp) ───────────────────────────────────────
    // "Remove from msga": hides the session here — Claude Code keeps it — and
    // deletes it (with its worktrees) if msga started it. It comes back if it
    // gets new activity after that (resumed elsewhere).
    void leave(model::ConvRef conv) override;
    // Takes a prompt or an answer out of the session's transcript, so Claude
    // doesn't have it either when the session goes on.
    void remove(model::ConvRef conv, model::Ts ts) override;
    bool canDeleteMessage(model::ConvRef conv, model::Ts ts) const override;
    // Your reactions, for you alone: Claude never hears of them, and they're
    // kept in memory only — gone when msga quits.
    void react(model::ConvRef conv, model::Ts ts, std::string_view name, bool add) override;
    void search(std::string query, std::function<void(std::vector<SearchHit>)> done) override;
    std::vector<AgentRole> agentRoles() const override;
    std::string            saveAgentRole(const AgentRole &role, std::string *error) override;
    void                   removeAgentRole(const std::string &id) override;
    void                   restoreAgentRole(const std::string &id) override;
    void           findAgentSessions(std::function<void(std::vector<FoundSession>)> done) override;
    model::ConvRef addFoundSession(const std::string &id) override;
    std::vector<Command> commands(model::ConvRef conv) override;
    uint64_t             commandsRevision(model::ConvRef conv) override;
    LocalResult          runLocalCommand(
        model::ConvRef conv, model::Ts thread, const std::string &name, const std::string &args
    ) override;
    std::vector<std::string> promptHistory(model::ConvRef conv) override;
    std::vector<std::string> folderPromptHistory(const std::string &dir) override;
    void                     loadMyProfile(std::function<void(MyProfile)> done) override;
    void updateProfile(std::string name, std::string email, std::string phone, Done done) override;
    void setPhoto(std::string path, Done done) override;

    // What the reading side did so far, for tests and profiling: transcript
    // bytes read on the UI thread (sessions' and subagents'; not those read
    // on a worker), items rendered into messages, messages copied out of the
    // render cache (into the Store or a page), and sessions synced.
    struct Counters {
        uint64_t bytesRead = 0, renders = 0, copies = 0, syncs = 0;
    };
    const Counters &counters() const { return _counters; }

    // Each part keeps its private helpers on this struct (one per session).
    struct Tracked;
    struct Cleanup;
    struct Rendered;
    struct Visible;
    struct SubagentFeed;

private:
    // ── backend.cpp ─────────────────────────────────────────────────────────
    void                       loadKnown();
    void                       saveKnown();
    void                       scheduleSaveKnown();
    void                       sectionsChanged() override; // kept with the known sessions
    Tracked                   &ensureTracked(const std::string &convId);
    Tracked                   *find(std::string_view convId);
    const Tracked             *find(std::string_view convId) const;
    Tracked                   *findRef(model::ConvRef conv);
    const Tracked             *findRef(model::ConvRef conv) const;
    std::string                convIdFor(const std::string &sessionId) const;
    void                       tail(Tracked &t);
    // A whole transcript (re)read from its start: on a worker (parseOnWorker,
    // when it's big), the session read nothing meanwhile (Tracked::parsing).
    void                       parseOnWorker(Tracked &t);
    // `then` once the session's transcript is read (now, when it isn't being
    // read on a worker).
    void                       whenParsed(Tracked &t, std::function<void()> then);
    // What sync() made of the session last time (refreshScan skips a session
    // whose key is the same).
    uint64_t                   syncKey(Tracked &t);
    // The indexes of the session's Subagent items with an agent (cached).
    const std::vector<size_t> &subagentItems(Tracked &t);
    // The transcript of a session that has none yet, looked for at most once
    // per refresh while it isn't there — while nothing runs the session, once
    // in a while ("" = none).
    std::string                lookForTranscript(const Tracked &t);
    void                       firstScan(Done done); // connect(): transcripts parsed on a worker
    void                       refresh();
    void                       refreshScan(); // refresh()'s body
    void                       scheduleRefresh();
    void                       watchTick();
    void                       watchCheck(); // the look: what changed since the last
    void                       syncWatches();
    void                       readWatchEvents();
    std::vector<std::string>   watchedFiles(bool *anyBusy) const;
    // `looked`: findSubagentRuns has just run (the refresh's own look).
    void                       pumpTyping(bool looked = false);
    void                       announceRoles();
    // Puts what the session shows into the Store: its messages (where its
    // history was loaded), its conversation and its user.
    void                       sync(Tracked &t);
    void                       syncMeta(Tracked &t);
    void                       syncThreads(Tracked &t);
    void           syncList(model::ConvRef conv, model::Ts root, const std::vector<Visible> &want);
    void           syncUsers();
    // Puts a user into the Store and tells its observers (Store::addUser
    // alone fires nothing: a dot or a name would change unseen). Inside a
    // roster refresh the users change once, at its end.
    void           putUser(model::User u);
    model::UserRef userRef(const std::string &id);
    model::UserRef roleUser(const std::string &role);
    model::UserRef sessionUser(const Tracked &t);
    model::User    meUser() const;
    model::User    assistantUser(const Tracked &t) const;
    model::User    teammateUser(const Role &r) const;

    bool               busy(const Tracked &t) const;
    bool               working(const Tracked &t) const; // busy, msga's own turn aside
    bool               needsUser(const Tracked &t) const;
    bool               unavailable(const Tracked &t) const; // the yellow dot
    std::string        readOnlyReason(const Tracked &t) const;
    bool               readOnly(const Tracked &t) const; // readOnlyReason isn't ""
    const std::string &roleOf(const Tracked &t) const;   // its teammate's role id
    const Role        &roleFor(const Tracked &t) const;  // …and the teammate, as shown
    model::UserRef     subagentAuthor(const TranscriptItem &item, model::UserRef parent);
    // What a PeerMessage of `subagent` handing back (at `ts`) shows as in the
    // session: a line pointing to that subagent's thread.
    model::Message     handbackPointer(
        const Tracked &t, const TranscriptItem &subagent, model::Ts ts, model::UserRef parent
    );
    bool                     roleBusy(const std::string &role) const;
    bool                     roleSubagentRunning(const std::string &role) const;
    bool                     roleUnavailable(const std::string &role) const;
    std::vector<std::string> roleIds() const;
    std::string              titleOf(const Tracked &t) const;    // Claude Code's name for it
    std::string              shownTitle(const Tracked &t) const; // the user's name, else titleOf
    std::string              teammateNames(std::string_view text) const;

    // Branched sessions (/btw threads).
    std::unordered_set<std::string> detectForks();      // parents whose threads changed
    int64_t        bornMicros(const std::string &path); // a transcript's birth (cached)
    bool           asThread(const Tracked &t) const;
    Tracked       *forkFor(const std::string &parentConv, model::Ts root);
    const Tracked *forkFor(const std::string &parentConv, model::Ts root) const;
    std::string    subagentOf(const Tracked &t, model::Ts root) const;

    // What a session shows, rendered (each message as a Visible recipe:
    // make() builds it, `fp` tells it changed).
    const Rendered            &renderedAt(Tracked &t, size_t i);
    // Items before `renderFrom` (a ts; replies relayed to subagents aside) are
    // listed unrendered — enough for what sync() takes of them; realize()
    // renders one. kRenderAll / kRenderNone: every item, or none.
    static constexpr model::Ts kRenderAll  = 0;
    static constexpr model::Ts kRenderNone = INT64_MAX;
    std::vector<Visible>       visibleList(Tracked &t, model::Ts renderFrom = kRenderAll);
    void                       realize(Tracked &t, Visible &v);
    std::vector<Visible>       threadList(Tracked &fork);
    // …a subagent thread: the replies `shown` (visibleList(t)) relays to it,
    // then the subagent's own transcript.
    std::vector<Visible>
                   subagentList(Tracked &t, model::Ts root, const std::vector<Visible> &shown);
    model::Message make(const Visible &v) const;
    // A Visible over `m` (its own), or over a rendered message.
    static Visible ownVisible(model::Message m);
    static Visible renderedVisible(const std::vector<Rendered> &src, size_t index);
    // A terminal session stopped at its prompt for the user.
    bool           terminalWaits(const Tracked &t) const;
    // The ts of the session's "Waiting for you…" message, 0 when it shows none.
    model::Ts      waitingTs(const Tracked &t, model::Ts lastTs) const;
    model::Message waitingMessage(const Tracked &t, model::Ts ts);
    // The newest ts the session's list (a /btw branch: its thread) shows;
    // 0 when it's empty. Nothing rendered.
    model::Ts      lastShownTs(Tracked &t);
    // The message at ts in a list the Store doesn't hold, built for its
    // notification alone (no output files); false when nothing shows there.
    bool           newsAt(Tracked &t, bool thread, model::Ts ts, model::Message *out);
    // A message as sync() takes it: whose and what kind, its thread.
    struct Seen {
        bool      mine = false, progress = false;
        model::Ts thread = 0;
    };
    // What visibleList / threadList show, without rendering any of
    // it — for the lists the Store doesn't hold (sync).
    std::map<model::Ts, Seen> seenOf(Tracked &t, bool thread);
    void appendOutgoing(const Tracked &t, std::vector<Visible> &out, model::Ts threadRoot) const;
    bool isOutgoingCopy(const Tracked &t, model::Ts ts) const;
    // An answer's output files (outputs.h): the copies made before, else
    // made on a worker — the answer is rendered again once they are.
    void attachOutputs(
        model::Message                    &m,
        const std::vector<TranscriptItem> &items,
        size_t                             i,
        const std::string                 &convId,
        const std::string                 &cwd,
        const std::string                 &agentId = {}
    );
    // The answers waiting for their files to be looked for (attachOutputs),
    // looked for in one job.
    void flushOutputs();
    void outputsMade(
        const std::string &convId, const std::string &agentId, const std::vector<std::string> &keys
    );
    // Reactions on, and each message's fingerprint taken (Visible::fp).
    void          applyReactions(const std::string &convId, std::vector<Visible> &list) const;
    // A subagent's transcript, read up to its end (only what was appended) —
    // a long one first read on a worker: unread until then.
    SubagentFeed &subagentFeed(const Tracked &t, const std::string &agentId);
    void          parseSubagentOnWorker(const std::string &path, const Tracked &t, SubagentFeed &f);
    // Subagent transcripts read on workers are in: their sessions synced again.
    void          subagentsRead();
    // What msga keeps of a session it no longer tracks: its subagents'
    // transcripts, the answers known to have no output files.
    void          forgetCaches(Tracked &t);
    // The background subagents running now (pumpTyping, roleSubagentRunning):
    // looked for once per tick.
    void          findSubagentRuns();
    int     subagentReplyCount(const Tracked &t, const std::string &agentId, model::Ts *latest);
    // When the subagent's run under way began (epoch ms); 0 = it isn't running.
    int64_t subagentRunSinceMs(const Tracked &t, const std::string &agentId);
    // The folder's prompt history (↑) read on a worker, one read per folder
    // at a time (_historyReads).
    void    readAheadHistory(const std::string &dir);

    // ── backend_send.cpp ────────────────────────────────────────────────────
    void
    sendText(model::ConvRef conv, std::string text, model::Ts threadTs, Done done, Unsent back);
    void        sendNext(model::ConvRef conv);
    void        failSends(Tracked &t, const std::string &reason);
    void        giveBack(Unsent back); // to onUnsent, if anything was in it
    bool        loginKnownGood() const;
    void        whenLoggedIn(std::function<void(bool loggedIn)> then);
    bool        typesLive(const Tracked &t) const;
    void        typeLive(Tracked &t);
    void        dispatch(Tracked &t);
    void        readApproval(Tracked &t);
    void        stopWorker(Tracked &t);
    void        adoptCopy(Tracked &t, const std::string &copyId);
    void        settleTurn(Tracked &t); // msga's turn: ended, or given up on
    std::string cannotStartIn(const std::string &dir) const;
    Tracked &
    createSession(const std::string &dir, bool skipPermissionChecks, const std::string &role);
    // A branch of the session (a /btw, or an agent branch): `opts` says
    // which tools it goes without and who follows its first turn. Its
    // `started`, given, takes the place of `done`.
    struct ForkOptions {
        std::vector<std::string> deniedTools;
        AgentTurnFn              turn;
        AgentBranchDone          started;
        Unsent                   back; // a /btw's, should it fail to start
    };
    void startFork(Tracked &parent, const std::string &question, Done done, ForkOptions opts = {});
    void launchFork(Tracked &parent, const std::string &question, Done done, ForkOptions opts = {});
    // Files copied into the cache on a worker (sending them): then(copies,
    // ""), or then({}, the path that couldn't be read).
    void uploadFiles(
        std::vector<std::string>                                                 files,
        std::function<void(std::vector<std::string> copies, std::string failed)> then
    );
    // msga's copy of a message to `target` (a reply relayed to a subagent of
    // it: relayRoot, shown as `shown`), queued; it goes when Claude is free.
    // `back` is what onUnsent gets should it fail.
    void enqueue(
        Tracked &target, std::string text, model::Ts relayRoot, std::string shown, Unsent back = {}
    );

    // ── backend_branch.cpp ──────────────────────────────────────────────────
    // The branch's turn followed (Tracked::watch): its phase told as it
    // changes, then its end — once per refresh, after the turn is settled.
    void watchTurn(Tracked &t);
    // The followed turn failed (or the branch is gone): told, and no more.
    void endWatch(Tracked &t, const std::string &error);
    // The branch `branchConv`'s first prompt landed: where it was forked,
    // looked for on a worker, then `started`.
    void branchStarted(const std::string &branchConv, AgentBranchDone started);

    // ── backend_manage.cpp ──────────────────────────────────────────────────
    // ↑'s history for `dir` from what is read already; the first look reads
    // it ahead (readAheadHistory) and answers none.
    std::vector<std::string> loadedHistory(const std::string &dir, const std::string &sessionId);
    struct CommandList;
    // conv's folder's slash commands, asked of Claude Code again after a
    // while; null without a folder.
    CommandList *commandList(model::ConvRef conv);

    void hideSession(const std::string &convId, const std::shared_ptr<Cleanup> &cleanup);
    void stopRemoved(const std::string &sessionId, const std::string &cwd);
    void removeOwned(
        const std::string              &sessionId,
        const std::string              &jobId,
        const std::string              &cwd,
        bool                            background,
        const std::shared_ptr<Cleanup> &cleanup
    );
    void release(const std::shared_ptr<Cleanup> &cleanup);
    // done(ok, error) later, as post().
    void postDone(Done done, bool ok, std::string error = {});
    // removeOwned, when msga started the session — or a session of msga's
    // did (looked for on a worker: that reads transcripts); else `otherwise`.
    void removeIfOwned(
        const std::string              &sessionId,
        const std::string              &jobId,
        const std::string              &cwd,
        bool                            background,
        const std::shared_ptr<Cleanup> &cleanup,
        std::function<void()>           otherwise = {}
    );
    const TranscriptItem *deletableItem(Tracked &t, model::Ts ts);
    Tracked              *queuedHolder(const std::string &convId, model::Ts ts, size_t *index);
    std::vector<std::pair<std::string, std::string>> conversationStatus(Tracked &t);
    // What search() looks through of a session: each message its list shows
    // (visibleList; a /btw branch: threadList; in any order: search sorts
    // by ts) — its text, or the Markdown its text is rendered from, rendered
    // on a worker.
    struct SearchText;
    void searchTexts(Tracked &t, std::vector<SearchText> &out);
    void teamChanged(const std::string &id);
    void loadProfile();
    void saveProfile() const;
    void reportError(const std::string &message);

    // Calls fn later on the loop unless this backend is gone by then.
    void post(std::function<void()> fn);
    // A one-shot timer that dies with the backend.
    void after(int ms, std::function<void()> fn);

    plat::App                                                   &_app;
    Credentials                                                  _creds;
    Paths                                                        _paths;
    Team                                                         _team;
    std::unique_ptr<Launcher>                                    _launcher;
    std::shared_ptr<bool>                                        _alive;
    // By conversation id — the session id, except for a session started with
    // "+": its conversation exists before Claude Code picks the session's id on
    // the first message, so it keeps its own id and _convOf maps the session to it.
    std::map<std::string, std::unique_ptr<Tracked>, std::less<>> _sessions;
    std::unordered_map<std::string, std::string> _convOf; // session id → conversation id
    // Sessions removed from msga, by session id: when, and the transcript whose
    // later growth brings the session back (forgotten once Claude Code drops it).
    struct Hidden {
        int64_t     atMs = 0;
        std::string transcript;
        int64_t     seenSize = -1;    // transcript bytes already known not to be new activity
        bool        stopping = false; // its worker is being stopped (stopRemoved)
        bool checking = false; // its transcript's growth looked through (checkHiddenOnWorker)
    };
    void checkHiddenOnWorker(
        const std::string &sessionId, const std::string &path, int64_t from, int64_t size
    );
    std::unordered_map<std::string, Hidden> _hidden;
    // Its entry, made now (with the transcript as it stands) if there's none.
    Hidden                                 &hide(const std::string &sessionId);
    // Its worker stopped: what it wrote as it exited is no new activity.
    // Returns its transcript ("" when it isn't hidden).
    std::string                             noteStopped(const std::string &sessionId);
    // Sessions msga started ("+" sessions, /btw branches), by session id —
    // Claude Code records no such thing. Closing msga leaves their workers be;
    // "Remove from msga" deletes them. Forgotten once Claude Code drops the job.
    std::unordered_set<std::string>         _launchedHere;
    // Worktrees being deleted right now (cleaned paths): never twice at once.
    std::unordered_set<std::string>         _reaping;
    std::unordered_set<std::string>         _forkingIn; // folders a /btw is being launched in
    // Sends waiting, in order, behind one whose files are being copied into
    // the cache on a worker (sendWithFiles); _copying = the conversations
    // with a copy under way.
    struct Queued {
        model::ConvRef           conv = model::kNoConv;
        std::string              text;
        model::Ts                threadTs = 0;
        std::vector<std::string> files;
        Done                     done;
    };
    std::vector<Queued>         _sendQueue;
    std::vector<model::ConvRef> _copying;

    uint64_t _debounce = 0, _watchTimer = 0, _safetyPoll = 0, _saveTimer = 0, _typingTimer = 0;
    model::OneShotTimers                                         _oneShots{_app};
    bool                                                         _started       = false;
    bool                                                         _firstScanDone = false;
    int                                                          _batchUsers    = 0;
    bool                                                         _usersDirty    = false;
    // What the watcher saw last (path → size + mtime), and when it last looked.
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> _watched;
    int64_t                                                      _watchCheckedMs = 0;
    // inotify (Linux): its descriptor (-1 = not yet, -2 = none to have), its
    // watch on the loop (0: no loop to wake, read by watchTick), the folders
    // watched and the files in them looked at; _watchAll: every one is.
    int                                                          _inotify        = -1;
    uint64_t                                                     _inotifyWatch   = 0;
    std::unordered_map<std::string, int64_t>                     _watchDirs;
    std::unordered_set<std::string>                              _watchFiles;
    bool                                                         _watchAll           = false;
    // Typing into live workers (typeLive): misses in a row, and off until when.
    int                                                          _typeLiveMisses     = 0;
    int64_t                                                      _typeLiveOffUntilMs = 0;
    // The CLI's login (whenLoggedIn): the last check's answer and when it came.
    Login                                                        _login = Login::Unknown;
    int64_t                                _loginCheckedMs = 0; // 0 = never, or to be checked again
    bool                                   _loginChecking  = false;
    std::vector<std::function<void(bool)>> _loginWaiters;
    int64_t                                _loginFailedSeen = 0; // newest parser loginFailedAt seen
    bool                                   _zen             = false;
    std::string                            _myName;       // "" = the login name
    std::string                            _myAvatarPath; // a copy in msga's data; "" = initials
    std::string                            _lastConv;     // the conversation open last
    std::vector<model::Store::Section>     _sections;     // the sidebar's own (Store::sections)
    std::unordered_map<std::string, std::unique_ptr<SubagentFeed>> _subagents; // by transcript path
    uint64_t                        _subagentParses = 0; // parseSubagentOnWorker's, numbered
    std::vector<std::string>        _subagentsReadFor; // conversations synced again (subagentsRead)
    // Answers whose output files are being copied (by their copies' folder),
    // and those known to have none.
    std::unordered_set<std::string> _outputsPending, _noOutputs;
    struct OutputsWanted {
        std::string   text, agentId, folder;
        OutputContext ctx;
    };
    std::vector<OutputsWanted> _outputsQueue; // attachOutputs → flushOutputs
    bool                       _outputsFlushPosted = false;
    // The background subagents found running (findSubagentRuns): who thinks
    // where since when, and the roles they run as.
    struct SubagentRun {
        model::ConvRef ref    = model::kNoConv;
        model::UserRef author = model::kNoUser;
        model::Ts      root   = 0;
        int64_t        since  = 0;
    };
    std::vector<SubagentRun>        _subagentRuns;
    std::unordered_set<std::string> _subagentRoles;
    uint64_t                        _savedHash = 0; // of known-sessions.json as last written
    // Removed and started sessions Claude Code has dropped (not saved), as of
    // when saveKnown last looked.
    std::unordered_set<std::string> _dropped;
    int64_t                         _prunedMs = 0;
    // Each job's state.json as last read (scanSessions).
    JobStateCache                   _jobStates;
    // Sessions whose transcript wasn't found, by session id → the refresh
    // that looked (_scanGen: not looked for again in the same one) and when
    // (epoch ms).
    std::unordered_map<std::string, std::pair<uint64_t, int64_t>> _transcriptMiss;
    uint64_t                                                      _scanGen = 0;
    // Transcripts' birth times (bornMicros), and what detectForks last saw.
    std::unordered_map<std::string, int64_t>                      _born;
    uint64_t                                                      _forkSig = 0;
    // The safety poll's refresh syncs every session (refreshScan's skip of
    // the unchanged ones aside).
    bool                                                          _syncAll = false;
    // The first scan's transcripts, parsed on a worker (connect): taken by
    // tail() instead of reading them again. By transcript path.
    struct Preparsed {
        TranscriptParser parser;
        int64_t          offset = 0;
    };
    std::unordered_map<std::string, std::shared_ptr<Preparsed>> _preparsed;
    std::unordered_map<std::string, std::string> _preFound; // session id → transcript
    bool                                         _connecting = false;
    mutable Counters                             _counters;
    std::vector<Done> _connectDone; // connect()s waiting for the first scan
    struct CommandList {
        std::vector<SlashCommand> commands;
        int64_t                   fetchedMs = 0;
        uint64_t                  rev       = 0; // commandsRevision: unique across folders
        bool                      loading   = false;
    };
    std::unordered_map<std::string, CommandList> _commands;        // by session folder
    uint64_t                                     _commandsRev = 0; // the last rev handed out
    std::vector<std::string>              _historyReads; // folders a worker reads the history of
    std::unordered_map<std::string, bool> _roleBusy, _roleUnavailable; // by role, as announced
    Account                               _account; // the login, as Claude Code last reported it
    // The user's reactions, by conversation then message (react()).
    std::unordered_map<std::string, std::map<model::Ts, std::vector<model::Reaction>>> _reactions;
    // What the Store shows of each loaded list, by (conversation, thread root;
    // 0 = the top-level list): each message's fingerprint, and the oldest ts
    // served (older messages aren't in the Store).
    struct Shown {
        std::map<model::Ts, uint64_t> fp;
        model::Ts                     from = 0;
    };
    std::map<std::pair<model::ConvRef, model::Ts>, Shown>                    _shown;
    // The typing indicators set in the Store: (conv, user, thread) → since.
    std::map<std::tuple<model::ConvRef, model::UserRef, model::Ts>, int64_t> _typing;
};

// What a turn calling tool `name` ("Read", "Bash", "mcp__x__y"…) is doing,
// for an agent branch's status line.
model::Backend::AgentPhase toolPhase(std::string_view name);
// The phase as that line, translated: "Reading files…".
std::string                phaseStatus(model::Backend::AgentPhase phase);

} // namespace claude
