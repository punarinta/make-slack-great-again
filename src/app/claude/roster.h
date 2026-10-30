// Which Claude Code sessions exist on this machine, read from Claude Code's own
// state directory (verified with 2.1.282):
//   • ~/.claude/sessions/<pid>.json — one per live interactive session, with a
//     live status (idle / busy / shell / waiting);
//   • ~/.claude/jobs/<id>/state.json — one per background (`claude --bg`)
//     session, kept for as long as Claude Code keeps the session, with a state
//     (working / blocked / done) and a "needs" line when it waits on the user.
// Both are internal files: parsing is forgiving and a malformed file is simply
// skipped. Nothing here spawns `claude`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace claude {

struct Paths {
    std::string home; // ~/.claude, or $CLAUDE_CONFIG_DIR

    static Paths detect();
    std::string  sessionsDir() const { return home + "/sessions"; }
    std::string  jobsDir() const { return home + "/jobs"; }
    std::string  projectsDir() const { return home + "/projects"; }
    // The transcript for a session, found by id under projects/*/ — the folder
    // name is derived from the cwd differently per platform, so never rebuild it.
    // Empty when there is none (yet).
    std::string  findTranscript(std::string_view sessionId) const;
    // A session's subagent transcript: <transcript dir>/<sessionId>/subagents/agent-<id>.jsonl.
    static std::string
    subagentTranscript(std::string_view transcriptPath, std::string_view agentId);
    // …the folder they're in: <transcript dir>/<sessionId>/subagents.
    static std::string      subagentsDir(std::string_view transcriptPath);
    // The session id a transcript is named after: its file name without the
    // last extension.
    static std::string_view transcriptSessionId(std::string_view transcriptPath);
};

struct SessionInfo {
    enum class Kind : uint8_t { Interactive, Background };

    std::string sessionId;
    std::string name; // Claude Code's session name ("msga-5a", or a background task title)
    std::string cwd;
    Kind        kind = Kind::Interactive;
    std::string status;          // raw: idle/busy/shell/waiting, or working/blocked/done
    bool        running = false; // some process drives it right now (so msga must not)
    int64_t     pid     = 0;     // interactive only
    std::string entrypoint;      // interactive: "cli" = a terminal; "sdk-cli" = driven by a program
    int64_t     statusSinceMs = 0; // when `status` last changed (epoch ms), 0 = unknown
    std::string needs;             // background: what it waits on the user for
    // Background: `needs` is a permission prompt in the worker's terminal
    // ("approve Bash: …"), not a question a message answers.
    bool        awaitsApproval = false;
    // Background: started with msga's permission hook (approvals.h), so its
    // questions come to msga rather than only to its screen.
    bool        permissionHook = false;
    // Background, blocked on a question: the reply Claude Code predicts
    // (state.json "suggestedReply"), "" when there is none to offer.
    std::string suggestedReply;
    std::string transcriptPath; // background state names it; else found by id
    std::string peerSocket;     // a live process's messaging socket (sessions/<pid>.json)
    // Background: the live worker's own status (idle/busy/shell), which `status`
    // shows unless the job reads "blocked" — "" = no worker.
    std::string workerStatus;
    // A background worker's job (sessions/<pid>.json "jobId"): it pairs the
    // worker with its job even once the worker's session id is another — a
    // `/clear` sent to the job starts a new session in the same worker.
    // A background job entry has its own (its jobs/<id> folder).
    std::string jobId;
    // Background: the worktree Claude Code made for the job (state.json
    // "worktreePath"), "" when it works in its folder as it is.
    std::string worktreePath;
    bool        operator==(const SessionInfo &) const = default;
};

// Status vocabulary. Busy = working right now; needs-user = stopped until the
// user answers (a permission prompt in the terminal, a blocked background task).
// "shell" is neither: Claude is idle, but a command it started in the
// background (a watchdog loop, a dev server) still runs — Claude Code writes it
// as `idle && background shell running ? "shell" : status` (verified in 2.1.282;
// such a loop was seen running for 19 hours). Not busy: nothing is being typed.
bool statusIsBusy(std::string_view status);
bool statusNeedsUser(std::string_view status);
bool statusHasShell(std::string_view status);

