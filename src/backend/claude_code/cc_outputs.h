// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Files an agent made, shown as attachments on the answer that names them.
//
// Claude Code records no list of the files a turn produced: the Write tool is
// one way, but a shell command (an ImageMagick convert, a heredoc) leaves only
// its command line. What an answer does say is where things are — a full path,
// or a folder and the bare names under it ("Files are in /tmp/x/: - a.png").
// So an answer's attachments are the media files it names that were made
// during its turn (modified between the turn's prompt and the answer): a file
// it merely refers to, made earlier, isn't an output.
//
// Each is copied into msga's cache the first time the answer is shown, under
// the session and the answer, so the attachment keeps showing what the agent
// made even after the file is changed or deleted (a job's tmp folder goes with
// the job). The copies go when the session is removed from msga.
#pragma once

#include "backend/domain.h"

#include <QString>
#include <QStringList>
#include <vector>

namespace claude_code {

// The existing files `text` names whose kind is shown as an attachment
// (images, PDFs, audio, video, HTML, CSV): absolute paths, and bare or
// relative names resolved against the folders the text names, then `cwd`.
// Absolute, in the order named, each once.
QStringList mentionedFiles(const QString &text, const QString &cwd);

struct OutputContext {
    QString convId;        // whose copies they are (cleared with the session)
    QString messageKey;    // the answer's own key (its record uuid, else ts)
    QString cwd;           // for names relative to the session's folder
    qint64  turnStart = 0; // epoch micros: the turn's prompt
    qint64  date      = 0; // epoch micros: the answer
};

// The attachments of an answer: its cached copies when it has them, else the
// files `text` names that were made during its turn, copied now. Empty when
// there are none.
std::vector<File> outputFiles(const QString &text, const OutputContext &ctx);

// Where the copies of `convId`'s outputs live, and dropping them.
QString outputsDir(const QString &convId);
void    clearOutputs(const QString &convId);
// Drops the copies of every session not in `keep` (conversation ids).
void    pruneOutputs(const QStringList &keep);

} // namespace claude_code
