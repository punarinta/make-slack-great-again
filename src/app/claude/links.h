// Agent thread links (docs/agent-thread-links-plan.md): a thread of a Slack
// workspace (the OW) answered by a Claude Code session of this machine (the
// main session), without the user carrying messages back and forth.
//
// A link is one OW thread ↔ one main session. The people allowed to ask
// (the linked message's author, and whoever the user adds) start turns: once
// they have been quiet for a while their new messages go to a branch of the
// session (Backend::startAgentBranch / continueAgentBranch) with the thread
// so far, the branch runs with every tool that acts or reaches out turned
// off, and the answer is posted into the thread as the user, labelled as an
// AI reply (Backend::postAgentReply). A status line in the thread says what
// the turn is doing until the answer is there. Before every turn the branch
// is checked against its session: one the session has gone on from is
// replaced by a new branch, told the thread's history.
//
// Everything goes through model::Backend, both sides: the workspaces'
// own backends (never the screens' proxy), handed over by whoever runs them
// (the accounts controller) as they start and stop. Links outlive both: a
// workspace that isn't running leaves its links waiting, and they go on
// from where they were (links.json) once it runs again — a question asked
// meanwhile is answered late, and says so.
#pragma once

#include "app/model/backend.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace claude {

class Links {
public:
    struct Branch {
        std::string id;       // the branch (Backend::AgentBranch::id)
        model::Ts   root = 0; // its thread root under the session
        std::string label;    // what that root shows (setAgentBranchLabel)
    };
    struct Link {
        std::string              id;
        std::string              workspace;  // the OW: its Store::workspaceId…
        std::string              channel;    // …the conversation's id…
        model::Ts                thread = 0; // …and the thread's root
        std::string              session;    // the main session's Conversation::id
        std::string              forkPoint;  // the current branch's (AgentBranch::forkPoint)
        std::vector<Branch>      branches;   // oldest first; the last one is current
        std::vector<std::string> askers;     // user ids whose 🤖 messages start turns
        // msga's own posts in the thread (never a question), and of them
        // the answers (the history new branches are told).
        std::vector<model::Ts>   posts, answers;
        std::vector<model::Ts>   pending;         // askers' messages waiting for a turn…
        std::vector<model::Ts>   asked;           // …and the ones the turn under way answers
        model::Ts                lastSeen = 0;    // the thread is watched from here
        model::Ts                sentUpTo = 0;    // the thread up to here was answered…
        model::Ts                turnUpTo = 0;    // …and up to here is the turn under way's
        model::Ts                status   = 0;    // the status line of the turn under way
        std::string              answered;        // the last answer posted (AgentTurn::answerId)
        bool                     running = false; // a turn under way…
        bool                     sent    = false; // …handed to the branch
        int64_t                  created = 0;     // epoch secs

        // This run only.
        uint32_t    gen      = 0; // the turn's: a callback of an older one is stale
        uint64_t    debounce = 0, editTimer = 0;
        int64_t     lastAskMs = 0, lastEditMs = 0;
        std::string shownStatus, wantStatus;
        bool        statusPosting = false; // the status post is under way
        bool        acked         = false; // the status line went up on the call itself
        int         posting       = 0;     // answer posts under way
        bool        following     = false; // its branch's turn is followed
        bool        inRun         = false; // the turn under way began in this run
        bool        linkedNow     = false; // made this run: nothing of it is late
    };

    // `path`: links.json; `cacheDir`: where the askers' files are fetched to.
    Links(plat::App &app, std::string path, std::string cacheDir);
    ~Links();
    Links(const Links &)            = delete;
    Links &operator=(const Links &) = delete;

    // ── The workspaces, as they run (the accounts controller) ──────────────
    // The Claude Code workspace (null: none, or it stopped), then ready()
    // once it has listed its sessions.
    void setAgents(model::Store *store, model::Backend *backend);
    void agentsReady();
    // A Slack workspace (its own backend: onThreadReplies is set here),
    // ready() once connected, detach() before its backend goes.
    void attach(model::Store &store, model::Backend &backend);
    void ready(const model::Store &store);
    void detach(const model::Store &store);

    // ── What the screens ask and do ─────────────────────────────────────────
    // Whether there is a Claude Code workspace to ask.
    bool            available() const { return _agents != nullptr; }
    model::Backend *agents() const { return _agents; }
    model::Store   *agentStore() const { return _agentStore; }
    // The link of thread `root` of `conv` in `store`'s workspace, if any.
    const Link     *find(const model::Store &store, model::ConvRef conv, model::Ts root) const;
    // The link whose branch is thread `root` of session `conv` (the Claude
    // Code workspace's conversation), if any.
    const Link     *findBranch(model::ConvRef conv, model::Ts root) const;
    const Link     *byId(const std::string &id) const;
    // Where a link's OW thread is now: its workspace's Store and the
    // conversation (null / kNoConv while that workspace isn't running).
    model::Store   *owStore(const Link &l) const;
    model::ConvRef  owConv(const Link &l) const;
    // Where its newest branch is in the Claude Code workspace: the session's
    // conversation and the branch's root (0: no branch yet).
    model::ConvRef  sessionConv(const Link &l) const;
    model::Ts       branchRoot(const Link &l) const;

