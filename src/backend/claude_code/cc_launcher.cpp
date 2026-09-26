// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_launcher.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace claude_code {

QString parseBackgroundedShortId(const QString &output) {
    static const QRegularExpression re(QStringLiteral("backgrounded\\s*·\\s*([0-9a-f]{6,})"));
    const auto                      m = re.match(output);
    return m.hasMatch() ? m.captured(1) : QString();
}

bool startedACopy(const QString &output) {
    return output.contains(QLatin1String("started a copy"));
}

Launcher::Launcher(QString claudePath, Paths paths, QObject *parent)
    : QObject(parent), _claudePath(std::move(claudePath)), _paths(std::move(paths)) {}

void Launcher::commandFor(QString &program, QStringList &argv) const {
    program = _claudePath;
#if defined(Q_OS_WIN)
    // An npm install is a batch script, which CreateProcess can't start itself.
    if (program.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive)) {
        argv.prepend(program);
        argv.prepend(QStringLiteral("/c"));
        program = QStringLiteral("cmd.exe");
    }
#else
    Q_UNUSED(argv)
#endif
}

QProcess *Launcher::newProcess(const QString &cwd, QString &program, QStringList &argv) {
    auto *p = new QProcess(this);
    commandFor(program, argv);
#if defined(Q_OS_WIN)
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= CREATE_NO_WINDOW; // no console window flashing up
    });
#endif
    if (!cwd.isEmpty())
        p->setWorkingDirectory(cwd);
    return p;
}

void Launcher::run(
    const QStringList &args, const QString &cwd, std::function<void(int, QString)> done
) {
    QString     program;
    QStringList argv = args;
    auto       *p    = newProcess(cwd, program, argv);
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->setStandardInputFile(QProcess::nullDevice()); // never wait on a prompt
    connect(p, &QProcess::finished, this, [p, done](int code, QProcess::ExitStatus st) {
        const QString out = QString::fromUtf8(p->readAll());
        p->deleteLater();
        done(st == QProcess::NormalExit ? code : -1, out);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        p->deleteLater();
        done(
            -1,
            QCoreApplication::translate("claude_code", "Couldn't start %1: %2")
                .arg(QFileInfo(_claudePath).fileName(), p->errorString())
        );
    });
    // Each of these returns within a second or so; a stuck one must not wedge
    // the session's queue forever.
    QTimer::singleShot(60'000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
    p->start(program, argv);
}

std::vector<SlashCommand> parseCommandList(const QByteArray &output) {
    std::vector<SlashCommand> out;
    for (const QByteArray &line : output.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.value(QLatin1String("type")).toString() != QLatin1String("control_response"))
            continue;
        const QJsonArray commands = o.value(QLatin1String("response"))
                                        .toObject()
                                        .value(QLatin1String("response"))
                                        .toObject()
                                        .value(QLatin1String("commands"))
                                        .toArray();
        for (const auto &v : commands) {
            const QJsonObject c = v.toObject();
            SlashCommand      cmd;
            cmd.name  = c.value(QLatin1String("name")).toString();
            cmd.desc  = c.value(QLatin1String("description")).toString().trimmed();
            cmd.usage = c.value(QLatin1String("argumentHint")).toString().trimmed();
            if (cmd.name.isEmpty() || cmd.name.startsWith(QLatin1String("__")) ||
                cmd.desc.startsWith(QLatin1String("(removed)")) ||
                cmd.desc.startsWith(QLatin1String("Renamed to ")))
                continue; // internal, or kept only to point elsewhere
            // Skills say where they come from at the end: "… (user)", "… (project)".
            static const QRegularExpression kOrigin(QStringLiteral("\\s*\\((user|project)\\)$"));
            const auto                      m      = kOrigin.match(cmd.desc);
            const bool                      isProj = m.hasMatch() && m.captured(1) == "project";
            if (m.hasMatch())
                cmd.desc.truncate(m.capturedStart());
            cmd.source = c.value(QLatin1String("builtin")).toBool()
                             ? QCoreApplication::translate("claude_code", "Claude Code")
                         : isProj ? QCoreApplication::translate("claude_code", "Project skill")
                                  : QCoreApplication::translate("claude_code", "Skill");
            out.push_back(std::move(cmd));
        }
        break;
    }
    return out;
}

