// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_attach.h"

#include "backend/claude_code/cc_pty.h"
#include "backend/claude_code/cc_vt.h"

#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace claude_code {

namespace {

// The terminal msga attaches with: wide, so a long first line of the message
// still starts on the prompt box's first row.
constexpr int kRows = 50;
constexpr int kCols = 200;

// Pasted a few hundred characters at a time, a paste stays plain typing (see
// the header); 400 is well under where it was seen turn into <pasted_content>.
constexpr int kChunk = 400;

constexpr int kLookMs     = 300;    // how often the screen is looked at while it changes
int           attachMs    = 15'000; // attaching took ~2 s when tried
constexpr int kEchoMs     = 5'000;
constexpr int kSubmitMs   = 10'000;
constexpr int kWriteGapMs = 15; // between pastes, so each is one of its own

const QByteArray kPasteStart = QByteArrayLiteral("\x1b[200~");
const QByteArray kPasteEnd   = QByteArrayLiteral("\x1b[201~");

QString plainLines(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // Control characters would be keys (Esc, Ctrl+C…): only tabs stay.
    QString out;
    out.reserve(text.size());
    for (const QChar c : text)
        if (c == QLatin1Char('\n') || c == QLatin1Char('\t') || c.unicode() >= 0x20)
            if (c.unicode() != 0x7F)
                out.append(c);
    while (out.endsWith(QLatin1Char('\n')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    while (out.startsWith(QLatin1Char('\n')))
        out.remove(0, 1);
    // "!…" would be a shell command, "/…" a slash command: a space keeps it text.
    if (out.startsWith(QLatin1Char('!')) || out.startsWith(QLatin1Char('/')))
        out.prepend(QLatin1Char(' '));
    return out;
}

} // namespace

QList<QByteArray> AttachInput::keystrokes(const QString &text) {
    QList<QByteArray> out;
    const QStringList lines = plainLines(text).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        for (int at = 0; at < line.size();) {
            int n = std::min<int>(kChunk, line.size() - at);
            if (at + n < line.size() && line[at + n].isLowSurrogate())
                --n; // never split a character in two
            out << kPasteStart + line.mid(at, n).toUtf8() + kPasteEnd;
            at += n;
        }
        if (i + 1 < lines.size())
            out << QByteArrayLiteral("\n"); // a new line in the prompt, not Enter
    }
    return out;
}

void AttachInput::setAttachTimeoutMs(int ms) {
    attachMs = ms;
}

AttachInput *AttachInput::send(
    const QString     &program,
    const QStringList &args,
    const QString     &cwd,
    const QString     &text,
    Done               done,
    QObject           *parent
) {
    auto *self = new AttachInput(text, std::move(done), parent);
    if (plainLines(text).isEmpty()) {
        self->finish(Outcome::NotReady, QStringLiteral("nothing to type"));
        return self;
    }
    if (!self->_pty->start(program, args, cwd, kRows, kCols)) {
        self->finish(Outcome::NotReady, self->_pty->errorString());
        return self;
    }
    self->_limit->start(attachMs);
    // `attach` draws within a moment ("Attaching…"); a terminal that shows
    // nothing at all isn't going to (a pseudo-terminal not wired up).
    QTimer::singleShot(std::min(attachMs, 5000), self, [self] {
        if (!self->_gotOutput && self->_phase == Phase::Attaching)
            self->finish(Outcome::NotReady, QStringLiteral("claude attach showed nothing"));
    });
    return self;
}

bool AttachInput::cancel() {
    if (_phase == Phase::Done)
        return true;
    if (_phase != Phase::Attaching)
        return false;
    finish(Outcome::NotReady, QStringLiteral("cancelled"));
    return true;
}

AttachInput::AttachInput(QString text, Done done, QObject *parent)
    : QObject(parent), _text(std::move(text)), _done(std::move(done)), _pty(new PtyProcess(this)),
      _screen(new VtScreen(kRows, kCols)), _quiet(new QTimer(this)), _limit(new QTimer(this)) {
    _quiet->setSingleShot(true);
    _quiet->setInterval(kLookMs);
    _limit->setSingleShot(true);
    const QStringList lines = plainLines(_text).split(QLatin1Char('\n'));
    _echo                   = lines.first().left(24).trimmed();
    _writes                 = keystrokes(_text);
    connect(_pty, &PtyProcess::output, this, &AttachInput::onOutput);
    connect(_quiet, &QTimer::timeout, this, &AttachInput::settle);
    connect(_pty, &PtyProcess::finished, this, [this] {
        // `attach` ended on its own: no such session, or it went away.
        finish(
            _phase == Phase::Attaching ? Outcome::NotReady : Outcome::Failed,
            QStringLiteral("claude attach exited")
        );
    });
    connect(_limit, &QTimer::timeout, this, [this] {
        switch (_phase) {
        case Phase::Attaching:
            finish(Outcome::NotReady, QStringLiteral("the prompt box never showed up ready"));
            break;
        case Phase::Echoing:
            finish(Outcome::Failed, QStringLiteral("the message didn't show up in the prompt box"));
            break;
        case Phase::Submitting:
            finish(Outcome::Failed, QStringLiteral("the prompt box kept the message after Enter"));
            break;
        default:
            break;
        }
    });
}

AttachInput::~AttachInput() {
    delete _screen;
}

void AttachInput::onOutput(const QByteArray &bytes) {
    _gotOutput = true;
    _screen->feed(bytes);
    // Looked at a moment after output starts, not once it stops: a turn's
    // spinner redraws for as long as the turn runs.
    if (!_quiet->isActive())
        _quiet->start();
}

void AttachInput::settle() {
    switch (_phase) {
    case Phase::Attaching:
        // Ready twice running, a look apart — never on a frame half drawn.
        if (!readyForInput(*_screen)) {
            _readySeen = false;
            return; // not yet (still drawing, or a question on screen): the deadline decides
        }
        if (!std::exchange(_readySeen, true)) {
            _quiet->start(); // the second look, output or not
            return;
        }
        _phase = Phase::Typing;
        _limit->stop();
        typeNext();
        break;
    case Phase::Echoing: {
        const auto box = findPromptBox(*_screen);
        if (!box || box->lines.isEmpty() || !box->lines.first().trimmed().startsWith(_echo))
            return;
        _phase = Phase::Submitting;
        _limit->start(kSubmitMs);
        _pty->write(QByteArrayLiteral("\r"));
        break;
    }
    case Phase::Submitting: {
        // Taken once the box is empty again — or gone: the Enter went to the
        // box (the message was in it), and what replaced it came after, say a
        // permission question for the turn it started.
        const auto box = findPromptBox(*_screen);
        if (!box || box->empty)
            finish(Outcome::Sent, {});
        break;
    }
    default:
        break;
    }
}

void AttachInput::typeNext() {
    if (_phase != Phase::Typing)
        return;
    if (_writes.isEmpty()) {
        _phase = Phase::Echoing;
        _limit->start(kEchoMs);
        _quiet->start(); // looked at even if typing drew nothing more
        return;
    }
    _pty->write(_writes.takeFirst());
    QTimer::singleShot(kWriteGapMs, this, &AttachInput::typeNext);
}

void AttachInput::finish(Outcome outcome, const QString &detail) {
    if (_phase == Phase::Done)
        return;
    _phase = Phase::Done;
    _quiet->stop();
    _limit->stop();
    disconnect(_pty, nullptr, this, nullptr);
    _pty->terminate(); // detaching: the session goes on
    if (auto done = std::exchange(_done, {}))
        done(outcome, detail);
    // The terminal goes once `attach` has exited (or been made to).
    connect(_pty, &PtyProcess::finished, this, &QObject::deleteLater);
    if (!_pty->isRunning())
        deleteLater();
    else
        QTimer::singleShot(5000, this, &QObject::deleteLater);
}

} // namespace claude_code
