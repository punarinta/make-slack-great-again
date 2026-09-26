// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Claude Code transcript (~/.claude/projects/<slug>/<sessionId>.jsonl) → the
// items msga shows as messages (docs/backend-modules-plan.md §5.2).
//
// The transcript is Claude Code's internal, unversioned format, so this parser
// is deliberately forgiving: unknown record types and fields are skipped, never
// errors. It is fed incrementally (a live transcript is appended to while the
// session runs) and produces exactly the same items whether the file arrives in
// one piece or line by line — item ids (ts) are derived from record timestamps
// plus a per-parser tie-breaker, never from wall-clock time.
//
// What becomes an item:
//   • a typed prompt (user record with string/text content, or a prompt queued
//     mid-turn) → UserPrompt; slash commands are shown as "/name args";
//     task notifications and other system injections are hidden;
//   • what a command Claude Code runs itself prints (/context, /compact) →
//     AssistantText, ending the turn;
//   • assistant text → AssistantText, marked Progress when the same turn goes on
//     to call tools (no notification for those), Final when the turn ends after
//     it, and Pending while that isn't known yet (the last text of a live turn);
//   • consecutive tool calls → one ToolGroup item (grows as calls arrive);
//   • an Agent/Task call → a Subagent item of its own — it roots a thread holding
//     the subagent's own transcript (subagents/agent-<agentId>.jsonl);
//   • msga's relay of a reply in that thread (subagentReplyPrompt) → a UserPrompt
//     of the reply alone, marked with the subagent it went to (relayTo).
// Thinking, attachments, file-history, cost and mode records are hidden.
#pragma once

#include "backend/domain.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <vector>

namespace claude_code {

struct ToolCall {
    QString toolUseId;
    QString name;    // "Bash", "Read", "mcp__x__y", …
    QString summary; // one line: the command, path, pattern, url, …
    bool    error                              = false;
    bool    operator==(const ToolCall &) const = default;
};

struct TranscriptItem {
    enum class Kind { UserPrompt, AssistantText, ToolGroup, Subagent };
    enum class State { Final, Progress, Pending };

    Kind                  kind  = Kind::UserPrompt;
    State                 state = State::Final; // AssistantText only; others are Final
    Ts                    ts;                   // "secs.micros", unique within the transcript
    qint64                date = 0;             // epoch micros
    QString               text;                 // prompt / markdown answer / subagent description
    std::vector<ToolCall> tools;                // ToolGroup: the calls; Subagent: the one call
    QString               agentId;              // Subagent: set once the call's result arrives
    QString               agentType;            // Subagent: the call's subagent_type ("designer")
    QString               agentRole; // Subagent: the teammate its prompt names (roleInAgentPrompt)
    QString     relayTo; // UserPrompt: a reply in this subagent's thread (text = the reply alone)
    QStringList images;  // UserPrompt: pasted images and sent files, in msga's cache (paths)
    QStringList imageNames; // parallel to images: "Image 3.png", after Claude Code's paste number
    // UserPrompt, AssistantText: the transcript record it was read from, which
    // removeFromTranscript takes out; "" for the rest (a tool call can't go
    // without its result).
    QString     uuid;
    bool        operator==(const TranscriptItem &) const = default;
};

class TranscriptParser {
public:
    // Feed bytes appended to the file since the last call. A trailing partial
    // line is buffered until its newline arrives.
    void feed(const QByteArray &bytes);

    const std::vector<TranscriptItem> &items() const { return _items; }

    // True while the last turn has not ended (no turn_duration record after the
    // latest prompt) — the session is, or was when it stopped, mid-turn.
    bool   turnOpen() const { return _turnOpen; }
    // Epoch micros of the record that opened the last turn; 0 = none yet.
    qint64 turnStartedAt() const { return _turnStartedAt; }
    // Items from here on get a ts after `micros` — taken by a message msga
    // shows of its own (a prompt on its way), which no item may collide with.
    void   reserveTs(qint64 micros) { _lastMicros = std::max(_lastMicros, micros); }

    // The session's own title, when Claude Code generated one ("ai-title").
    const QString &aiTitle() const { return _aiTitle; }

    // Epoch micros of the newest record seen (any type) — "last activity".
    qint64                     lastActivity() const { return _lastActivity; }
    // Epoch micros of every record read, in file order — when a subagent's run
    // began (Backend::pumpTyping).
    const std::vector<qint64> &activity() const { return _activity; }
    // Epoch micros of the latest notification that a background task (a
    // subagent: its agentId) stopped; 0 = none yet. One arrives each time it
    // stops — it may start again, on its own or for a relayed reply.
    qint64 taskStoppedAt(const QString &taskId) const { return _taskStopped.value(taskId); }