QJsonObject parseAccount(const QByteArray &output) {
    for (const QByteArray &line : output.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.value(QLatin1String("type")).toString() == QLatin1String("control_response"))
            return o.value(QLatin1String("response"))
                .toObject()
                .value(QLatin1String("response"))
                .toObject()
                .value(QLatin1String("account"))
                .toObject();
    }
    return {};
}

void Launcher::listCommands(
    const QString &cwd, std::function<void(std::vector<SlashCommand>, QJsonObject)> done
) {
    QString     program;
    QStringList argv = {
        QStringLiteral("-p"),
        QStringLiteral("--input-format"),
        QStringLiteral("stream-json"),
        QStringLiteral("--output-format"),
        QStringLiteral("stream-json"),
        QStringLiteral("--verbose"),
        QStringLiteral("--no-session-persistence"),
        QStringLiteral("--strict-mcp-config"),
    };
    auto *p = newProcess(cwd, program, argv);
    // stderr is never read: an unread pipe could fill up and stall the process.
    p->setStandardErrorFile(QProcess::nullDevice());
    auto settled = std::make_shared<bool>(false); // finished and errorOccurred can both fire
    auto finish  = [p, done, settled] {
        if (std::exchange(*settled, true))
            return;
        const QByteArray out = p->readAllStandardOutput();
        done(parseCommandList(out), parseAccount(out));
        p->deleteLater();
    };
    connect(p, &QProcess::finished, this, finish);
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish();
    });
    QTimer::singleShot(30'000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
    p->start(program, argv);
    // It answers the request, then exits at the end of its input.
    p->write(
        R"({"type":"control_request","request_id":"msga","request":{"subtype":"initialize"}})"
        "\n"
    );
    p->closeWriteChannel();
}

