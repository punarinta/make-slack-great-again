// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_roster.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace claude_code {
namespace {

QJsonObject parseObject(const QByteArray &json) {
    QJsonParseError err;
    const auto      doc = QJsonDocument::fromJson(json, &err);
    return err.error == QJsonParseError::NoError ? doc.object() : QJsonObject{};
}

QByteArray readSmallFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(256 * 1024); // state files are a few KB; never slurp a surprise
}

qint64 isoToMs(const QString &iso) {
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

} // namespace

Paths Paths::detect() {
    Paths         p;
    const QString env =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("CLAUDE_CONFIG_DIR"));
    p.home = env.isEmpty() ? QDir::homePath() + QStringLiteral("/.claude") : env;
    return p;
}

QString Paths::findTranscript(const QString &sessionId) const {
    if (sessionId.isEmpty())
        return {};
    const QString name = sessionId + QStringLiteral(".jsonl");
    const QDir    projects(projectsDir());
    for (const auto &dir : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString candidate = projects.filePath(dir + QLatin1Char('/') + name);
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

QString Paths::subagentTranscript(const QString &transcriptPath, const QString &agentId) {
    const QFileInfo fi(transcriptPath);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
           QStringLiteral("/subagents/agent-") + agentId + QStringLiteral(".jsonl");
}

bool statusIsBusy(const QString &s) {
    return s == QLatin1String("busy") || s == QLatin1String("working");
}

bool statusHasShell(const QString &s) {
    return s == QLatin1String("shell");
}

bool statusNeedsUser(const QString &s) {
    return s == QLatin1String("waiting") || s == QLatin1String("blocked");
}

std::optional<SessionInfo> parseInteractiveSession(const QByteArray &json) {
    const QJsonObject o = parseObject(json);
    SessionInfo       s;
    s.sessionId = o.value(QLatin1String("sessionId")).toString();
    if (s.sessionId.isEmpty())
        return std::nullopt;
    // A background session's worker process registers here too, as kind "bg";
    // scanSessions folds it into the job's entry.
    s.kind          = o.value(QLatin1String("kind")).toString() == QLatin1String("bg")
                          ? SessionInfo::Kind::Background
                          : SessionInfo::Kind::Interactive;
    s.name          = o.value(QLatin1String("name")).toString();
    s.cwd           = o.value(QLatin1String("cwd")).toString();
    s.status        = o.value(QLatin1String("status")).toString();
    s.pid           = o.value(QLatin1String("pid")).toInteger();
    s.entrypoint    = o.value(QLatin1String("entrypoint")).toString();
    s.statusSinceMs = o.value(QLatin1String("statusUpdatedAt")).toInteger();
    s.peerSocket    = o.value(QLatin1String("messagingSocketPath")).toString();
    s.running       = true; // the caller checks the pid
    return s;
}

std::optional<SessionInfo> parseBackgroundJob(const QByteArray &json) {
    const QJsonObject o = parseObject(json);
    SessionInfo       s;
    s.sessionId = o.value(QLatin1String("sessionId")).toString();
    if (s.sessionId.isEmpty())
        return std::nullopt;
    s.kind   = SessionInfo::Kind::Background;
    s.name   = o.value(QLatin1String("name")).toString();
    s.cwd    = o.value(QLatin1String("cwd")).toString();
    s.status = o.value(QLatin1String("state")).toString();
    s.needs  = o.value(QLatin1String("needs")).toString();
    // Waiting for an approval reads "working" + a needs line, not "blocked"
    // (verified 2026-09-25): it is waiting for the user all the same.
    if (!s.needs.isEmpty())
        s.status = QStringLiteral("blocked");
    s.transcriptPath = o.value(QLatin1String("linkScanPath")).toString();
    s.statusSinceMs  = isoToMs(o.value(QLatin1String("updatedAt")).toString());
    // Running = its worker process is alive, which only the worker's own
    // sessions/<pid>.json tells (applyWorker); a job file alone runs nothing.
    s.running        = false;
    return s;
}

bool isProcessAlive(qint64 pid) {
    if (pid <= 0)
        return false;
#if defined(Q_OS_WIN)
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

std::vector<qint64> liveWorkerPids(const Paths &paths, const QString &sessionId) {
    std::vector<qint64> out;
    const QDir          sessions(paths.sessionsDir());
    for (const auto &f : sessions.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        const auto s = parseInteractiveSession(readSmallFile(sessions.filePath(f)));
        if (s && s->kind == SessionInfo::Kind::Background && s->sessionId == sessionId &&
            isProcessAlive(s->pid))
            out.push_back(s->pid);
    }
    return out;
}

bool hasLiveWorker(const Paths &paths, const QString &sessionId) {
    return !liveWorkerPids(paths, sessionId).empty();
}

#if defined(Q_OS_LINUX)
namespace {

QList<QByteArray> procStrings(qint64 pid, const char *file) {
    QFile f(QStringLiteral("/proc/%1/%2").arg(pid).arg(QLatin1String(file)));
    if (!f.open(QIODevice::ReadOnly))
        return {}; // gone, or not ours to read
    return f.readAll().split('\0');
}

qint64 parentPid(qint64 pid) {
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    // "<pid> (<comm>) <state> <ppid> …" — comm may hold spaces and parentheses.
    const QByteArray stat = f.readAll();
    const auto       rest = stat.mid(stat.lastIndexOf(')') + 2).split(' ');
    return rest.size() > 1 ? rest[1].toLongLong() : 0;
}

bool descendsFrom(qint64 pid, qint64 ancestor) {
    for (int depth = 0; pid > 1 && depth < 64; ++depth) {
        if (pid == ancestor)
            return true;
        pid = parentPid(pid);
    }
    return false;
}

} // namespace
#endif

std::vector<qint64> leftoverProcesses(const QString &sessionId, const QString &shortId) {
    std::vector<qint64> out;
#if defined(Q_OS_LINUX)
    if (sessionId.isEmpty() || shortId.isEmpty())
        return out;
    const QByteArray idVar     = "CLAUDE_CODE_SESSION_ID=" + sessionId.toUtf8();
    const QByteArray jobSuffix = "/jobs/" + shortId.toUtf8();
    const qint64     self      = QCoreApplication::applicationPid();
    const QDir       proc(QStringLiteral("/proc"));
    for (const QString &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool         isPid = false;
        const qint64 pid   = entry.toLongLong(&isPid);
        if (!isPid || pid == self)
            continue;
        const auto env    = procStrings(pid, "environ");
        const bool hasJob = std::any_of(env.begin(), env.end(), [&](const QByteArray &v) {
            return v.startsWith("CLAUDE_JOB_DIR=") && v.endsWith(jobSuffix);
        });
        if (!hasJob || !env.contains(idVar))
            continue;
        const auto argv = procStrings(pid, "cmdline");
        if (argv.contains("--bg-spare") || argv.contains("--bg-pty-host") ||
            (argv.size() > 1 && argv[1] == "daemon"))
            continue; // Claude Code's own machinery, serving every session
        if (descendsFrom(pid, self))
            continue; // msga was started from that session (a dev run)
        out.push_back(pid);
    }
#else
    Q_UNUSED(sessionId);
    Q_UNUSED(shortId);
#endif
    return out;
}

void signalProcess(qint64 pid, bool force) {
#if defined(Q_OS_WIN)
    Q_UNUSED(pid);
    Q_UNUSED(force);
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
    job.running    = true;
    job.pid        = worker.pid;
    job.peerSocket = worker.peerSocket;
    if (job.status != QLatin1String("blocked") && !worker.status.isEmpty())
        job.status = worker.status;
    if (job.name.isEmpty())
        job.name = worker.name;
}

std::vector<SessionInfo> scanSessions(const Paths &paths) {
    std::vector<SessionInfo>    out;
    QHash<QString, SessionInfo> workers; // background workers, by session id
    const QDir                  sessions(paths.sessionsDir());
    for (const auto &f : sessions.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        auto s = parseInteractiveSession(readSmallFile(sessions.filePath(f)));
        if (!s || !isProcessAlive(s->pid))
            continue;
        if (s->kind == SessionInfo::Kind::Background)
            workers.insert(s->sessionId, std::move(*s));
        else
            out.push_back(std::move(*s));
    }
    const QDir jobs(paths.jobsDir());
    for (const auto &d : jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        auto s =
            parseBackgroundJob(readSmallFile(jobs.filePath(d + QStringLiteral("/state.json"))));
        if (!s)
            continue;
        // A session both listed as interactive and as a job (a job attached in a
        // terminal) keeps its interactive entry: that one has the live status.
        const bool dup = std::any_of(out.begin(), out.end(), [&](const SessionInfo &e) {
            return e.sessionId == s->sessionId;
        });
        if (dup)
            continue;
        if (const auto w = workers.constFind(s->sessionId); w != workers.cend())
            applyWorker(*s, *w);
        out.push_back(std::move(*s));
    }
    return out;
}

} // namespace claude_code
