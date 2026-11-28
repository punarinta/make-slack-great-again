// The git worktrees a Claude Code session used, and deleting them once the
// session is removed from msga (only sessions msga started). Claude Code
// records a worktree in three places (read from 2.1.283's source):
//   • the job's state.json — worktreePath / worktreeBranch / originCwd, the
//     worktree a background session works in; ExitWorktree nulls it;
//   • the transcript — a {"type":"worktree-state","worktreeSession":{…}}
//     record each time the session enters one (null / {} when it leaves: the
//     worktree it left stays on disk all the same);
//   • a subagent's <transcript dir>/<id>/subagents/agent-*.meta.json, for a
//     subagent run in a worktree of its own.
// `claude rm` deletes only the first kind, and keeps even that when it holds
// uncommitted changes or unpushed commits. msga deletes them all, as they are.
#pragma once

#include "app/claude/roster.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace claude {

struct WorktreeRef {
    std::string path;
    std::string branch; // "" = unknown
    std::string origin; // the checkout it was made from, "" = unknown
    bool        operator==(const WorktreeRef &) const = default;
};

// Adds `ref` to `refs` unless a worktree at that path is there already (what
// it knows the other doesn't — branch, origin — is filled in).
void                     addWorktree(std::vector<WorktreeRef> &refs, WorktreeRef ref);
// A job's state.json → its worktree (none, or one).
std::vector<WorktreeRef> worktreesOfJob(std::string_view stateJson);
// The worktrees a transcript's session entered, and its subagents made.
std::vector<WorktreeRef> worktreesOfTranscript(const std::string &transcriptPath);

// Whether `path` is `dir` or lies inside it.
bool pathWithin(std::string_view path, std::string_view dir);

// Whether a live Claude Code session works in worktree `path`: a live
// process (sessions/<pid>.json, a terminal or a background worker) whose cwd
// is in it, or a background job with a live worker whose state.json names it.
bool worktreeInUse(const Paths &paths, std::string_view path);

// `git worktree list --porcelain` → its entries, the main worktree first.
struct WorktreeEntry {
    std::string path;
    std::string branch; // short name, "" = detached or bare
};
std::vector<WorktreeEntry> parseWorktreeList(std::string_view porcelain);

// Deletes each of `refs` — uncommitted changes, unpushed commits and all —
// with its branch, one after another, git run as a child process on a worker
// thread (nothing blocks the caller's):
//   git worktree remove --force --force <path>   (from its main checkout)
//   the folder itself, when that left it behind, then git worktree prune
//   git branch -D <branch>
// Kept: a repository's main worktree and the branch checked out there, a
// branch another worktree has checked out, a folder that isn't a worktree, and
// any worktree `inUse` says a live session works in (asked on the loop just
// before it's touched). `done` gets the worktrees that couldn't be deleted —
// on the loop, never from inside this call (not even for no `refs`). `app`
// must outlive the run.
void reapWorktrees(
    plat::App                                               &app,
    std::vector<WorktreeRef>                                 refs,
    std::function<bool(const std::string &path)>             inUse,
    std::function<void(std::vector<std::string> notDeleted)> done
);

} // namespace claude