    // "Ask agent…": the thread `ts` is in becomes a link to `session` (a
    // Conversation::id of the Claude Code workspace), and its message is
    // asked about at once. The message's author may call it (🤖) from then on.
    void link(model::Store &store, model::ConvRef conv, model::Ts ts, const std::string &session);
    // "Allow <name> to ask agent".
    void allow(const std::string &linkId, const std::string &userId);
    // "Unlink agent": the thread stays as it is; a turn under way is let go.
    void unlink(const std::string &linkId);

    const std::vector<std::unique_ptr<Link>> &all() const { return _links; } // tests

    // Something the user should hear about (the error banner).
    std::function<void(const std::string &message)>                           onError;
    // A link came, went or changed (the robot, the menus, the chip follow).
    std::function<void()>                                                     onChanged;
    // A message's mrkdwn as plain text, names resolved (the screens'
    // plainText); unset: the mrkdwn as it is.
    std::function<std::string(const model::Store &, std::string_view mrkdwn)> plainText;
    // The tools a branch goes without: everything that acts on the machine
    // or reaches out (the asker's text is untrusted).
    static const std::vector<std::string>                                    &deniedTools();
    // Tests: the quiet wait before a turn and the gap between status edits.
    void setTimings(int debounceMs, int editGapMs);

private:
    struct Ws {
        model::Store   *store   = nullptr;
        model::Backend *backend = nullptr;
        bool            ready   = false;
    };
    using Replies = std::vector<model::Backend::ThreadReply>;

    Link *byIdMut(const std::string &id);
    Ws   *wsOf(const Link &l);
    Ws   *wsOf(const model::Store &store);
    void  load();
    void  save();  // soon
    void  write(); // now
    void  changed();
    // A workspace (or the agents) became ready: its links watched again,
    // turns cut short by a restart taken up.
    void  resume(Ws &w);
    void  resumeLink(Link &l);
    void  watch(Link &l);
    void  threadReplies(
        const model::Store &store,
        model::ConvRef      conv,
        model::Ts           root,
        Replies             replies,
        const std::string  &error
    );
    void schedule(Link &l); // the quiet wait, then a turn
    void beginTurn(Link &l);
    void turnWithHistory(const std::string &id, uint32_t gen, Replies history);
    void
    buildTurn(const std::string &id, uint32_t gen, std::shared_ptr<Replies> history, bool fresh);
    void handTurn(
        const std::string       &id,
        uint32_t                 gen,
        std::string              prompt,
        std::vector<std::string> files,
        std::string              label,
        bool                     fresh
    );
    void turnUpdate(const std::string &id, uint32_t gen, const model::Backend::AgentTurn &t);
    void postAnswer(Link &l, const model::Backend::AgentTurn &t);
    void postChunks(
        const std::string                        &id,
        std::vector<std::string>                  chunks,
        size_t                                    next,
        std::shared_ptr<std::vector<model::File>> files
    );
    void        endTurn(Link &l); // over: the next one, if asked meanwhile
    std::string startLine(const Link &l) const;
    void        failTurn(Link &l, const std::string &error);
    void        setStatus(Link &l, std::string text);
    void        applyStatus(Link &l);
    void        clearStatus(Link &l); // the answer is there: the line goes
    void        cancelTimers(Link &l);
    void        drop(const std::string &id);
    void        checkSessions(); // removed from msga: their links go
    std::string textOf(const model::Store &st, const model::Message &m) const;
    std::string place(const Link &l) const; // "#general in Lumen"

    plat::App                         &_app;
    std::string                        _path, _cacheDir;
    std::vector<std::unique_ptr<Link>> _links;
    std::vector<Ws>                    _ws;
    model::Store                      *_agentStore    = nullptr;
    model::Backend                    *_agents        = nullptr;
    bool                               _agentsReady   = false;
    uint32_t                           _agentObserver = 0;
    uint64_t                           _checkTimer = 0, _saveTimer = 0;
    int64_t                            _startSecs  = 0; // asked before this: answered late
    int                                _debounceMs = 5000, _editGapMs = 3000;
    std::shared_ptr<char>              _alive = std::make_shared<char>(0);
};

} // namespace claude
