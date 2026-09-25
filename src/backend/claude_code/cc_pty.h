// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// A program run in a pseudo-terminal of its own, for driving a terminal UI
// (`claude attach`, see cc_attach): it sees a real terminal of the given size,
// what it draws arrives through output(), and write() is typing. POSIX
// pseudo-terminals on Linux and macOS, ConPTY on Windows (10 1809 and later).
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

namespace claude_code {

class PtyProcess : public QObject {
    Q_OBJECT
public:
    explicit PtyProcess(QObject *parent = nullptr);
    ~PtyProcess() override; // ends the program if it still runs

    // Start `program` with `args` in `cwd`, the environment inherited plus
    // TERM=xterm-256color. False (errorString() says why) when it couldn't.
    bool
    start(const QString &program, const QStringList &args, const QString &cwd, int rows, int cols);
    void    write(const QByteArray &bytes);
    // Ends the program (SIGTERM / TerminateProcess); finished() follows.
    void    terminate();
    bool    isRunning() const;
    QString errorString() const { return _error; }

signals:
    void output(const QByteArray &bytes);
    void finished(); // the program exited, or its terminal closed

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    QString               _error;
    friend struct Impl;
};

} // namespace claude_code
