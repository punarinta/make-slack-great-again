#include "app/claude/launcher.h"

#include "app/model/jobs.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/str.h"
#include "base/utf8.h"
#include "plat/plat.h"

#include <algorithm>
#include <utility>

namespace claude {

namespace {

// White space as \s reads it (Unicode), from `i` on.
size_t skipSpace(std::string_view s, size_t i) {
    while (i < s.size()) {
        size_t j = i;
        if (!utf8::isSpace(utf8::decode(s, j)))
            break;
        i = j;
    }
    return i;
}

bool isLeadingSpace(std::string_view s) {
    if (s.empty())
        return false;
    size_t i = 0;
    return utf8::isSpace(utf8::decode(s, i));
}

std::string exitedMessage(int code) {
    return i18n::arg(i18n::tr("Claude Code exited (code %1)."), str::number(code));
}

} // namespace

std::string parseBackgroundedShortId(std::string_view output) {
    // backgrounded\s*·\s*([0-9a-f]{6,})
    constexpr std::string_view kWord = "backgrounded", kDot = "·";
    for (size_t at = output.find(kWord); at != std::string_view::npos;
         at        = output.find(kWord, at + 1)) {
        size_t i = skipSpace(output, at + kWord.size());
        if (output.substr(i, kDot.size()) != kDot)
            continue;
        i                 = skipSpace(output, i + kDot.size());
        const size_t from = i;
        while (i < output.size() &&
               ((output[i] >= '0' && output[i] <= '9') || (output[i] >= 'a' && output[i] <= 'f')))
            ++i;
        if (i - from >= 6)
            return std::string(output.substr(from, i - from));
    }
    return {};
}

bool startedACopy(std::string_view output) {
    return output.find("started a copy") != std::string_view::npos;
}

std::string parseRemoveRefusal(std::string_view output, int exitCode) {
    if (exitCode == 0)
        return {};
    const auto lines     = str::split(output, '\n');
    // "<head> — <why>" → "<why>"
    auto       afterDash = [](std::string_view line) {
        constexpr std::string_view kDash = " — ";
        const size_t               dash  = line.find(kDash);
        return std::string(
            str::trim(dash == std::string_view::npos ? line : line.substr(dash + kDash.size()))
        );
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (str::startsWith(line, "kept ")) {
            for (size_t j = i + 1; j < lines.size(); ++j) {
                const std::string_view next = lines[j];
                if (str::trimSpace(next).empty())
                    continue;
                if (!isLeadingSpace(next))
                    break;
                return std::string(str::trimSpace(next));
            }
            return afterDash(line);
        }
        if (str::startsWith(line, "couldn't remove "))
            return afterDash(line);
    }
    for (const std::string_view line : lines)
        if (!str::trimSpace(line).empty())
            return std::string(str::trimSpace(line));
    return exitedMessage(exitCode);
}

std::vector<SlashCommand> parseCommandList(std::string_view output) {
    std::vector<SlashCommand> out;
    for (const std::string_view line : str::split(output, '\n')) {
        json::Document doc;
        if (!doc.parse(std::string(line)))
            continue;
        const json::Value o = doc.root();
        if (o["type"].str() != "control_response")
            continue;
        for (const json::Value c : o["response"]["response"]["commands"]) {
            SlashCommand cmd;
            cmd.name  = std::string(c["name"].str());
            cmd.desc  = std::string(str::trimSpace(c["description"].str()));
            cmd.usage = std::string(str::trimSpace(c["argumentHint"].str()));
            if (cmd.name.empty() || str::startsWith(cmd.name, "__") ||
                str::startsWith(cmd.desc, "(removed)") || str::startsWith(cmd.desc, "Renamed to "))
                continue; // internal, or kept only to point elsewhere
            // Skills say where they come from at the end: "… (user)", "… (project)".
            const bool isProj = str::endsWith(cmd.desc, "(project)");
            const bool isUser = str::endsWith(cmd.desc, "(user)");
            if (isProj || isUser) {
                cmd.desc.resize(cmd.desc.size() - (isProj ? 9 : 6));
                // The blanks before it go too (it starts trimmed: only the end can have any).
                cmd.desc.resize(str::trimSpace(cmd.desc).size());
            }
            cmd.source = c["builtin"].boolean() ? i18n::tr("Claude Code")
                         : isProj               ? i18n::tr("Project skill")
                                                : i18n::tr("Skill");
            out.push_back(std::move(cmd));
        }
        break;
    }
    return out;
}

Account parseAccount(std::string_view output) {
    for (const std::string_view line : str::split(output, '\n')) {
        json::Document doc;
        if (!doc.parse(std::string(line)) || doc.root()["type"].str() != "control_response")
            continue;
        const json::Value a = doc.root()["response"]["response"]["account"];
        Account           acc;
        acc.email            = std::string(a["email"].str());
        acc.organization     = std::string(a["organization"].str());
        acc.subscriptionType = std::string(a["subscriptionType"].str());
        acc.apiProvider      = std::string(a["apiProvider"].str());
        return acc;
    }
    return {};
}

Login parseLoginStatus(std::string_view output, int exitCode) {
    // The JSON may come after a warning line or two.
    const size_t   brace = output.find('{');
    json::Document doc;
    if (brace != std::string_view::npos && doc.parse(std::string(output.substr(brace)))) {
        if (const json::Value v = doc.root()["loggedIn"]; v.isBool())
            return v.boolean() ? Login::In : Login::Out;
    }
    if (exitCode != 0 && output.find("Not logged in") != std::string_view::npos)
        return Login::Out; // the --text form
    return Login::Unknown;
}

Launcher::Launcher(plat::App &app, std::string claudePath, Paths paths)
    : _app(app), _claudePath(std::move(claudePath)), _paths(std::move(paths)),
      _alive(std::make_shared<bool>(true)) {}

Launcher::~Launcher() {
    *_alive = false;
}

void Launcher::later(int ms, std::function<void()> fn) {
    _app.addTimer(ms, false, [alive = _alive, fn = std::move(fn)] {
        if (*alive)
            fn();
    });
}

void Launcher::commandFor(std::string &program, std::vector<std::string> &argv) const {
    program = _claudePath;
#ifdef _WIN32
    // An npm install is a batch script, which CreateProcess can't start itself.
    if (str::endsWith(str::asciiLower(program), ".cmd")) {
        argv.insert(argv.begin(), program);
        argv.insert(argv.begin(), "/c");
        program = "cmd.exe";
    }
#else
    (void)argv;
#endif
}

void Launcher::spawn(
    std::vector<std::string> args, base::RunOptions o, std::function<void(base::RunResult)> done
) {
    // A background session refuses a folder Claude Code doesn't trust: the
    // user picked it in msga, which says as much as its trust prompt would.
    const bool  bg = !args.empty() && args.front() == "--bg";
    std::string program;
    commandFor(program, args);
    auto r = std::make_shared<base::RunResult>();
    model::runInBackground(
        _app,
        [r, bg, program = std::move(program), args = std::move(args), o = std::move(o)] {
            if (bg && !isFolderTrusted(o.cwd))
                trustFolder(o.cwd);
            *r = base::run(program, args, o);
        },
        [r, alive = _alive, done = std::move(done)] {
            if (*alive)
                done(std::move(*r));
        }
    );
}

void Launcher::run(
    std::vector<std::string>              args,
    const std::string                    &cwd,
    std::function<void(int, std::string)> done
) {
    base::RunOptions o;
    o.cwd         = cwd;
    o.mergeStderr = true; // stdin: the null device, never a prompt to wait on
    // Each of these returns within a second or so; a stuck one must not wedge
    // the session's queue forever.
    o.timeoutMs   = 60'000;
    spawn(
        std::move(args),
        std::move(o),
        [name = std::string(file::baseName(_claudePath)),
         done = std::move(done)](base::RunResult r) {
            if (!r.started) {
                done(-1, i18n::arg(i18n::tr("Couldn't start %1: %2"), name, r.output));
                return;
            }
            done(r.timedOut ? -1 : r.code, std::move(r.output));
        }
    );
}

void Launcher::listCommands(
    const std::string &cwd, std::function<void(std::vector<SlashCommand>, Account)> done
) {
    base::RunOptions o;
    o.cwd         = cwd;
    // stderr is never read: discarded, it can't fill up and stall the process.
    o.mergeStderr = false;
    o.timeoutMs   = 30'000;
    // It answers the request, then exits at the end of its input.
    o.input = R"({"type":"control_request","request_id":"msga","request":{"subtype":"initialize"}})"
              "\n";
    spawn(
        {"-p",
         "--input-format",
         "stream-json",
         "--output-format",
         "stream-json",
         "--verbose",
         "--no-session-persistence",
         "--strict-mcp-config"},
        std::move(o),
        [done = std::move(done)](base::RunResult r) {
            done(parseCommandList(r.output), parseAccount(r.output));
        }
    );
}

void Launcher::checkLogin(std::function<void(Login)> done) {
    base::RunOptions o;
    o.mergeStderr = false; // unread, it could fill up
    o.timeoutMs   = 15'000;
    spawn({"auth", "status"}, std::move(o), [done = std::move(done)](base::RunResult r) {
        done(r.started && !r.timedOut ? parseLoginStatus(r.output, r.code) : Login::Unknown);
    });
}

std::string Launcher::sessionIdForShort(std::string_view shortId) const {
    json::Document doc;
    if (!doc.parse(readJobState(_paths, shortId), nullptr))
        return {};
    return std::string(doc.root()["sessionId"].str());
}

void Launcher::waitStopped(
    const std::string    &sessionId,
    std::vector<int64_t>  pids,
    int                   attemptsLeft,
    std::function<void()> then
) {
    // The worker's pid file goes when it exits. The job's state is no help: a
    // worker stopped while idle leaves it reading "done" (verified 2026-09-25).
    // The pid file may go a moment before the process does, so the workers
    // seen before the stop are waited for too — looked at first: while one
    // lives, the session files (every one read) needn't be.
    const bool exited =
        std::none_of(pids.begin(), pids.end(), isProcessAlive) && !hasLiveWorker(_paths, sessionId);
    if (exited || attemptsLeft <= 0) {
        then();
        return;
    }
    later(250, [this, sessionId, pids = std::move(pids), attemptsLeft, then = std::move(then)] {
        waitStopped(sessionId, pids, attemptsLeft - 1, then);
    });
}

void Launcher::reapLeftovers(
    const std::string &sessionId, std::function<void()> done, const std::string &jobId
) {
    // Looked for on a worker: that reads every process's environment (/proc).
    const std::string shortId = jobId.empty() ? sessionId.substr(0, 8) : jobId;
    const auto        signal  = [paths = _paths, sessionId, shortId](bool force) {
        auto pids = leftoverProcesses(sessionId, shortId);
        for (const int64_t pid : strandedWorker(paths, sessionId, shortId))
            pids.push_back(pid);
        for (const int64_t pid : pids)
            signalProcess(pid, force);
        return !pids.empty();
    };
    auto any = std::make_shared<bool>(false);
    model::runInBackground(
        _app,
        [signal, any] { *any = signal(false); },
        [this, alive = _alive, signal, any, done = std::move(done)]() mutable {
            if (!*alive)
                return;
            if (!*any) {
                if (done)
                    done();
                return;
            }
            // What ignores SIGTERM gets SIGKILL — looked up again, never by
            // stale pid.
            later(2000, [this, signal, done = std::move(done)] {
                model::runInBackground(
                    _app,
                    [signal] { signal(true); },
                    [alive = _alive, done] {
                        if (*alive && done)
                            done();
                    }
                );
            });
        }
    );
}

void Launcher::stop(
    const std::string &sessionId, const std::string &cwd, std::function<void()> done
) {
    const std::string shortId = sessionId.substr(0, 8);
    auto              pids    = liveWorkerPids(_paths, sessionId);
    run({"stop", shortId},
        cwd,
        [this, sessionId, pids = std::move(pids), done = std::move(done)](int, std::string) {
            // `stop` returns before the worker exits. Up to 10 s.
            waitStopped(sessionId, pids, 40, [this, sessionId, done] {
                reapLeftovers(sessionId, done);
            });
        });
}

void Launcher::remove(
    const std::string                       &sessionId,
    const std::string                       &jobId,
    const std::string                       &cwd,
    std::function<void(std::string refusal)> done
) {
    const std::string shortId = jobId.empty() ? sessionId.substr(0, 8) : jobId;
    auto              pids    = liveWorkerPids(_paths, sessionId, shortId);
    run({"rm", shortId},
        cwd,
        [this, sessionId, shortId, pids = std::move(pids), done = std::move(done)](
            int code, std::string out
        ) {
            std::string refusal = parseRemoveRefusal(out, code);
            // Like `stop`, it may return before the worker has exited (and
            // one it couldn't end is ended here). Up to 10 s.
            waitStopped(
                sessionId,
                pids,
                40,
                [this, sessionId, shortId, done, refusal = std::move(refusal)] {
                    reapLeftovers(sessionId, [done, refusal] { done(refusal); }, shortId);
                }
            );
        });
}

void Launcher::runNewSession(std::vector<std::string> args, const std::string &cwd, Done done) {
    run(std::move(args), cwd, [this, done = std::move(done)](int code, std::string out) {
        const std::string shortId = parseBackgroundedShortId(out);
        const std::string id      = shortId.empty() ? std::string() : sessionIdForShort(shortId);
        if (code != 0 || id.empty()) {
            const std::string_view text = str::trimSpace(out);
            done({}, text.empty() ? exitedMessage(code) : std::string(text));
            return;
        }
        done(id, {});
    });
}

void Launcher::addStartSettings(std::vector<std::string> &args) const {
    if (_startSettings.empty())
        return;
    args.emplace_back("--settings");
    args.push_back(_startSettings);
}

void Launcher::start(
    const std::string &cwd,
    const std::string &prompt,
    bool               skipPermissionChecks,
    const std::string &rolePrompt,
    const std::string &agentsJson,
    Done               done
) {
    std::vector<std::string> args = {"--bg", "--disallowedTools", "AskUserQuestion"};
    addStartSettings(args);
    if (skipPermissionChecks)
        args.emplace_back("--dangerously-skip-permissions");
    if (!rolePrompt.empty()) {
        args.emplace_back("--append-system-prompt");
        args.push_back(rolePrompt);
    }
    if (!agentsJson.empty()) {
        args.emplace_back("--agents");
        args.push_back(agentsJson);
    }
    args.emplace_back("--");
    args.push_back(prompt);
    runNewSession(std::move(args), cwd, std::move(done));
}

std::shared_ptr<AttachInput> Launcher::sendLive(
    const std::string &sessionId,
    const std::string &cwd,
    const std::string &prompt,
    AttachInput::Done  done
) {
    std::string              program;
    std::vector<std::string> argv;
    attachCommand(sessionId, program, argv);
    return AttachInput::send(_app, program, argv, cwd, prompt, std::move(done));
}

void Launcher::attachCommand(
    std::string_view sessionId, std::string &program, std::vector<std::string> &argv
) const {
    argv = {"attach", std::string(sessionId.substr(0, 8))};
    commandFor(program, argv);
}

void Launcher::fork(
    const std::string              &sessionId,
    const std::string              &cwd,
    const std::string              &prompt,
    Done                            done,
    const std::vector<std::string> &deniedTools
) {
    std::vector<std::string> args = {"--bg", "--resume", sessionId, "--fork-session"};
    addStartSettings(args);
    if (!deniedTools.empty()) {
        // Saved with the branch like any start option (its job's respawnFlags,
        // seen 2.1.289): a flag-free resume or a fork of it keeps them.
        args.emplace_back("--disallowedTools");
        args.emplace_back("AskUserQuestion");
        for (const std::string &t : deniedTools)
            if (t != "AskUserQuestion")
                args.push_back(t);
    }
    args.emplace_back("--");
    args.push_back(prompt);
    runNewSession(std::move(args), cwd, std::move(done));
}

void Launcher::resume(
    const std::string &sessionId,
    const std::string &cwd,
    const std::string &prompt,
    bool               isBackground,
    bool               stopFirst,
    Done               done
) {
    auto go = [this, sessionId, cwd, prompt, isBackground, done = std::move(done)] {
        std::vector<std::string> args = {"--bg", "--resume", sessionId};
        if (!isBackground) { // first time in the background: it takes flags
            args.emplace_back("--disallowedTools");
            args.emplace_back("AskUserQuestion");
            addStartSettings(args);
        }
        args.emplace_back("--");
        args.push_back(prompt);
        run(std::move(args), cwd, [this, sessionId, done](int code, std::string out) {
            const std::string shortId = parseBackgroundedShortId(out);
            if (code != 0 || shortId.empty()) {
                done({}, std::string(str::trimSpace(out)));
                return;
            }
            if (startedACopy(out)) {
                // Not expected (we stop first and pass no flags), but it has
                // happened: the old worker was still exiting. The copy holds the
                // whole conversation and is on the new turn — it IS the session
                // from here on, so it's reported as the one continued.
                const std::string copyId = sessionIdForShort(shortId);
                if (!copyId.empty() && copyId != sessionId) {
                    done(copyId, {});
                    return;
                }
                done(
                    {},
                    i18n::tr("Claude Code started a copy of this session instead of continuing it.")
                );
                return;
            }
            done(sessionId, {});
        });
    };
    // The roster the caller went by can be a moment old: a worker it hasn't
    // seen yet (or one seen gone whose process lingers) is stopped all the same.
    auto pids = isBackground ? liveWorkerPids(_paths, sessionId) : std::vector<int64_t>{};
    if (!stopFirst && pids.empty()) {
        go();
        return;
    }
    run({"stop", sessionId.substr(0, 8)},
        cwd,
        [this, sessionId, pids = std::move(pids), go = std::move(go)](int, std::string) {
            // `stop` returns before the worker has exited; resuming earlier only
            // starts a copy. Up to 10 s.
            waitStopped(sessionId, pids, 40, go);
        });
}

} // namespace claude
