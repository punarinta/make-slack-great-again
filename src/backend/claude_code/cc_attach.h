// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Types a message into a live background session through its own terminal UI:
// `claude attach <short>` run in a hidden pseudo-terminal, the way a person at
// a terminal would. The message reaches the session without stopping it — so
// a subagent it runs, a background command or a scheduled prompt carries on —
// and arrives as the user's own prompt: answered at once when Claude is idle,
// queued by Claude Code until the next step when it's mid-turn. Stopping and
// resuming (Launcher::resume) would end all of that, and a second process
// can't hand the running one a prompt any other way (verified with Claude Code
// 2.1.282: the cross-session socket delivers a peer's message, not the user's,
// and the daemon's terminal socket is private).
//
// Verified live with 2.1.282, 2026-09-25:
//   • Attaching mirrors the session's screen; several attachers can be on at
//     once (nobody is pushed off), and ending `attach` leaves the session be.
//   • A bracketed paste of more than one line, or of a long line, arrives
//     wrapped in <pasted_content> — which Claude reads as material the user
//     pasted, not what they said. Line by line, a few hundred characters at a
//     time, with a newline (LF) between lines, it arrives exactly as written.
//   • A message starting with "!" switches the prompt to shell mode and runs
//     as a command; one starting with "/" runs a slash command, and some open
//     a panel that keeps the keyboard afterwards. A leading space keeps either
//     plain text — messages meant as slash commands aren't sent this way.
//   • A permission question (the session's own or a subagent's) or a panel
//     takes the prompt box's place and the keyboard: an Enter there answers
//     it. A subagent's question isn't in any state file, so the screen is the
//     only way to know — nothing is typed unless it shows the empty prompt box
//     with the cursor in it (readyForInput).
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QTimer;

namespace claude_code {

class PtyProcess;
class VtScreen;

class AttachInput : public QObject {
    Q_OBJECT
public:
    enum class Outcome {
        Sent,     // typed and taken: the prompt box emptied after Enter
        NotReady, // nothing typed: the prompt box never showed up ready
        Failed,   // typing began but didn't go through; it may be half there
    };
    using Done = std::function<void(Outcome, QString detail)>;

    // `program` + `args` run `claude attach <short>` (the caller's CLI path,
    // wrapped as it needs to be on Windows). Deletes itself once `done` ran.
    static AttachInput *send(
        const QString     &program,
        const QStringList &args,
        const QString     &cwd,
        const QString     &text,
        Done               done,
        QObject           *parent
    );

    // Give up before anything is typed: `done` gets NotReady ("cancelled") at
    // once. False when typing has begun — it then goes on to the end.
    bool cancel();

    // What goes to the terminal for `text`, write by write (exposed for tests).
    static QList<QByteArray> keystrokes(const QString &text);
    // How long the prompt box is waited for (tests shorten it).
    static void              setAttachTimeoutMs(int ms);

private:
    AttachInput(QString text, Done done, QObject *parent);
    ~AttachInput() override;
    void onOutput(const QByteArray &bytes);
    void settle(); // look at the screen
    void typeNext();
    void finish(Outcome outcome, const QString &detail);

    enum class Phase { Attaching, Typing, Echoing, Submitting, Done };

    QString           _text;
    Done              _done;
    PtyProcess       *_pty       = nullptr;
    VtScreen         *_screen    = nullptr;
    QTimer           *_quiet     = nullptr; // a look at the screen, soon after output
    bool              _readySeen = false;
    bool              _gotOutput = false;
    QTimer           *_limit     = nullptr; // the current phase's deadline
    Phase             _phase     = Phase::Attaching;
    QList<QByteArray> _writes;
    QString           _echo; // the start of the message, as the prompt box shows it
};

} // namespace claude_code
