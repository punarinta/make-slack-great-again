// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Every Claude Code session on this machine, for "Find a session" — what
// Claude Code's own /resume picks from: the transcripts under
// ~/.claude/projects/<folder>/<sessionId>.jsonl, whatever folder they ran in.
//
// Transcripts run to tens of megabytes, so only each file's two ends are read:
// the start for the first prompt, the end for the rest. Claude Code repeats
// the title records ("custom-title" from /rename, "ai-title") and the
// "last-prompt" record all along a transcript, and reads the title from the
// tail itself, so the tail holds the current ones. Plain file reading, safe
// on a worker thread.
#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

namespace claude_code {

struct CatalogEntry {
    QString sessionId;
    QString transcriptPath;
    QString cwd;
    QString title;       // the /rename name, else Claude's own title; "" = none
    QString firstPrompt; // one line
    QString lastPrompt;  // one line
    QString role;        // the team role it was started with (cc_roles); "" = none
    QString roleName;    // …as its prompt names it
    qint64  modifiedMs = 0;
};

// Sessions with at least one prompt, newest first. `projectsDir` is
// Paths::projectsDir().
std::vector<CatalogEntry> scanCatalog(const QString &projectsDir);

// One transcript's entry, read from its ends; false when it can't be read or
// holds no prompt.
bool readCatalogEntry(const QString &transcriptPath, CatalogEntry &entry);

// One transcript's entry from its first and last bytes (the tail's first line
// is taken to be cut, and dropped); false when it holds no prompt at all.
bool catalogEntryFrom(const QByteArray &head, const QByteArray &tail, CatalogEntry &entry);

} // namespace claude_code
