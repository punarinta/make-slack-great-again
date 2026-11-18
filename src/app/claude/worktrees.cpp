#include "app/claude/worktrees.h"

#include "app/claude/async.h"
#include "app/claude/common.h"
#include "app/claude/outputs.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace claude {
namespace {

std::string cleaned(std::string_view path) {
    return path.empty() ? std::string() : cleanPath(path);
}

// Paths compared as the file system does: resolved where they exist.
std::string comparable(std::string_view path) {
#ifndef _WIN32
    if (std::string real = file::realPath(path); !real.empty())
        return real;
#endif
    return cleaned(path);
}

#if defined(_WIN32) || defined(__APPLE__)
constexpr bool kCaseless = true;
#else
constexpr bool kCaseless = false;
#endif

bool sameText(std::string_view a, std::string_view b) {
    if (!kCaseless)
        return a == b;
    return a.size() == b.size() && str::asciiLower(a) == str::asciiLower(b);
}

bool startsWithPath(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && sameText(s.substr(0, prefix.size()), prefix);
}

bool samePath(std::string_view a, std::string_view b) {
    return !a.empty() && !b.empty() &&
           (sameText(cleaned(a), cleaned(b)) || sameText(comparable(a), comparable(b)));
}

std::string shortBranch(std::string_view branch) {
    if (str::startsWith(branch, "refs/heads/"))
        branch.remove_prefix(11);
    return std::string(branch);
}

WorktreeRef refFrom(json::Value o, const char *originKey) {
    return {
        cleaned(o["worktreePath"].str()),
        shortBranch(o["worktreeBranch"].str()),
        cleaned(o[originKey].str()),
    };
}

WorktreeRef refFromJson(std::string_view text, const char *originKey) {
    json::Document doc;
    if (!doc.parse(std::string(text)))
        return {};
    return refFrom(doc.root(), originKey);
}

const std::string &gitExe() {
    static const std::string exe = [] {
        std::string found = base::findExecutable("git");
        return found.empty() ? std::string("git") : found;
    }();
    return exe;
}

void runGit(
    plat::App                                        &app,
    std::vector<std::string>                          args,
    std::function<void(int code, std::string output)> done
) {
    base::RunOptions opts;
    opts.mergeStderr = false; // unread
    opts.timeoutMs   = 60'000;
    runAsync(app, gitExe(), std::move(args), std::move(opts), [done](base::RunResult r) {
        done(r.started && !r.timedOut ? r.code : -1, std::move(r.output));
    });
}

struct Reap {
    plat::App                                    *app = nullptr;
    std::vector<WorktreeRef>                      refs;
    size_t                                        next = 0;
    std::function<bool(const std::string &)>      inUse;
    std::function<void(std::vector<std::string>)> done;
    std::vector<std::string>                      notDeleted;
};
using ReapPtr = std::shared_ptr<Reap>;
using List    = std::vector<WorktreeEntry>;

void reapNext(ReapPtr r);

// The branch goes last, unless the main worktree or another has it out.
void deleteBranch(ReapPtr r, const std::string &mainDir, const List &list, const WorktreeRef &ref) {
    const bool checkedOut = std::any_of(list.begin(), list.end(), [&](const WorktreeEntry &e) {
        return e.branch == ref.branch && !samePath(e.path, ref.path);
    });
    if (ref.branch.empty() || checkedOut) {
        reapNext(r);
        return;
    }
    runGit(*r->app, {"-C", mainDir, "branch", "-D", ref.branch}, [r, ref](int code, std::string) {
        if (code != 0)
            LOG_INFO("claude", "branch %s not deleted", ref.branch.c_str());
        reapNext(r);
    });
}

// The folder is gone (or never was): the registration goes, then the branch.
void pruneThenBranch(
    ReapPtr r, const std::string &mainDir, const List &list, const WorktreeRef &ref
) {
    if (file::exists(ref.path)) {
        r->notDeleted.push_back(ref.path);
        reapNext(r);
        return;
    }
    runGit(
        *r->app, {"-C", mainDir, "worktree", "prune"}, [r, mainDir, list, ref](int, std::string) {
            deleteBranch(r, mainDir, list, ref);
        }
    );
}

// Deletes the folder itself, off the loop's thread.
void deleteFolder(ReapPtr r, const std::string &mainDir, const List &list, const WorktreeRef &ref) {
    if (!file::exists(ref.path)) {
        pruneThenBranch(r, mainDir, list, ref);
        return;
    }
    model::runInBackground(
        *r->app,
        [path = ref.path] { file::removeTree(path); },
        [r, mainDir, list, ref] { pruneThenBranch(r, mainDir, list, ref); }
    );
}

void reapOne(ReapPtr r, const WorktreeRef &ref, const List &list) {
    const std::string mainDir = list.front().path;
    // Never the main worktree, nor the branch it has out.
    if (samePath(ref.path, mainDir) || pathWithin(mainDir, ref.path)) {
        reapNext(r);
        return;
    }
    const bool registered = std::any_of(list.begin() + 1, list.end(), [&](const WorktreeEntry &e) {
        return samePath(e.path, ref.path);
    });
    if (!registered) {
        // Unregistered: a folder is deleted only if it is a linked worktree's
        // (its .git a file pointing back to the repository), else left be.
        const std::string dotGit = file::join(ref.path, ".git");
        if (file::exists(ref.path) && !(file::exists(dotGit) && !file::isDir(dotGit))) {
            reapNext(r);
            return;
        }
        deleteFolder(r, mainDir, list, ref);
        return;
    }
    runGit(
        *r->app,
        {"-C", mainDir, "worktree", "remove", "--force", "--force", ref.path},
        [r, mainDir, list, ref](int, std::string) { deleteFolder(r, mainDir, list, ref); }
    );
}

// Finds the worktree's repository from the worktree itself or the checkout it
// was made from, whichever git still knows.
void findRepo(ReapPtr r, const WorktreeRef &ref, std::vector<std::string> from) {
    while (!from.empty() && !file::isDir(from.front()))
        from.erase(from.begin());
    if (from.empty()) {
        LOG_INFO("claude", "no repository found for worktree %s", ref.path.c_str());
        reapNext(r);
        return;
    }
    const std::string dir = from.front();
    from.erase(from.begin());
    runGit(
        *r->app,
        {"-C", dir, "worktree", "list", "--porcelain"},
        [r, ref, from](int code, std::string out) {
            const List list = code == 0 ? parseWorktreeList(out) : List{};
            if (list.empty()) {
                findRepo(r, ref, from);
                return;
            }
            reapOne(r, ref, list);
        }
    );
}

void reapNext(ReapPtr r) {
    if (r->next >= r->refs.size()) {
        // Never from inside reapWorktrees itself, even with nothing to do.
        r->app->post([r] {
            if (r->done)
                r->done(std::move(r->notDeleted));
        });
        return;
    }
    const WorktreeRef ref = r->refs[r->next++];
    if (ref.path.empty() || (r->inUse && r->inUse(ref.path))) {
        if (!ref.path.empty())
            LOG_INFO("claude", "worktree %s kept, a live session works in it", ref.path.c_str());
        reapNext(r);
        return;
    }
    findRepo(r, ref, {ref.path, ref.origin});
}

} // namespace

