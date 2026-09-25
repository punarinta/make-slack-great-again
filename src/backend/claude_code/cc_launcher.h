// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Starts and continues Claude Code sessions as BACKGROUND sessions (`claude --bg`),
// which Claude Code's own daemon runs — so they keep working if msga crashes or
// quits (docs/backend-modules-plan.md §10). Every call is a short-lived CLI
// command; what the session says is read from its transcript like any other.
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
//   • `--bg` refuses a folder Claude Code hasn't trusted (trust is inherited from
//     a trusted parent folder).
//   • `claude --bg --resume <id> --fork-session -- <prompt>` branches a session
//     (even one whose worker is alive) into a new background session: its
//     transcript starts with a copy of the original's records, same uuids and
//     timestamps, then the prompt.
#pragma once

#include "backend/claude_code/cc_roster.h"
#include "backend/domain.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

class QProcess;

namespace claude_code {

class Launcher : public QObject {
    Q_OBJECT
public:
    Launcher(QString claudePath, Paths paths, QObject *parent = nullptr);

    // done(sessionId, error): sessionId empty on failure, error then says why.
    using Done = std::function<void(QString sessionId, QString error)>;

    // A new background session in `cwd` whose first turn is `prompt`. Claude's
    // multiple-choice question tool is turned off: from msga a question must
    // arrive as text, answerable by message. `rolePrompt` (a team role's, see
    // cc_roles) is appended to Claude Code's system prompt, and `agentsJson`
    // (the team's, see subagentsJson) defines its subagent types; the session
    // keeps both from then on.
    void start(
        const QString &cwd,
        const QString &prompt,
        bool           skipPermissionChecks,
        const QString &rolePrompt,
        const QString &agentsJson,
        Done           done
    );

    // Continue session `sessionId` with `prompt`. `isBackground`: it is (or was)
    // a background session — stopped first when its worker is still alive
    // (`stopFirst`, or one is found alive now), and resumed without flags so it
    // keeps its saved options. Should Claude Code start a copy after all, `done`
    // gets the copy's id: the conversation goes on there.
    void resume(
        const QString &sessionId,
        const QString &cwd,
        const QString &prompt,
        bool           isBackground,
        bool           stopFirst,
        Done           done
    );

    // A new background session branched off session `sessionId` — a copy of its
    // conversation so far — whose first turn is `prompt`. The original isn't
    // touched: no stop, and its worker (or terminal) goes on as it was.
    void fork(const QString &sessionId, const QString &cwd, const QString &prompt, Done done);

    // Stop background session `sessionId` now, mid-turn or idle (`claude stop`):
    // its worker exits, the conversation is kept and can be resumed. Then what
    // it left running is ended too (leftoverProcesses), so nothing it started
    // goes on or wakes it again. `done` runs once all that is over (the worker
    // is waited for up to 10 s).
    void stop(const QString &sessionId, const QString &cwd, std::function<void()> done);

    // The full session id of background job `shortId` (from its state.json).
    QString sessionIdForShort(const QString &shortId) const;

    // The slash commands Claude Code offers in `cwd` — built-ins, skills,
    // project and plugin commands — from a throwaway print-mode process asked
    // only to initialize: no model call, no session saved, no MCP servers
    // started. Empty on failure.
    // `account` is the login it reports (email, organization, subscriptionType,
    // apiProvider).
    void listCommands(
        const QString &cwd, std::function<void(std::vector<SlashCommand>, QJsonObject account)> done
    );

private:
    QProcess *newProcess(const QString &cwd, QString &program, QStringList &argv);
    void
         run(const QStringList                            &args,
             const QString                                &cwd,
             std::function<void(int code, QString output)> done);
    void waitStopped(
        const QString        &sessionId,
        std::vector<qint64>   pids,
        int                   attemptsLeft,
        std::function<void()> then
    );
    void reapLeftovers(const QString &sessionId, std::function<void()> done);

    QString _claudePath;
    Paths   _paths;
};

// The commands in Claude Code's answer to an "initialize" control request
// (stream-json output). Internal and retired commands are left out.
std::vector<SlashCommand> parseCommandList(const QByteArray &output);
// The login in that answer ({} when absent).
QJsonObject               parseAccount(const QByteArray &output);

// "backgrounded · 1a2b3c4d · title" → "1a2b3c4d"; empty when absent.
QString parseBackgroundedShortId(const QString &output);
// Whether the CLI answered by starting a copy instead of continuing the session.
bool    startedACopy(const QString &output);

} // namespace claude_code