QString Launcher::sessionIdForShort(const QString &shortId) const {
    QFile f(_paths.jobsDir() + QLatin1Char('/') + shortId + QStringLiteral("/state.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll())
        .object()
        .value(QLatin1String("sessionId"))
        .toString();
}

void Launcher::waitStopped(
    const QString &sessionId, std::vector<qint64> pids, int attemptsLeft, std::function<void()> then
) {
    // The worker's pid file goes when it exits. The job's state is no help: a
    // worker stopped while idle leaves it reading "done" (verified 2026-09-25).
    // The pid file may go a moment before the process does, so the workers
    // seen before the stop are waited for too.
    const bool exited =
        !hasLiveWorker(_paths, sessionId) && std::none_of(pids.begin(), pids.end(), isProcessAlive);
    if (exited || attemptsLeft <= 0) {
        then();
        return;
    }
    QTimer::singleShot(250, this, [this, sessionId, pids, attemptsLeft, then] {
        waitStopped(sessionId, pids, attemptsLeft - 1, then);
    });
}

void Launcher::reapLeftovers(const QString &sessionId, std::function<void()> done) {
    const QString shortId = sessionId.left(8);
    const auto    pids    = leftoverProcesses(sessionId, shortId);
    if (pids.empty()) {
        if (done)
            done();
        return;
    }
    for (const qint64 pid : pids)
        signalProcess(pid, false);
    // What ignores SIGTERM gets SIGKILL — looked up again, never by stale pid.
    QTimer::singleShot(2000, this, [sessionId, shortId, done] {
        for (const qint64 pid : leftoverProcesses(sessionId, shortId))
            signalProcess(pid, true);
        if (done)
            done();
    });
}

void Launcher::stop(const QString &sessionId, const QString &cwd, std::function<void()> done) {
    const QString shortId = sessionId.left(8);
    const auto    pids    = liveWorkerPids(_paths, sessionId);
    run({QStringLiteral("stop"), shortId}, cwd, [this, sessionId, pids, done](int, QString) {
        // `stop` returns before the worker exits. Up to 10 s.
        waitStopped(sessionId, pids, 40, [this, sessionId, done] {
            reapLeftovers(sessionId, done);
        });
    });
}

void Launcher::runNewSession(const QStringList &args, const QString &cwd, Done done) {
    run(args, cwd, [this, done](int code, QString out) {
        const QString shortId = parseBackgroundedShortId(out);
        const QString id      = shortId.isEmpty() ? QString() : sessionIdForShort(shortId);
        if (code != 0 || id.isEmpty()) {
            done(
                {},
                out.trimmed().isEmpty()
                    ? QCoreApplication::translate("claude_code", "Claude Code exited (code %1).")
                          .arg(code)
                    : out.trimmed()
            );
            return;
        }
        done(id, {});
    });
}

void Launcher::start(
    const QString &cwd,
    const QString &prompt,
    bool           skipPermissionChecks,
    const QString &rolePrompt,
    const QString &agentsJson,
    Done           done
) {
    QStringList args = {
        QStringLiteral("--bg"),
        QStringLiteral("--disallowedTools"),
        QStringLiteral("AskUserQuestion"),
    };
    if (skipPermissionChecks)
        args << QStringLiteral("--dangerously-skip-permissions");
    if (!rolePrompt.isEmpty())
        args << QStringLiteral("--append-system-prompt") << rolePrompt;
    if (!agentsJson.isEmpty())
        args << QStringLiteral("--agents") << agentsJson;
    args << QStringLiteral("--") << prompt;
    runNewSession(args, cwd, std::move(done));
}

AttachInput *Launcher::sendLive(
    const QString                                     &sessionId,
    const QString                                     &cwd,
    const QString                                     &prompt,
    std::function<void(AttachInput::Outcome, QString)> done
) {
    QString     program;
    QStringList argv;
    attachCommand(sessionId, program, argv);
    return AttachInput::send(program, argv, cwd, prompt, std::move(done), this);
}

void Launcher::attachCommand(const QString &sessionId, QString &program, QStringList &argv) const {
    argv = {QStringLiteral("attach"), sessionId.left(8)};
    commandFor(program, argv);
}

void Launcher::fork(
    const QString &sessionId, const QString &cwd, const QString &prompt, Done done
) {
    const QStringList args = {
        QStringLiteral("--bg"),
        QStringLiteral("--resume"),
        sessionId,
        QStringLiteral("--fork-session"),
        QStringLiteral("--"),
        prompt,
    };
    runNewSession(args, cwd, std::move(done));
}

void Launcher::resume(
    const QString &sessionId,
    const QString &cwd,
    const QString &prompt,
    bool           isBackground,
    bool           stopFirst,
    Done           done
) {
    auto go = [this, sessionId, cwd, prompt, isBackground, done] {
        QStringList args = {QStringLiteral("--bg"), QStringLiteral("--resume"), sessionId};
        if (!isBackground) // first time in the background: it takes flags
            args << QStringLiteral("--disallowedTools") << QStringLiteral("AskUserQuestion");
        args << QStringLiteral("--") << prompt;
        run(args, cwd, [this, sessionId, done](int code, QString out) {
            const QString shortId = parseBackgroundedShortId(out);
            if (code != 0 || shortId.isEmpty()) {
                done({}, out.trimmed());
                return;
            }
            if (startedACopy(out)) {
                // Not expected (we stop first and pass no flags), but it has
                // happened: the old worker was still exiting. The copy holds the
                // whole conversation and is on the new turn — it IS the session
                // from here on, so it's reported as the one continued.
                const QString copyId = sessionIdForShort(shortId);
                if (!copyId.isEmpty() && copyId != sessionId) {
                    done(copyId, {});
                    return;
                }
                done(
                    {},
                    QCoreApplication::translate(
                        "claude_code",
                        "Claude Code started a copy of this session instead of continuing it."
                    )
                );
                return;
            }
            done(sessionId, {});
        });
    };
    // The roster the caller went by can be a moment old: a worker it hasn't
    // seen yet (or one seen gone whose process lingers) is stopped all the same.
    const auto pids = isBackground ? liveWorkerPids(_paths, sessionId) : std::vector<qint64>{};
    if (!stopFirst && pids.empty()) {
        go();
        return;
    }
    const QString shortId = sessionId.left(8);
    run({QStringLiteral("stop"), shortId}, cwd, [this, sessionId, pids, go](int, QString) {
        // `stop` returns before the worker has exited; resuming earlier only
        // starts a copy. Up to 10 s.
        waitStopped(sessionId, pids, 40, go);
    });
}

} // namespace claude_code