    // As of the newest record that says: the Claude Code version that wrote
    // it, the model that answered, the permission mode of the last prompt.
    const QString &version() const { return _version; }
    const QString &model() const { return _model; }
    const QString &permissionMode() const { return _permissionMode; }
    // The team role the session was started with (cc_roles), read from the
    // system prompt Claude Code recorded; "" = none of ours (a Generalist).
    const QString &role() const { return _role; }
    const QString &roleName() const { return _roleName; } // as the prompt names it

private:
    void handleLine(const QByteArray &line);
    Ts   nextTs(qint64 micros, qint64 *outDate);
    void closeToolGroup();
    void resolvePendingText(TranscriptItem::State state);
    void endTurn();
    void openTurn(qint64 micros);
    // A "<task-notification>…" the session was sent: its task(s) stopped at `micros`.
    void noteTaskNotification(const QString &text, qint64 micros);
    // What a command Claude Code runs itself printed: an answer, and the turn's end.
    void addCommandOutput(const QString &output, qint64 micros);
    void addPrompt(
        const QString     &text,
        qint64             micros,
        const QStringList &images     = {},
        const QStringList &imageNames = {}
    );
    QByteArray                  _partial;
    std::vector<TranscriptItem> _items;
    qint64                      _lastMicros   = 0;
    qint64                      _lastActivity = 0;
    std::vector<qint64>         _activity;
    QHash<QString, qint64>      _taskStopped;        // by task id
    int                         _openToolGroup = -1; // index into _items, -1 when none
    int                         _pendingText   = -1; // index of the Pending text, -1 when none
    int           _commandOutput = -1; // index of the latest command output, -1 when none
    bool          _turnOpen      = false;
    qint64        _turnStartedAt = 0;
    QString       _aiTitle;
    QString       _version;
    QString       _model;
    QString       _permissionMode;
    QString       _role;
    QString       _roleName;
    QString       _lineUuid;  // the record being read
    QSet<QString> _seenUuids; // every record read, so a copy's repeats are skipped
};

// What msga sends the session for a reply in a subagent's thread: there is no
// way to type to a subagent, but the session can pass a message on to one — a
// finished one too — with its SendMessage tool, addressed by the agentId. The
// subagent's transcript then records it as the coordinator's (hidden, isMeta),
// so the thread shows this prompt, parsed back into the reply (relayTo).
QString subagentReplyPrompt(const QString &agentId, const QString &reply);

// What the user typed, out of a prompt msga sent: without msga's teammate note,
// and a relayed thread reply (subagentReplyPrompt) as the reply alone, its
// subagent in `relayTo`.
QString typedPrompt(const QString &prompt, QString *relayTo = nullptr);

// Claude Code's prompt history — history.jsonl, the list its prompt box steps
// through with ↑ — for the sessions of folder `project`, newest first: session
// `sessionId`'s own prompts, then the folder's other sessions'. Up to `max`, as
// many as Claude Code offers. msga's prompts are in it too (they're typed into
// the session) and come back as typed (typedPrompt); long pastes come back
// whole (inline, or <pasteDir>/<contentHash>.txt); pasted images are dropped.
QStringList promptHistory(
    const QString &historyPath,
    const QString &pasteDir,
    const QString &project,
    const QString &sessionId,
    int            max = 100
);

// Takes the record `uuid` (a prompt or an answer: TranscriptItem::uuid) out of
// the transcript at `path`, so the session no longer has it when it resumes.
// The thinking behind it goes too (an answer's, or for a prompt the whole
// turn's), since Claude would recall the prompt from it, and records that
// followed the removed ones are linked to what preceded them — Claude Code
// reads a conversation back along those links, and a broken one loses
// everything before it. Claude Code's bookkeeping copies of a prompt's text go
// as well. Rewrites the file in place: nothing may be writing to
// it. False, with *error, when the record isn't there or the file can't be
// rewritten.
bool removeFromTranscript(const QString &path, const QString &uuid, QString *error = nullptr);

// Whether the transcript at `path` has a conversation record (a prompt or an
// answer) past byte `from`. What Claude Code appends besides — last-prompt,
// cost-state and title records, e.g. when its daemon retires an idle
// background worker — is no activity in the session. With `afterMs`, only a
// record timestamped after it counts.
bool hasTurnSince(const QString &path, qint64 from, qint64 afterMs = 0);

// A pasted image (a prompt's base64 "image" block) saved once in msga's cache,
// named by its content hash; returns the file's path ("" when it can't be saved).
QString cachePastedImage(const QString &mediaType, const QByteArray &base64);

// Files sent with a message ride its prompt as "@path" mentions, which Claude
// Code expands into attachments (an image arrives as an image) — the same in a
// `--bg` prompt as typed into a live worker's prompt box. So the files are
// copied into msga's cache first (cacheUpload: <sha1>/<name>, where no temp
// folder cleanup reaches them), and the parser turns mentions of that folder
// back into the message's files (takeAttachments). Typed live, a mention must
// not end the prompt: the terminal UI's completion list takes the Enter
// (verified with Claude Code 2.1.283, 2026-09-26) — so the mentions go first,
// with a stand-in text after them when the message has none.
QString uploadsDir();
QString cacheUpload(const QString &path); // "" when it can't be copied
QString withAttachments(const QString &text, const QStringList &paths);
QString takeAttachments(const QString &prompt, QStringList *paths);

// Human line for one tool call's input ("git status", "src/main.cpp", …).
QString summarizeToolInput(const QString &toolName, const QJsonObject &input);

// Claude's markdown → msga's text model (via the composer's CommonMark→mrkdwn
// conversion and the mrkdwn parser), with headings bolded and tables kept
// monospaced so they stay aligned.
TextWithEntities renderMarkdown(const QString &markdown);

// A message with markdown tables, as blocks in reading order: "rich_text" for
// the text around them, "table" (cells rendered like the text, header row bold)
// for each table — drawn as a real grid, like Slack's table messages. Empty
// when there is no table: the message text alone renders it.
std::vector<Block> markdownBlocks(const QString &markdown);

// The message msga shows for an item. `claude` is the session's assistant user.
Message toMessage(const TranscriptItem &item, const UserId &me, const UserId &claude);

// Visible = shown in history / announced as new. A Pending text is only shown
// once the session is no longer working on that turn (`sessionBusy` false).
inline bool isVisible(const TranscriptItem &item, bool sessionBusy) {
    return item.state != TranscriptItem::State::Pending || !sessionBusy;
}

// The ts format used for items: "secs.micros" with zero-padded micros, so string
// order == time order (Session compares lastRead/latestTs as strings).
Ts microsToTs(qint64 micros);

} // namespace claude_code
