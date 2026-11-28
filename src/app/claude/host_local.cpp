// This computer as the backend's host (host.h): what the backend did itself
// before hosts existed, moved here unchanged.
#include "app/claude/host.h"

#include "app/claude/progress.h"
#include "app/claude/transcript.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/i18n.h"

#include <algorithm>
#include <utility>

namespace claude {

using i18n::tr;

Host::~Host() = default;

int64_t Host::bornMicros(const std::string &) const {
    return 0;
}

LocalHost::LocalHost(plat::App &app, std::string claudePath, Paths paths, std::string approvalsDir)
    : _app(app), _claudePath(std::move(claudePath)), _paths(std::move(paths)),
      _approvalsDir(std::move(approvalsDir)), _launcher(_app, _claudePath, _paths),
      _alive(std::make_shared<bool>(true)) {}

LocalHost::~LocalHost() {
    *_alive = false;
}

void LocalHost::useHook(const std::string &exe) {
    if (exe.empty() || _approvalsDir.empty() || !file::makeDirs(_approvalsDir))
        return;
    _launcher.setStartSettings(permissionHookSettings(exe, _approvalsDir));
    _launcher.setPromptNote(progressNote(exe));
}

std::vector<SessionInfo> LocalHost::scan(JobStateCache *cache) {
    return scanSessions(_paths, nullptr, cache);
}

std::function<std::vector<SessionInfo>()> LocalHost::scanner() const {
    return [paths = _paths] { return scanSessions(paths); };
}

int64_t LocalHost::bornMicros(const std::string &path) const {
    file::Stat st;
    return file::stat(path, &st) ? st.birthMicros : 0;
}

void LocalHost::start(
    const std::string &cwd,
    const std::string &prompt,
    bool               skipPermissionChecks,
    const std::string &rolePrompt,
    const std::string &agentsJson,
    Launcher::Done     done
) {
    _launcher.start(cwd, prompt, skipPermissionChecks, rolePrompt, agentsJson, std::move(done));
}

void LocalHost::resume(
    const std::string &sessionId,
    const std::string &cwd,
    const std::string &prompt,
    bool               isBackground,
    bool               stopFirst,
    Launcher::Done     done
) {
    _launcher.resume(sessionId, cwd, prompt, isBackground, stopFirst, std::move(done));
}

void LocalHost::fork(
    const std::string              &sessionId,
    const std::string              &cwd,
    const std::string              &prompt,
    Launcher::Done                  done,
    const std::vector<std::string> &deniedTools
) {
    _launcher.fork(sessionId, cwd, prompt, std::move(done), deniedTools);
}

void LocalHost::stop(
    const std::string &sessionId, const std::string &cwd, std::function<void()> done
) {
    _launcher.stop(sessionId, cwd, std::move(done));
}

void LocalHost::remove(
    const std::string                       &sessionId,
    const std::string                       &jobId,
    const std::string                       &cwd,
    std::function<void(std::string refusal)> done
) {
    _launcher.remove(sessionId, jobId, cwd, std::move(done));
}

void LocalHost::checkLogin(std::function<void(Login)> done) {
    _launcher.checkLogin(std::move(done));
}

void LocalHost::listCommands(
    const std::string &cwd, std::function<void(std::vector<SlashCommand>, Account)> done
) {
    _launcher.listCommands(cwd, std::move(done));
}

std::shared_ptr<HostTask> LocalHost::sendLive(
    const std::string &sessionId,
    const std::string &cwd,
    const std::string &prompt,
    AttachInput::Done  done
) {
    return _launcher.sendLive(sessionId, cwd, prompt, std::move(done));
}

std::shared_ptr<HostTask> LocalHost::answer(
    const std::string   &sessionId,
    const std::string   &cwd,
    const std::string   &needs,
    int                  number,
    const std::string   &label,
    AttachAnswer::Result done
) {
    std::string              program;
    std::vector<std::string> argv;
    _launcher.attachCommand(sessionId, program, argv);
    return AttachAnswer::choose(
        _app,
        program,
        argv,
        cwd,
        [needs](const PermissionQuestion &q) { return questionIsFor(needs, q); },
        number,
        label,
        std::move(done)
    );
}

std::vector<HookQuestion> LocalHost::pendingQuestions() {
    return pendingHookQuestions(_approvalsDir);
}

void LocalHost::takeQuestion(const std::string &id) {
    takeHookQuestion(_approvalsDir, id);
}

bool LocalHost::answerQuestion(const std::string &id, const std::string &answer) {
    return answerHookQuestion(_approvalsDir, id, answer);
}

int64_t LocalHost::fileSize(const std::string &path) {
    return file::size(path);
}

bool LocalHost::statFile(const std::string &path, int64_t *size, int64_t *mtimeMicros) {
    file::Stat st;
    if (!file::stat(path, &st) || st.isDir)
        return false;
    *size        = st.size;
    *mtimeMicros = st.mtimeMicros;
    return true;
}

bool LocalHost::readTail(const std::string &path, int64_t size, size_t max, std::string *out) {
    return size > 0 && file::readRange(path, std::max<int64_t>(0, size - int64_t(max)), max, out);
}

bool LocalHost::canStopCommands() const {
#ifdef __linux__ // the process is found through /proc (commandProcessGroup)
    return true;
#else
    return false;
#endif
}

void LocalHost::stopCommand(
    const std::string &sessionId, const std::string &output, std::function<void(bool)> done
) {
    // Found and signalled on a worker (a walk over /proc).
    auto stopped = std::make_shared<bool>(false);
    model::runInBackground(
        _app,
        [sessionId, output, stopped] {
            const int64_t group = commandProcessGroup(sessionId, output);
            *stopped            = group > 0 && signalProcessGroup(group);
        },
        [alive = _alive, stopped, done = std::move(done)] {
            if (*alive)
                done(*stopped);
        }
    );
}

void LocalHost::commandGone(
    const std::string &sessionId, const std::string &output, std::function<void(bool)> done
) {
    auto gone = std::make_shared<bool>(false);
    model::runInBackground(
        _app,
        [gone, sessionId, output] { *gone = commandProcessGroup(sessionId, output) == 0; },
        [alive = _alive, gone, done = std::move(done)] {
            if (*alive)
                done(*gone);
        }
    );
}

void LocalHost::deleteRecord(
    const std::string &path, const std::string &uuid, std::function<void(bool, std::string)> done
) {
    struct Job {
        bool        ok = false;
        std::string error;
    };
    auto job = std::make_shared<Job>();
    model::runInBackground(
        _app,
        [job, path, uuid] { job->ok = removeFromTranscript(path, uuid, &job->error); },
        [alive = _alive, job, done = std::move(done)] {
            if (*alive)
                done(job->ok, std::move(job->error));
        }
    );
}

void LocalHost::reapWorktrees(
    std::vector<WorktreeRef> refs, std::function<void(std::vector<std::string>)> done
) {
    claude::reapWorktrees(
        _app,
        std::move(refs),
        [paths = _paths](const std::string &path) { return worktreeInUse(paths, path); },
        [alive = _alive, done = std::move(done)](std::vector<std::string> notDeleted) {
            if (*alive)
                done(std::move(notDeleted));
        }
    );
}

bool LocalHost::isDir(const std::string &dir) const {
    return file::isDir(dir);
}

std::string LocalHost::folderProblem(const std::string &dir) const {
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

void LocalHost::upload(
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

void LocalHost::catalog(std::function<void(std::vector<CatalogEntry>)> done) {
    auto entries = std::make_shared<std::vector<CatalogEntry>>();
    model::runInBackground(
        _app,
        [entries, dir = _paths.projectsDir()] { *entries = scanCatalog(dir); },
        [alive = _alive, entries, done = std::move(done)] {
            if (*alive)
                done(std::move(*entries));
        }
    );
}

bool LocalHost::catalogEntry(const std::string &sessionId, CatalogEntry *out) {
    const std::string transcript = _paths.findTranscript(sessionId);
    return !transcript.empty() && readCatalogEntry(transcript, *out);
}

} // namespace claude