std::optional<SessionInfo> parseInteractiveSession(std::string_view json);
std::optional<SessionInfo> parseBackgroundJob(std::string_view json);
// Job `jobId`'s state.json as it reads now, "" when there is none.
std::string                readJobState(const Paths &paths, std::string_view jobId);

// Fold a background session's live worker (its sessions/<pid>.json, kind "bg")
// into the job's entry: running while the worker lives, busy per its status.
void applyWorker(SessionInfo &job, const SessionInfo &worker);

// Whether a process id is alive. A pid file can outlive a crashed session.
bool isProcessAlive(int64_t pid);

// Whether a live background worker holds session `sessionId` (its
// sessions/<pid>.json, kind "bg"; by its job too, see SessionInfo::jobId). After
// `claude stop` this is what tells the worker has exited: an idle job's
// state.json keeps reading "done".
bool hasLiveWorker(const Paths &paths, std::string_view sessionId, std::string_view jobId = {});
// …and their pids. The pid file can go before the process has exited, and a
// resume in between only starts a copy: waiting for a stop watches both.
// `jobId` "" = the job named after the session (its first 8 characters).
std::vector<int64_t>
liveWorkerPids(const Paths &paths, std::string_view sessionId, std::string_view jobId = {});

// Processes background session `sessionId` (job `shortId`) started that
// outlive its worker. `claude stop` ends the worker and with it the subagents
// and scheduled prompts that run inside it, but a command Claude ran in the
// background (run_in_background: a dev server, a watch loop) is its own
// process session and carries on, reparented to init (verified with 2.1.282).
// Everything the worker spawns has CLAUDE_CODE_SESSION_ID=<sessionId> and
// CLAUDE_JOB_DIR=<jobs dir>/<shortId> in its environment; both must match, as
// Claude Code's own daemon and warm spares can inherit a session id from the
// shell that first started them — those, msga and msga's children are never
// listed. Linux only (/proc); empty elsewhere.
std::vector<int64_t> leftoverProcesses(std::string_view sessionId, std::string_view shortId);
// The worker of background session `sessionId` still alive after `claude
// stop` had its time, with the pty host that holds it (one per worker, argv
// --bg-pty-host). A worker the daemon lost track of — a daemon restarted
// under it (seen 2026-09-28: a worker idling on for 3 days, its pty host
// reparented to init) — takes no `stop`. Linux only (/proc); empty elsewhere.
std::vector<int64_t>
strandedWorker(const Paths &paths, std::string_view sessionId, std::string_view jobId = {});
// SIGTERM, or SIGKILL when `force`. Not on Windows.
void signalProcess(int64_t pid, bool force);

// Each job's state.json and each sessions/<pid>.json as parsed when it last
// changed (size and mtime), so a scan that runs every few seconds reads only
// the files that did.
struct JobStateCache {
    struct Entry {
        int64_t                    size = -1, mtimeMicros = -1;
        std::optional<SessionInfo> job; // parseBackgroundJob's / parseInteractiveSession's
    };
    using Files = std::unordered_map<std::string, Entry>;
    Files byJob;     // by job folder name
    Files bySession; // by sessions/<pid>.json path
};

// Every session currently listed by the two directories. Interactive sessions
// whose process is gone are left out (their pid file is stale). `live`: also
// every live process's own entry (sessions/<pid>.json, interactive sessions
// and background workers alike). `jobs`: the files read before, kept (and
// pruned) there.
std::vector<SessionInfo> scanSessions(
    const Paths &paths, std::vector<SessionInfo> *live = nullptr, JobStateCache *jobs = nullptr
);

// Whether Claude Code trusts `dir` — background sessions refuse untrusted
// folders. Read as Claude Code reads it (2.1.289): in a git repository only
// the repository root's entry counts, never a trusted folder above it;
// elsewhere the folder's own or a parent's. Reads the "projects" map of
// Claude Code's global config (~/.claude.json, or
// $CLAUDE_CONFIG_DIR/.claude.json when that is set).
bool isFolderTrusted(std::string_view dir);
// Whether trustFolder can record trust for `dir`: Claude Code never saves
// trust for the home folder itself.
bool canTrustFolder(std::string_view dir);
// Records `dir` as trusted, as accepting Claude Code's trust prompt there
// does (inside a repository: its root), under Claude Code's own config lock.
// Blocking file I/O: off the UI thread. True when it is trusted afterwards.
bool trustFolder(std::string_view dir);

} // namespace claude
