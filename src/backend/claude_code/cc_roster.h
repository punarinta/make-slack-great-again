// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Which Claude Code sessions exist on this machine, read from Claude Code's own
// state directory (docs/backend-modules-plan.md §5.1, verified with 2.1.282):
//   • ~/.claude/sessions/<pid>.json — one per live interactive session, with a
//     live status (idle / busy / shell / waiting);
//   • ~/.claude/jobs/<id>/state.json — one per background (`claude --bg`)
//     session, kept for as long as Claude Code keeps the session, with a state
//     (working / blocked / done) and a "needs" line when it waits on the user.
// Both are internal files: parsing is forgiving and a malformed file is simply
// skipped. Nothing here spawns `claude`.
#pragma once

#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>

namespace claude_code {

struct Paths {
    QString home; // ~/.claude, or $CLAUDE_CONFIG_DIR

    static Paths   detect();
    QString        sessionsDir() const { return home + QStringLiteral("/sessions"); }
    QString        jobsDir() const { return home + QStringLiteral("/jobs"); }
    QString        projectsDir() const { return home + QStringLiteral("/projects"); }
    // The transcript for a session, found by id under projects/*/ — the folder
    // name is derived from the cwd differently per platform, so never rebuild it.
    // Empty when there is none (yet).
    QString        findTranscript(const QString &sessionId) const;
    // A session's subagent transcript: <transcript dir>/<sessionId>/subagents/agent-<id>.jsonl.
    static QString subagentTranscript(const QString &transcriptPath, const QString &agentId);
};

struct SessionInfo {
    enum class Kind { Interactive, Background };

    QString sessionId;
    QString name; // Claude Code's session name ("msga-5a", or a background task title)
    QString cwd;
    Kind    kind = Kind::Interactive;
    QString status;            // raw: idle/busy/shell/waiting, or working/blocked/done
    bool    running = false;   // some process drives it right now (so msga must not)
    qint64  pid     = 0;       // interactive only
    QString entrypoint;        // interactive: "cli" = a terminal; "sdk-cli" = driven by a program
    qint64  statusSinceMs = 0; // when `status` last changed (epoch ms), 0 = unknown
    QString needs;             // background: what it waits on the user for
    QString transcriptPath;    // background state names it; else found by id
    QString peerSocket;        // a live process's messaging socket (sessions/<pid>.json)
    // Background: the live worker's own status (idle/busy/shell), which `status`
    // shows unless the job reads "blocked" — "" = no worker.
    QString workerStatus;
    bool    operator==(const SessionInfo &) const = default;
};

// Status vocabulary. Busy = working right now; needs-user = stopped until the
// user answers (a permission prompt in the terminal, a blocked background task).
// "shell" is neither: Claude is idle, but a command it started in the
// background (a watchdog loop, a dev server) still runs — Claude Code writes it
// as `idle && background shell running ? "shell" : status` (verified in 2.1.282;
// such a loop was seen running for 19 hours). Not busy: nothing is being typed.
bool statusIsBusy(const QString &status);
bool statusNeedsUser(const QString &status);
bool statusHasShell(const QString &status);

std::optional<SessionInfo> parseInteractiveSession(const QByteArray &json);
std::optional<SessionInfo> parseBackgroundJob(const QByteArray &json);

// Fold a background session's live worker (its sessions/<pid>.json, kind "bg")
// into the job's entry: running while the worker lives, busy per its status.
void applyWorker(SessionInfo &job, const SessionInfo &worker);

// Whether a process id is alive. A pid file can outlive a crashed session.
bool isProcessAlive(qint64 pid);

// Whether a live background worker holds session `sessionId` (its
// sessions/<pid>.json, kind "bg"). After `claude stop` this is what tells the
// worker has exited: an idle job's state.json keeps reading "done".
bool                hasLiveWorker(const Paths &paths, const QString &sessionId);
// …and their pids. The pid file can go before the process has exited, and a
// resume in between only starts a copy: waiting for a stop watches both.
std::vector<qint64> liveWorkerPids(const Paths &paths, const QString &sessionId);

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
std::vector<qint64> leftoverProcesses(const QString &sessionId, const QString &shortId);
// SIGTERM, or SIGKILL when `force`. Not on Windows.
void                signalProcess(qint64 pid, bool force);

// Every session currently listed by the two directories. Interactive sessions
// whose process is gone are left out (their pid file is stale).
std::vector<SessionInfo> scanSessions(const Paths &paths);

} // namespace claude_code