void addWorktree(std::vector<WorktreeRef> &refs, WorktreeRef ref) {
    if (ref.path.empty())
        return;
    for (WorktreeRef &have : refs)
        if (samePath(have.path, ref.path)) {
            if (have.branch.empty())
                have.branch = ref.branch;
            if (have.origin.empty())
                have.origin = ref.origin;
            return;
        }
    refs.push_back(std::move(ref));
}

std::vector<WorktreeRef> worktreesOfJob(std::string_view stateJson) {
    std::vector<WorktreeRef> out;
    addWorktree(out, refFromJson(stateJson, "originCwd"));
    return out;
}

std::vector<WorktreeRef> worktreesOfTranscript(const std::string &transcriptPath) {
    std::vector<WorktreeRef> out;
    if (transcriptPath.empty())
        return out;
    // A megabyte at a time (a transcript runs to tens of MB); only a line
    // that names the record is parsed.
    LineReader     lines(transcriptPath, 0);
    json::Document doc;
    for (std::string_view line; lines.next(&line);) {
        if (line.find("\"worktree-state\"") == std::string_view::npos || !doc.parse(line, nullptr))
            continue;
        const json::Value o = doc.root();
        if (o["type"].str() != "worktree-state")
            continue;
        // null / {} = the session left its worktree, which stays on disk.
        addWorktree(out, refFrom(o["worktreeSession"], "originalCwd"));
    }
    const std::string subagents = Paths::subagentsDir(cleanPath(file::absolute(transcriptPath)));
    std::vector<file::DirEntry> metas;
    file::listDir(subagents, &metas);
    std::sort(metas.begin(), metas.end(), [](const file::DirEntry &a, const file::DirEntry &b) {
        return a.name < b.name;
    });
    for (const file::DirEntry &m : metas) {
        if (m.isDir || !str::endsWith(m.name, ".meta.json") || m.size > 256 * 1024)
            continue;
        std::string meta;
        if (file::readAll(file::join(subagents, m.name), &meta))
            addWorktree(out, refFromJson(meta, "originalCwd"));
    }
    return out;
}

bool pathWithin(std::string_view path, std::string_view dir) {
    if (path.empty() || dir.empty())
        return false;
    if (samePath(path, dir))
        return true;
    const std::string pairs[2][2] = {
        {cleaned(path), cleaned(dir)},
        {comparable(path), comparable(dir)},
    };
    for (const auto &pd : pairs) {
        const std::string prefix = str::endsWith(pd[1], "/") ? pd[1] : pd[1] + "/";
        if (startsWithPath(pd[0], prefix))
            return true;
    }
    return false;
}

bool worktreeInUse(const Paths &paths, std::string_view path) {
    // One scan: every live process's own entry (a session, a background
    // worker) by its folder, and the sessions it makes up by theirs.
    std::vector<SessionInfo>       live;
    const std::vector<SessionInfo> sessions = scanSessions(paths, &live);
    for (const SessionInfo &s : live)
        if (pathWithin(s.cwd, path))
            return true;
    for (const SessionInfo &s : sessions)
        if (s.running && (pathWithin(s.cwd, path) || pathWithin(s.worktreePath, path)))
            return true;
    return false;
}

std::vector<WorktreeEntry> parseWorktreeList(std::string_view porcelain) {
    std::vector<WorktreeEntry> out;
    str::Splitter              lines(porcelain, '\n');
    for (std::string_view line; lines.next(&line);) {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (str::startsWith(line, "worktree "))
            out.push_back({cleaned(line.substr(9)), {}});
        else if (str::startsWith(line, "branch ") && !out.empty())
            out.back().branch = shortBranch(line.substr(7));
    }
    return out;
}

void reapWorktrees(
    plat::App                                               &app,
    std::vector<WorktreeRef>                                 refs,
    std::function<bool(const std::string &path)>             inUse,
    std::function<void(std::vector<std::string> notDeleted)> done
) {
    auto r   = std::make_shared<Reap>();
    r->app   = &app;
    r->refs  = std::move(refs);
    r->inUse = std::move(inUse);
    r->done  = std::move(done);
    reapNext(r);
}

} // namespace claude
