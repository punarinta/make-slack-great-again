// Starts and continues Claude Code sessions as BACKGROUND sessions (`claude --bg`),
// which Claude Code's own daemon runs — so they keep working if msga crashes or
// quits (docs/backend-modules-plan.md §10). Every call is a short-lived CLI
// command (base::run on a worker thread, see async.h); what the session says
// is read from its transcript like any other.
//
// Verified with Claude Code 2.1.282 (2026-09-25):
//   • `--bg` picks its own session id (it ignores --session-id) and prints
//     "backgrounded · <short id>"; the job's state.json holds the full id.
//   • A finished turn leaves the job "done" with its worker still alive, and
//     resuming then only starts a copy — so a follow-up is `claude stop <short>`,
//     wait for the worker's sessions/<pid>.json to go (stop returns before the
//     worker exits, and an idle job keeps reading "done"), then
//     `claude --bg --resume <id> -- <prompt>`.
//   • `claude stop` ends the worker and what runs inside it — subagents,
//     scheduled prompts (CronCreate), a subagent's running command — but not a
//     command Claude ran in the background, which carries on orphaned.
//   • A background session keeps the options it was started with; passing any
//     flag on resume starts a copy instead. A session that was never a
//     background one (a terminal or -p session) takes flags fine.
//   • `--disallowedTools` takes a list, so the prompt must come after `--`.
//   • `--append-system-prompt` is saved with the session like any start option,
//     and the rendered system prompt is recorded in the transcript and sent
//     again on every resume.
//   • `--agents <json>` is saved too (a flag-free resume reports "woke session
//     … with its saved options (… --agents …)"), and its types are offered to
//     the Agent tool next to the built-in ones, each with its own prompt.
//   • `--bg` refuses a folder Claude Code hasn't trusted — since 2.1.289 a git
//     repository's root must be trusted itself, a trusted parent doesn't
//     count. msga records the trust before starting one (see trustFolder).
//   • `claude --bg --resume <id> --fork-session -- <prompt>` branches a session
//     (even one whose worker is alive) into a new background session: its
//     transcript starts with a copy of the original's records, same uuids and
//     timestamps, then the prompt.
//
// Callbacks run on the plat loop, never re-entrantly, and never after the
// Launcher is destroyed — except AttachInput's (see sendLive).
#pragma once

#include "app/claude/attach.h"
#include "app/claude/common.h"
#include "app/claude/roster.h"
#include "base/process.h"

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

// Whether the CLI is logged in (`claude auth status`). Unknown: it couldn't be
// told — the CLI didn't start, or is too old for `auth status` — which never
// blocks anything; a turn that then fails for want of a login says so itself.
enum class Login : uint8_t { In, Out, Unknown };

// The login Claude Code reports with its command list.
struct Account {
    std::string email, organization, subscriptionType, apiProvider;
    bool        empty() const {
        return email.empty() && organization.empty() && subscriptionType.empty() &&
               apiProvider.empty();
    }
};

class Launcher {
public:
    Launcher(plat::App &app, std::string claudePath, Paths paths);
    ~Launcher(); // what is under way finishes, but no callback of it runs
    Launcher(const Launcher &)            = delete;
    Launcher &operator=(const Launcher &) = delete;

    // `--settings` for the sessions it starts (see approvals.h), saved with
    // each like its other start options; "" = none.
    void setStartSettings(std::string settingsJson) { _startSettings = std::move(settingsJson); }

    // done(sessionId, error): sessionId empty on failure, error then says why.
    using Done = std::function<void(std::string sessionId, std::string error)>;

    // A new background session in `cwd` whose first turn is `prompt`. Claude's
    // multiple-choice question tool is turned off: from msga a question must
    // arrive as text, answerable by message. `rolePrompt` (a team role's, see
    // roles.h) is appended to Claude Code's system prompt, and `agentsJson`
    // (the team's, see subagentsJson) defines its subagent types; the session
    // keeps both from then on.
    void start(
        const std::string &cwd,
        const std::string &prompt,
        bool               skipPermissionChecks,
        const std::string &rolePrompt,
        const std::string &agentsJson,
        Done               done
    );

    // Continue session `sessionId` with `prompt`. `isBackground`: it is (or was)
    // a background session — stopped first when its worker is still alive
    // (`stopFirst`, or one is found alive now), and resumed without flags so it
    // keeps its saved options. Should Claude Code start a copy after all, `done`
    // gets the copy's id: the conversation goes on there.
    void resume(
        const std::string &sessionId,
        const std::string &cwd,
        const std::string &prompt,
        bool               isBackground,
        bool               stopFirst,
        Done               done
    );

    // Type `prompt` into background session `sessionId` through its live
    // worker's terminal UI (see AttachInput): no stop, no resume, so what the
    // worker runs — subagents, background commands, scheduled prompts — goes
    // on, and a turn under way gets it as its next step. NotReady means nothing
    // was typed (the UI showed a question or a panel, or `attach` failed), so
    // another way, or a later try, is safe. The AttachInput lives on its own
    // (see attach.h): `done` may run after the Launcher is gone, so the caller
    // guards it.
    std::shared_ptr<AttachInput> sendLive(
        const std::string &sessionId,
        const std::string &cwd,
        const std::string &prompt,
        AttachInput::Done  done
    );

