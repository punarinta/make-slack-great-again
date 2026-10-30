#include "app/claude/roster.h"
#include "app/claude/approvals.h"

#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"

#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "base/winstr.h"
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace claude {
namespace {

// State files are a few KB; never slurp a surprise.
constexpr size_t kMaxStateFile = 256 * 1024;

std::string readSmallFile(const std::string &path) {
    std::string out;
    file::readRange(path, 0, kMaxStateFile, &out);
    return out;
}

// Parses `json` into `doc`; the root object, or an empty Value when it isn't one.
json::Value parseObject(json::Document &doc, std::string_view json) {
    if (!doc.parse(std::string(json), nullptr) || !doc.root().isObject())
        return {};
    return doc.root();
}

std::string text(const json::Value &v) {
    return std::string(v.str());
}

int64_t isoToMs(std::string_view iso) {
    return base::parseIsoMicros(iso) / 1000;
}

std::string configDir() {
    const std::string env = base::env("CLAUDE_CONFIG_DIR");
    return env.empty() ? base::homeDir() + "/.claude" : env;
}

// The *.json files directly in `dir`, as full paths.
std::vector<std::string> jsonFiles(const std::string &dir) {
    std::vector<std::string>    out;
    std::vector<file::DirEntry> entries;
    if (!file::listDir(dir, &entries))
        return out;
    for (const auto &e : entries)
        if (!e.isDir && str::endsWith(e.name, ".json"))
            out.push_back(file::join(dir, e.name));
    return out;
}

#ifdef __linux__
int64_t selfPid() {
    return int64_t(getpid());
}
#endif

} // namespace

Paths Paths::detect() {
    Paths p;
    p.home = configDir();
    return p;
}

std::string Paths::findTranscript(std::string_view sessionId) const {
    if (sessionId.empty())
        return {};
    const std::string           name = str::concat({sessionId, ".jsonl"});
    const std::string           dir  = projectsDir();
    std::vector<file::DirEntry> entries;
    if (!file::listDir(dir, &entries))
        return {};
    for (const auto &e : entries) {
        if (!e.isDir)
            continue;
        std::string candidate = str::concat({dir, "/", e.name, "/", name});
        if (file::exists(candidate))
            return candidate;
    }
    return {};
}

std::string_view Paths::transcriptSessionId(std::string_view transcriptPath) {
    std::string_view base = file::baseName(transcriptPath);
    if (const size_t dot = base.rfind('.'); dot != std::string_view::npos && dot > 0)
        base = base.substr(0, dot);
    return base;
}

std::string Paths::subagentsDir(std::string_view transcriptPath) {
    // <dir>/<base name without its last extension>/subagents
    return str::concat(
        {file::dirName(transcriptPath), "/", transcriptSessionId(transcriptPath), "/subagents"}
    );
}

std::string Paths::subagentTranscript(std::string_view transcriptPath, std::string_view agentId) {
    return str::concat({subagentsDir(transcriptPath), "/agent-", agentId, ".jsonl"});
}

bool statusIsBusy(std::string_view s) {
    return s == "busy" || s == "working";
}

bool statusHasShell(std::string_view s) {
    return s == "shell";
}

bool statusNeedsUser(std::string_view s) {
    return s == "waiting" || s == "blocked";
}

std::optional<SessionInfo> parseInteractiveSession(std::string_view json) {
    json::Document    doc;
    const json::Value o = parseObject(doc, json);
    SessionInfo       s;
    s.sessionId = text(o["sessionId"]);
    if (s.sessionId.empty())
        return std::nullopt;
    // A background session's worker process registers here too, as kind "bg";
    // scanSessions folds it into the job's entry.
    s.kind =
        o["kind"].str() == "bg" ? SessionInfo::Kind::Background : SessionInfo::Kind::Interactive;
    s.name          = text(o["name"]);
    s.cwd           = text(o["cwd"]);
    s.status        = text(o["status"]);
    s.pid           = o["pid"].integer();
    s.entrypoint    = text(o["entrypoint"]);
    s.statusSinceMs = o["statusUpdatedAt"].integer();
    s.peerSocket    = text(o["messagingSocketPath"]);
    s.jobId         = text(o["jobId"]);
    s.running       = true; // the caller checks the pid
    return s;
}

std::optional<SessionInfo> parseBackgroundJob(std::string_view json) {
    json::Document    doc;
    const json::Value o = parseObject(doc, json);
    SessionInfo       s;
    s.sessionId = text(o["sessionId"]);
    if (s.sessionId.empty())
        return std::nullopt;
    s.kind           = SessionInfo::Kind::Background;
    s.name           = text(o["name"]);
    s.cwd            = text(o["cwd"]);
    s.status         = text(o["state"]);
    s.needs          = text(o["needs"]);
    // Waiting for an approval reads "working" + a needs line, not "blocked"
    // (verified 2026-09-25): it is waiting for the user all the same. A
    // question reads "blocked" + the question, which can begin with "approve"
    // too ("approve the data-origin wording before filing the issue", 2.1.284),
    // so only the state tells them apart. The one blocked dialog is the
    // project-MCP-servers one ("approve 2 new project MCP servers (…) —
    // attach to respond").
    s.awaitsApproval = str::startsWith(s.needs, "approve ") &&
                       (s.status != "blocked" || str::endsWith(s.needs, "attach to respond"));
    if (!s.needs.empty())
        s.status = "blocked";
    for (const json::Value f : o["respawnFlags"])
        s.permissionHook = s.permissionHook || hasPermissionHook(f.str());
    // Claude Code predicts the reply when a turn ends on a question (a bg
    // worker without a focused terminal predicts nothing else) and clears it
    // when the next turn starts — yet a stopped job can keep a stale one, and
    // its own list offers it only while "blocked" and not on multiple-choice
    // questions (verified in 2.1.283), so neither does msga.
    if (o["tempo"].str() == "blocked" && !o["block"].has("questions"))
        s.suggestedReply = std::string(str::trim(o["suggestedReply"].str()));
    s.transcriptPath = text(o["linkScanPath"]);
    s.worktreePath   = text(o["worktreePath"]);
    s.statusSinceMs  = isoToMs(o["updatedAt"].str());
    // Running = its worker process is alive, which only the worker's own
    // sessions/<pid>.json tells (applyWorker); a job file alone runs nothing.
    s.running        = false;
    return s;
}

std::string readJobState(const Paths &paths, std::string_view jobId) {
    if (jobId.empty())
        return {};
    return readSmallFile(str::concat({paths.jobsDir(), "/", jobId, "/state.json"}));
}

bool isProcessAlive(int64_t pid) {
    if (pid <= 0)
        return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return false;
    DWORD      code  = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

std::vector<int64_t>
liveWorkerPids(const Paths &paths, std::string_view sessionId, std::string_view jobIdOrEmpty) {
    std::vector<int64_t>   out;
    const std::string_view jobId = jobIdOrEmpty.empty() ? sessionId.substr(0, 8) : jobIdOrEmpty;
    for (const std::string &f : jsonFiles(paths.sessionsDir())) {
        const auto s = parseInteractiveSession(readSmallFile(f));
        if (s && s->kind == SessionInfo::Kind::Background &&
            (s->sessionId == sessionId || (!s->jobId.empty() && s->jobId == jobId)) &&
            isProcessAlive(s->pid))
            out.push_back(s->pid);
    }
    return out;
}

bool hasLiveWorker(const Paths &paths, std::string_view sessionId, std::string_view jobId) {
    return !liveWorkerPids(paths, sessionId, jobId).empty();
}

#ifdef __linux__
namespace {

// /proc/<pid>/<file>, read to its end. Not file::readAll: /proc files read
// as size 0, which a read sized by fstat takes as empty.
bool readProc(int64_t pid, const char *name, std::string *out) {
    out->clear();
    const std::string path = str::concat({"/proc/", str::number(pid), "/", name});
    const int         fd   = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[4096];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        out->append(buf, size_t(r));
        if (out->size() > (1u << 20))
            break; // no environment is this big
    }
    ::close(fd);
    return true;
}

// /proc/<pid>/<file> split at its NULs (environ, cmdline); empty when the
// process is gone or not ours to read.
std::vector<std::string> procStrings(int64_t pid, const char *name) {
    std::vector<std::string> out;
    std::string              data;
    if (!readProc(pid, name, &data))
        return out;
    size_t start = 0;
    for (size_t i = 0; i <= data.size(); ++i)
        if (i == data.size() || data[i] == '\0') {
            if (i > start || i < data.size())
                out.emplace_back(data, start, i - start);
            start = i + 1;
        }
    return out;
}

bool contains(const std::vector<std::string> &v, std::string_view s) {
    for (const auto &x : v)
        if (x == s)
            return true;
    return false;
}

int64_t parentPid(int64_t pid) {
    std::string stat;
    if (!readProc(pid, "stat", &stat))
        return 0;
    // "<pid> (<comm>) <state> <ppid> …" — comm may hold spaces and parentheses.
    const size_t close = stat.rfind(')');
    if (close == std::string::npos)
        return 0;
    size_t i = close + 2; // past ") "
    while (i < stat.size() && stat[i] != ' ')
        ++i; // the state
    int64_t ppid = 0;
    for (++i; i < stat.size() && stat[i] >= '0' && stat[i] <= '9'; ++i)
        ppid = ppid * 10 + (stat[i] - '0');
    return ppid;
}

bool descendsFrom(int64_t pid, int64_t ancestor) {
    for (int depth = 0; pid > 1 && depth < 64; ++depth) {
        if (pid == ancestor)
            return true;
        pid = parentPid(pid);
    }
    return false;
}

} // namespace
#endif

std::vector<int64_t> leftoverProcesses(std::string_view sessionId, std::string_view shortId) {
    std::vector<int64_t> out;
#ifdef __linux__
    if (sessionId.empty() || shortId.empty())
        return out;
    const std::string           idVar     = str::concat({"CLAUDE_CODE_SESSION_ID=", sessionId});
    const std::string           jobSuffix = str::concat({"/jobs/", shortId});
    const int64_t               self      = selfPid();
    std::vector<file::DirEntry> entries;
    file::listDir("/proc", &entries);
    for (const auto &entry : entries) {
        if (!entry.isDir || entry.name.empty())
            continue;
        int64_t pid   = 0;
        bool    isPid = true;
        for (const char c : entry.name) {
            if (c < '0' || c > '9') {
                isPid = false;
                break;
            }
            pid = pid * 10 + (c - '0');
        }
        if (!isPid || pid == self)
            continue;
        const auto env    = procStrings(pid, "environ");
        bool       hasJob = false;
        for (const auto &v : env)
            if (str::startsWith(v, "CLAUDE_JOB_DIR=") && str::endsWith(v, jobSuffix)) {
                hasJob = true;
                break;
            }
        if (!hasJob || !contains(env, idVar))
            continue;
        const auto argv = procStrings(pid, "cmdline");
        if (contains(argv, "--bg-spare") || contains(argv, "--bg-pty-host") ||
            (argv.size() > 1 && argv[1] == "daemon"))
            continue; // Claude Code's own machinery, serving every session
        if (descendsFrom(pid, self))
            continue; // msga was started from that session (a dev run)
        out.push_back(pid);
    }
#else
    (void)sessionId;
    (void)shortId;
#endif
    return out;
}

std::vector<int64_t>
strandedWorker(const Paths &paths, std::string_view sessionId, std::string_view jobId) {
    std::vector<int64_t> out;
#ifdef __linux__
    for (const int64_t pid : liveWorkerPids(paths, sessionId, jobId)) {
        out.push_back(pid);
        const int64_t host = parentPid(pid);
        if (host > 1 && contains(procStrings(host, "cmdline"), "--bg-pty-host"))
            out.push_back(host);
    }
#else
    (void)paths;
    (void)sessionId;
    (void)jobId;
#endif
    return out;
}

void signalProcess(int64_t pid, bool force) {
#ifdef _WIN32
    (void)pid;
    (void)force;
#else
    if (pid > 0)
        ::kill(pid_t(pid), force ? SIGKILL : SIGTERM);
#endif
}

void applyWorker(SessionInfo &job, const SessionInfo &worker) {
    // The worker stays alive (idle) after a turn even though the job reads
    // "done", and resuming it then only starts a copy: it counts as running.
    // The worker's status is the live one: a job can keep reading "working"
    // long after its last turn ended (a stale inFlight.queued, seen 2026-09-25).
    job.running      = true;
    job.pid          = worker.pid;
    job.peerSocket   = worker.peerSocket;
    job.workerStatus = worker.status;
    if (job.status != "blocked" && !worker.status.empty()) {
        job.status        = worker.status;
        job.statusSinceMs = worker.statusSinceMs; // since the turn began, say (pumpTyping)
    }
    if (job.name.empty())
        job.name = worker.name;
}

namespace {

// `path` parsed with `parse`, or as it was the last time when its size and
// modification time are the same (kept in `seen`, from `old` when there is one).
std::optional<SessionInfo> cachedParse(
    const std::string &key,
    const std::string &path,
    std::optional<SessionInfo> (*parse)(std::string_view),
    JobStateCache::Files *old,
    JobStateCache::Files &seen
) {
    if (!old)
        return parse(readSmallFile(path));
    JobStateCache::Entry e;
    file::Stat           st;
    if (file::stat(path, &st)) {
        e.size        = st.size;
        e.mtimeMicros = st.mtimeMicros;
    }
    const auto was = old->find(key);
    if (was != old->end() && e.size >= 0 && was->second.size == e.size &&
        was->second.mtimeMicros == e.mtimeMicros)
        e.job = std::move(was->second.job);
    else
        e.job = parse(readSmallFile(path));
    std::optional<SessionInfo> s = e.job;
    seen.emplace(key, std::move(e));
    return s;
}

} // namespace

std::vector<SessionInfo>
scanSessions(const Paths &paths, std::vector<SessionInfo> *live, JobStateCache *cache) {
    std::vector<SessionInfo>                     out;
    std::unordered_map<std::string, SessionInfo> workers;      // background workers, by session id
    std::unordered_map<std::string, SessionInfo> workersOfJob; // …and by job, when they name it
    JobStateCache seen; // what the cache keeps: the files still there
    for (const std::string &f : jsonFiles(paths.sessionsDir())) {
        auto s = cachedParse(
            f, f, parseInteractiveSession, cache ? &cache->bySession : nullptr, seen.bySession
        );
        if (!s || !isProcessAlive(s->pid))
            continue;
        if (live)
            live->push_back(*s);
        if (s->kind == SessionInfo::Kind::Background) {
            if (!s->jobId.empty())
                workersOfJob[s->jobId] = *s;
            std::string id = s->sessionId;
            workers[id]    = std::move(*s);
        } else {
            out.push_back(std::move(*s));
        }
    }
    const std::string           jobsDir = paths.jobsDir();
    std::vector<file::DirEntry> jobs;
    file::listDir(jobsDir, &jobs);
    for (const auto &d : jobs) {
        if (!d.isDir)
            continue;
        std::optional<SessionInfo> s = cachedParse(
            d.name,
            str::concat({jobsDir, "/", d.name, "/state.json"}),
            parseBackgroundJob,
            cache ? &cache->byJob : nullptr,
            seen.byJob
        );
        if (!s)
            continue;
        // A session both listed as interactive and as a job (a job attached in a
        // terminal) keeps its interactive entry: that one has the live status.
        bool dup = false;
        for (const auto &e : out)
            if (e.sessionId == s->sessionId) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        if (const auto w = workersOfJob.find(d.name); w != workersOfJob.end())
            applyWorker(*s, w->second);
        else if (const auto w = workers.find(s->sessionId); w != workers.end())
            applyWorker(*s, w->second);
        s->jobId = d.name; // the job itself, whatever session it now holds
        out.push_back(std::move(*s));
    }
    if (cache)
        *cache = std::move(seen);
    return out;
}

namespace {

std::string globalConfigPath() {
    const std::string env = base::env("CLAUDE_CONFIG_DIR");
    return env.empty() ? base::homeDir() + "/.claude.json" : env + "/.claude.json";
}

// A path as Claude Code keys its projects: absolute, forward slashes, no
// trailing one (a drive root keeps its own).
std::string projectKey(std::string_view dir) {
    std::string path = file::absolute(dir);
#ifdef _WIN32
    for (char &c : path)
        if (c == '\\')
            c = '/';
#endif
    while (path.size() > 1 && path.back() == '/' && !(path.size() == 3 && path[1] == ':'))
        path.pop_back();
    return path;
}

// The folder holding `.git` (a directory, or a worktree's file) at or above
// `path`; "" outside a repository.
std::string gitRoot(std::string path) {
    for (;;) {
        if (file::exists(path + "/.git"))
            return path;
        const std::string_view parent = file::dirName(path);
        if (parent.empty() || parent == path)
            return {};
        path = std::string(parent);
    }
}

// Where trust is looked up for a folder. Inside a git repository only the
// repository root's entry counts (`root`) — a trusted folder above it
// doesn't — unless the repository is the home folder itself; elsewhere the
// folder's own or any parent's, up to `bound` ("" = the top).
struct TrustScope {
    std::string path, root, bound;
};

TrustScope trustScope(std::string_view dir) {
    TrustScope s;
    s.path                 = projectKey(dir);
    const std::string repo = gitRoot(s.path);
    if (repo.empty())
        return s;
    if (repo == projectKey(base::homeDir()))
        s.bound = repo;
    else
        s.root = repo;
    return s;
}

bool trustedIn(const json::Value &projects, const TrustScope &s) {
    if (!s.root.empty())
        return projects[s.root]["hasTrustDialogAccepted"].boolean();
    std::string path = s.path;
    for (;;) {
        if (projects[path]["hasTrustDialogAccepted"].boolean())
            return true;
        const std::string_view parent = file::dirName(path);
        if (path == s.bound || parent.empty() || parent == path)
            return false;
        path = std::string(parent);
    }
}

void sleepMs(int ms) {
#ifdef _WIN32
    ::Sleep(DWORD(ms));
#else
    ::usleep(useconds_t(ms) * 1000);
#endif
}

// Claude Code's own lock on its config: the directory `<config>.lock`
// (proper-lockfile), stale once its holder stops refreshing it for 10 s.
class ConfigLock {
public:
    explicit ConfigLock(std::string path) : _path(std::move(path)) {
        for (int i = 0; i < 100; ++i) { // up to ~5 s
            if (makeDir()) {
                _held = true;
                return;
            }
            file::Stat st;
            if (file::stat(_path, &st) && base::nowMicros() - st.mtimeMicros > 10'000'000) {
                file::remove(_path); // its holder is gone
                continue;
            }
            sleepMs(50);
        }
    }
    ~ConfigLock() {
        if (_held)
            file::remove(_path);
    }
    bool held() const { return _held; }

private:
    bool makeDir() const {
#ifdef _WIN32
        return ::CreateDirectoryW(base::widePath(_path).c_str(), nullptr) != 0;
#else
        return ::mkdir(_path.c_str(), 0777) == 0;
#endif
    }
    std::string _path;
    bool        _held = false;
};

// A project entry as Claude Code's trust prompt leaves it: `old`'s settings
// kept, else its defaults.
void writeTrustedProject(json::Writer &w, const json::Value &old) {
    w.beginObject();
    if (old.isObject()) {
        for (json::Value m : old)
            if (m.key() != "hasTrustDialogAccepted")
                w.key(m.key()).value(m);
    } else {
        w.key("allowedTools").beginArray().endArray();
        w.key("mcpContextUris").beginArray().endArray();
        w.key("mcpServers").beginObject().endObject();
        w.key("enabledMcpjsonServers").beginArray().endArray();
        w.key("disabledMcpjsonServers").beginArray().endArray();
        w.key("hasClaudeMdExternalIncludesApproved").value(false);
        w.key("hasClaudeMdExternalIncludesWarningShown").value(false);
    }
    w.key("hasTrustDialogAccepted").value(true);
    w.endObject();
}

} // namespace

bool isFolderTrusted(std::string_view dir) {
    json::Document doc;
    if (!doc.parseFile(globalConfigPath()))
        return false;
    return trustedIn(doc.root()["projects"], trustScope(dir));
}

bool canTrustFolder(std::string_view dir) {
    return projectKey(dir) != projectKey(base::homeDir());
}

bool trustFolder(std::string_view dir) {
    if (!canTrustFolder(dir))
        return false;
    const TrustScope  s    = trustScope(dir);
    // Where the trust prompt records it: the repository's root, else the folder.
    const std::string key  = s.root.empty() ? s.path : s.root;
    const std::string path = globalConfigPath();
    ConfigLock        lock(path + ".lock");
    if (!lock.held())
        return false;
    json::Document doc;
    std::string    error;
    if (!doc.parseFile(path, &error)) {
        if (!error.empty() || file::exists(path))
            return false; // there but unreadable: never overwritten
    } else if (!doc.root().isObject()) {
        return false;
    }
    const json::Value root = doc.root();
    if (trustedIn(root["projects"], s))
        return true;
    // Everything else as it was (Claude Code rewrites the whole file too).
    json::Writer w(true);
    w.beginObject();
    bool projects = false;
    for (json::Value m : root) {
        w.key(m.key());
        if (m.key() != "projects" || !m.isObject() || projects) {
            w.value(m);
            continue;
        }
        projects   = true;
        bool found = false;
        w.beginObject();
        for (json::Value p : m) {
            w.key(p.key());
            if (p.key() == key && !found) {
                found = true;
                writeTrustedProject(w, p);
            } else {
                w.value(p);
            }
        }
        if (!found)
            writeTrustedProject(w.key(key), {});
        w.endObject();
    }
    if (!projects) {
        w.key("projects").beginObject();
        writeTrustedProject(w.key(key), {});
        w.endObject();
    }
    w.endObject();
    return file::writeAtomic(path, w.str() + "\n", 0600);
}

} // namespace claude
