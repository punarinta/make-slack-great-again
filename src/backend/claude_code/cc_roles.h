// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// The team: roles a Claude Code session can be started with — the Generalist
// (plain Claude Code), the built-in specialists, and teammates the user adds
// (docs/backend-modules-plan.md §10).
//
// A role's few lines are passed with --append-system-prompt, under a header
// line naming it: "# Your role: Copywriter (msga: copywriter)". Not --agent:
// an agent's prompt replaces Claude Code's instead of adding to it (verified
// 2026-09-25 — the recorded system prompt was the agent's text alone). Claude
// Code records the rendered system prompt in the transcript (an attachment of
// type "prompt_snapshot", its `systemPrompt` a list of parts, ours last) and
// sends that record again on every resume. So a session keeps the prompt it
// started with, whatever is edited later, and its role is read back from the
// header's id — which never changes, so renaming a teammate keeps its sessions.
// Sessions from before the id was written carry "# Your role: Engineer" only,
// matched by the built-in's English name.
//
// Teammates live in msga's app data, one file each (claude-code/team/<id>.md):
// a few "key: value" lines between "---" lines, then the prompt. A built-in
// has a file only once edited ("Restore default" deletes it). A removed
// teammate's file stays, marked removed: its sessions keep its name and
// picture. A role nothing here knows (another machine's, a deleted file) is a
// former teammate, named from the transcript's header.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QString>
#include <vector>

namespace claude_code {

struct Role {
    QString id;          // stable: "engineer", "copywriter"
    QString name;        // "Engineer", shown (a built-in's is translated)
    QString description; // one line for the teammate's page
    QString avatarUrl;   // qrc:/… or file:///…
    QString glyph;       // AvatarGlyphs id
    QColor  color;
    QString prompt;     // what the role adds to Claude Code's prompt; "" = nothing
    QString promptName; // the name in the header line (a built-in's stays English)
    bool    builtIn = false;
    bool    edited  = false; // a built-in changed from its default
    bool    removed = false; // no longer on the team; its sessions keep it
    bool    former  = false; // only known from a session's transcript
    qint64  created = 0;     // epoch ms; orders the added teammates
};

// What --append-system-prompt gets for `role`: the header line, then its
// prompt; "" when the role adds nothing (the plain Generalist).
QString appendedPrompt(const Role &role);
// "# Your role: <name> (msga: <id>)".
QString roleHeader(const QString &name, const QString &id);
// What --agents gets: `roles` (the listed team) as Claude Code subagent types,
// one per role that adds a prompt, named by its id — so "use @Engineer", which
// reaches Claude as "@claude:role:engineer", finds subagent_type "engineer".
// For a subagent --agent's replacing the prompt is the point: every subagent
// type works that way. An id Claude Code's own types already use is left out.
QString subagentsJson(const std::vector<Role> &roles);

// The role named by our part of a recorded system prompt.
struct RoleMark {
    QString id;   // "" = none of ours
    QString name; // as the header has it
};
// From a prompt_snapshot's `systemPrompt`.
RoleMark roleInSystemPrompt(const QJsonArray &systemPrompt);
// From raw transcript bytes (the JSON-escaped header, possibly cut off after
// its line) — for reading only a transcript's ends.
RoleMark roleInTranscriptBytes(const QByteArray &bytes);

// The built-ins as they come, the Generalist first.
const std::vector<Role> &builtInRoles();

class Team {
public:
    // `dir` holds the teammate files (created on the first save).
    explicit Team(QString dir);

    // Everyone known: the built-ins, then the added teammates (removed ones
    // included) in the order they were added.
    const std::vector<Role>       &roles() const { return _roles; }
    // The team as listed: without the removed ones.
    std::vector<Role>              listed() const;
    const Role                    &generalist() const { return _roles.front(); }
    // nullptr when unknown.
    const Role                    *find(const QString &id) const;
    // `id` as shown: a known role, a former teammate (named `nameHint`, or as
    // noted before), or the Generalist for "".
    Role                           resolve(const QString &id, const QString &nameHint = {}) const;
    // Remembers what a transcript calls a role nobody here knows; true when
    // that's news.
    bool                           noteFormer(const QString &id, const QString &name);
    // Former teammates seen so far: id → name.
    const QHash<QString, QString> &formers() const { return _formers; }

    // Adds (`role.id` empty) or updates a teammate; its id, "" on failure
    // (*error says why).
    QString save(Role role, QString *error = nullptr);
    // Takes an added teammate off the team (not a built-in).
    bool    remove(const QString &id);
    // A built-in back to how it comes.
    bool    restore(const QString &id);

private:
    void    load();
    bool    write(const Role &role, QString *error);
    QString avatarUrlFor(const Role &role) const;
    QString newId(const QString &name) const;

    QString                 _dir;
    std::vector<Role>       _roles;
    QHash<QString, QString> _formers;
};

} // namespace claude_code