    // `claude attach <short>` for background session `sessionId`, as the
    // program to start and its arguments (for AttachInput, AttachAnswer).
    void attachCommand(
        std::string_view sessionId, std::string &program, std::vector<std::string> &argv
    ) const;

    // A new background session branched off session `sessionId` — a copy of its
    // conversation so far — whose first turn is `prompt`. The original isn't
    // touched: no stop, and its worker (or terminal) goes on as it was.
    // `deniedTools` (with the multiple-choice question tool, as start() turns
    // it off) are turned off in the branch, which keeps that from then on;
    // none: the branch starts with no options of its own.
    void fork(
        const std::string              &sessionId,
        const std::string              &cwd,
        const std::string              &prompt,
        Done                            done,
        const std::vector<std::string> &deniedTools = {}
    );

    // Stop background session `sessionId` now, mid-turn or idle (`claude stop`):
    // its worker exits, the conversation is kept and can be resumed. Then what
    // it left running is ended too (leftoverProcesses), so nothing it started
    // goes on or wakes it again. `done` runs once all that is over (the worker
    // is waited for up to 10 s).
    void stop(const std::string &sessionId, const std::string &cwd, std::function<void()> done);

    // Delete background session `sessionId` the way Claude Code's own `claude
    // rm <short>` does: its worker is ended (running or not), its worktree
    // removed with the branch made for it, and its job dropped — the
    // transcript stays. Claude Code keeps a worktree that holds uncommitted
    // changes or unpushed commits, or that another session uses: then the job
    // stays too, and `refusal` says why (the first line of its explanation);
    // "" = removed. What the worker left running is ended as by stop(). The
    // job is `jobId` ("" = the one named after the session): a `/clear` gives
    // the job's worker another session id.
    void remove(
        const std::string                       &sessionId,
        const std::string                       &jobId,
        const std::string                       &cwd,
        std::function<void(std::string refusal)> done
    );

    // `claude auth status`: a claude.ai login and an API key (from the
    // environment or settings) both count. Takes a fraction of a second.
    void checkLogin(std::function<void(Login)> done);

    // The full session id of background job `shortId` (from its state.json).
    std::string sessionIdForShort(std::string_view shortId) const;

    // The slash commands Claude Code offers in `cwd` — built-ins, skills,
    // project and plugin commands — from a throwaway print-mode process asked
    // only to initialize: no model call, no session saved, no MCP servers
    // started. Empty on failure. `account` is the login it reports.
    void listCommands(
        const std::string &cwd, std::function<void(std::vector<SlashCommand>, Account account)> done
    );

private:
    // The program to start for the CLI with `argv` (on Windows an npm install's
    // batch script goes through cmd.exe).
    void commandFor(std::string &program, std::vector<std::string> &argv) const;
    void addStartSettings(std::vector<std::string> &args) const;
    // The CLI with `args` run to its end on a worker (base::run); done(result)
    // on the loop, unless the Launcher is gone by then.
    void spawn(
        std::vector<std::string> args, base::RunOptions o, std::function<void(base::RunResult)> done
    );
    // Runs the CLI with `args` (stdout and stderr merged, no stdin, 60 s at
    // most): done(code, output); code -1 when it didn't start or was killed.
    void
         run(std::vector<std::string>                          args,
             const std::string                                &cwd,
             std::function<void(int code, std::string output)> done);
    // Runs `args` (a --bg launch of a new session) and reports the session id
    // the CLI backgrounded, or the error it printed.
    void runNewSession(std::vector<std::string> args, const std::string &cwd, Done done);
    void waitStopped(
        const std::string    &sessionId,
        std::vector<int64_t>  pids,
        int                   attemptsLeft,
        std::function<void()> then
    );
    // `jobId` "" = the job named after the session.
    void reapLeftovers(
        const std::string &sessionId, std::function<void()> done, const std::string &jobId = {}
    );
    // `fn` on the loop in `ms`, unless the Launcher is gone by then.
    void later(int ms, std::function<void()> fn);

    plat::App            &_app;
    std::string           _claudePath;
    std::string           _startSettings;
    Paths                 _paths;
    std::shared_ptr<bool> _alive;
};

// The commands in Claude Code's answer to an "initialize" control request
// (stream-json output). Internal and retired commands are left out.
std::vector<SlashCommand> parseCommandList(std::string_view output);
// The login in that answer (empty when absent).
Account                   parseAccount(std::string_view output);

// `claude auth status` output (JSON with "loggedIn", verified 2.1.283; exit
// code 1 when logged out) → whether the CLI is logged in.
Login parseLoginStatus(std::string_view output, int exitCode);

// "backgrounded · 1a2b3c4d · title" → "1a2b3c4d"; empty when absent.
std::string parseBackgroundedShortId(std::string_view output);
// Whether the CLI answered by starting a copy instead of continuing the session.
bool        startedACopy(std::string_view output);
// Why `claude rm` (exit code `exitCode`, stdout and stderr merged) kept the
// session, "" when it removed it (read from Claude Code 2.1.283's source):
//   removed <id>                              exit 0
//   kept <id> — its worktree is still at <p>  exit 1, then indented lines
//     <why>                                     saying why (the first is it)
//   couldn't remove <id> — <why>              exit 1, the worker didn't end
std::string parseRemoveRefusal(std::string_view output, int exitCode);

} // namespace claude
