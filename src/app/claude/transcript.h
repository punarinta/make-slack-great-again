// Claude Code transcript (~/.claude/projects/<slug>/<sessionId>.jsonl) → the
// items msga shows as messages.
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
//   • Claude Code's "Not logged in" answer → AssistantText marked loginError;
//   • assistant text → AssistantText, marked Progress when the same turn goes on
//     to call tools (no notification for those), Final when the turn ends after
//     it, and Pending while that isn't known yet (the last text of a live turn);
//   • consecutive tool calls → one ToolGroup item (grows as calls arrive);
//   • an Agent/Task call → a Subagent item of its own — it roots a thread holding
//     the subagent's own transcript (subagents/agent-<agentId>.jsonl);
//   • msga's relay of a reply in that thread (subagentReplyPrompt) → a UserPrompt
//     of the reply alone, marked with the subagent it went to (relayTo);
//   • what another agent sent the session (a "peer" turn: a subagent's hand-back
//     of its report, a message from another Claude session) → PeerMessage —
//     Claude Code records it as a user turn, but nobody typed it.
// Thinking, attachments, file-history, cost and mode records are hidden.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace json {
class Document;
class Value;
} // namespace json

namespace claude {

// What a transcript line says at its top, read without making a document of
// it — a tool's output, most of a transcript's bytes, runs to megabytes of
// escaped text that is only skipped here. Each field is the first member of
// that name, "" when it isn't a string (as json::Value::str() reads it).
struct RecordSkim {
    std::string_view type, uuid, timestamp, version, permissionMode, cwd;
    size_t           members = 0;     // the record's
    bool             origin  = false; // an "origin" object
    bool             isMeta = false, isCompactSummary = false, isSidechain = false;
    bool             contentArray = false; // message.content is an array…
    bool             image        = false; // …with an "image" block
    struct ToolResult {
        std::string_view id; // tool_use_id
        bool             error = false;
    };
    std::vector<ToolResult> toolResults;     // …its "tool_result" blocks, in order
    std::string_view        agentId, status; // toolUseResult's
    // toolUseResult's: a command that went on in the background (the record
    // is then read whole, for where its output goes).
    std::string_view        backgroundTaskId;
};
// Fills *out from `line`; false when it isn't one valid JSON object, or has a
// string this can't hand out as is (one of the fields above, or a member name
// on the way to them, with escapes) — then json::Document reads it.
bool skimRecord(std::string_view line, RecordSkim *out);

struct ToolCall {
    std::string toolUseId;
    std::string name;    // "Bash", "Read", "mcp__x__y", …
    std::string summary; // one line: the command, path, pattern, url, …
    bool        error                              = false;
    bool        operator==(const ToolCall &) const = default;
};

struct TranscriptItem {
    enum class Kind : uint8_t { UserPrompt, AssistantText, ToolGroup, Subagent, PeerMessage };
    enum class State : uint8_t { Final, Progress, Pending };

    Kind                  kind  = Kind::UserPrompt;
    State                 state = State::Final; // AssistantText only; others are Final
    // Epoch micros, unique and increasing within the parser: the record's own
    // time, bumped past the previous item's when it isn't later (see nextTs).
    int64_t               ts    = 0;
    std::string           text;      // prompt / markdown answer / subagent description
    std::vector<ToolCall> tools;     // ToolGroup: the calls; Subagent: the one call
    std::string           agentId;   // Subagent: set once the call's result arrives;
                                     // PeerMessage: the subagent handing back
    std::string           agentType; // Subagent: the call's subagent_type ("designer")
    std::string           agentRole; // Subagent: the teammate its prompt names (roleInAgentPrompt)
    std::string relayTo;  // UserPrompt: a reply in this subagent's thread (text = the reply alone)
    std::string peerName; // PeerMessage from another session: that session's name
    std::vector<std::string>
        images; // UserPrompt: pasted images and sent files, in msga's cache (paths)
    // Parallel to images: "Image 3.png", after Claude Code's paste number.
    std::vector<std::string> imageNames;
    // AssistantText: Claude Code's own answer to a turn it couldn't run for want
    // of a login ("Not logged in · Please run /login") — shown as msga's
    // explanation, since /login can't be run from msga.
    bool                     loginError = false;
    // UserPrompt, AssistantText: the transcript record it was read from, which
    // removeFromTranscript takes out; "" for the rest (a tool call can't go
    // without its result).
    std::string              uuid;
    bool                     operator==(const TranscriptItem &) const = default;
};

class TranscriptParser {
public:
    // Feed bytes appended to the file since the last call. A trailing partial
    // line is buffered until its newline arrives.
    void feed(std::string_view bytes);

    const std::vector<TranscriptItem> &items() const { return _items; }
    // A revision per item, parallel to items(): it changes whenever that item
    // does (added, or changed by a later record: a tool's result, a pending
    // text resolved, a call joining its group), never otherwise — what was
    // made of an item is still good while its revision is the same. Unique
    // within the parser, never 0.
    uint64_t                           revision(size_t index) const;
    // The newest revision handed out: unchanged = no item changed.
    uint64_t                           revision() const { return _rev; }

    // True while the last turn has not ended (no turn_duration record after the
    // latest prompt) — the session is, or was when it stopped, mid-turn.
    bool    turnOpen() const { return _turnOpen; }
    // Epoch micros of the record that opened the last turn; 0 = none yet.
    int64_t turnStartedAt() const { return _turnStartedAt; }
    // Items from here on get a ts after `micros` — taken by a message msga
    // shows of its own (a prompt on its way), which no item may collide with.
    void    reserveTs(int64_t micros);

    // The session's own title, when Claude Code generated one ("ai-title").
    const std::string &aiTitle() const { return _aiTitle; }

    // Epoch micros of the latest turn that failed for want of a login; 0 = none.
    int64_t loginFailedAt() const { return _loginFailedAt; }

    // Epoch micros of the newest record seen (any type) — "last activity".
    int64_t                     lastActivity() const { return _lastActivity; }
    // Epoch micros of every record read, in file order — when a subagent's run
    // began (Backend::pumpTyping). Kept only once asked for (keepActivity:
    // subagents' parsers), from then on.
    const std::vector<int64_t> &activity() const { return _activity; }
    void                        keepActivity() { _keepActivity = true; }
    // Epoch micros of the latest notification that a background task (a
    // subagent: its agentId) stopped; 0 = none yet. One arrives each time it
    // stops — it may start again, on its own or for a relayed reply. A
    // foreground subagent's stop is its Agent call's result.
    int64_t                     taskStoppedAt(const std::string &taskId) const;
    // A command Claude runs in the background (Bash run_in_background, or one
    // moved there): its output is written to `outputPath` as it comes, and
    // its stop is a task notification. Verified with Claude Code 2.1.323: the
    // call's result has toolUseResult.backgroundTaskId and says "Output is
    // being written to: <path>" (/tmp/claude-<uid>/<project>/<session>/tasks/
    // <id>.output).
    struct BackgroundCommand {
        std::string taskId, outputPath;
        std::string command;   // the call's summary, "" when not found
        int64_t     startedAt; // epoch micros
    };
    // Those not stopped as far as the transcript says, oldest first.
    std::vector<BackgroundCommand> runningCommands() const;
    // How a background task (a command) ended, as its notification says
    // ("<status>completed|failed</status>", "… exit code 3" in its summary),
    // or stopped by Claude (a TaskStop / KillShell call, which no
    // notification follows — verified with 2.1.323).
    struct TaskEnd {
        enum class Kind : uint8_t { Done, Failed, Stopped };
        int64_t at       = 0; // epoch micros
        Kind    kind     = Kind::Done;
        int     exitCode = 0;
    };
    // Null while it hasn't ended (as far as the transcript says).
    const TaskEnd *taskEnd(const std::string &taskId) const;

    // As of the newest record that says: the Claude Code version that wrote
    // it, the model that answered, the permission mode of the last prompt.
    const std::string &version() const { return _version; }
    const std::string &model() const { return _model; }
    const std::string &permissionMode() const { return _permissionMode; }
    // The team role the session was started with (roles.h), read from the
    // system prompt Claude Code recorded; "" = none of ours (a Generalist).
    const std::string &role() const { return _role; }
    const std::string &roleName() const { return _roleName; } // as the prompt names it
    // The subagent types the session offers, as Claude Code last listed them
    // ("agent_listing_delta" records): a session started without --agents has
    // none of the team's (see teammateNote).
    const std::unordered_set<std::string> &agentTypes() const { return _agentTypes; }
    // Whether a record `uuid` was read (a copy's repeats are recognised by it).
    bool                                   hasRecord(std::string_view uuid) const;

private:
    // One line; `doc` and `skim` are the feed's, reused line after line.
    void handleLine(std::string_view line, json::Document &doc, RecordSkim &skim);
    // A record of tool results alone, read off its skim; false when it takes
    // the whole document (handleUser).
    bool handleToolResults(const RecordSkim &r);
    // Where the record's fields go before its type is looked at; false when
    // it was read already (a copy's repeat).
    bool beginRecord(
        std::string_view uuid,
        std::string_view timestamp,
        std::string_view version,
        std::string_view permissionMode,
        int64_t         *micros
    );
    void toolResult(
        std::string_view id,
        bool             failed,
        std::string_view agentId,
        std::string_view status,
        int64_t          micros
    );
    void    touch(size_t index); // _items[index] changed (see revision)
    void    handleUser(const json::Value &o, const json::Value &content, int64_t micros);
    void    handleAssistant(const json::Value &o, const json::Value &message, int64_t micros);
    int64_t nextTs(int64_t micros);
    void    closeToolGroup();
    void    resolvePendingText(TranscriptItem::State state);
    void    endTurn();
    void    openTurn(int64_t micros);
    // A "<task-notification>…" the session was sent: its task(s) stopped at `micros`.
    void    noteTaskNotification(std::string_view text, int64_t micros);
    void    noteTaskStopped(std::string_view taskId, int64_t micros);
    void    noteBackgroundCommand(
        std::string_view   taskId,
        std::string_view   toolUseId,
        const json::Value &content,
        int64_t            micros
    );
    // What another agent sent the session (`origin` of kind "peer"), as a
    // PeerMessage; false when `origin` isn't a peer's. `newTurn`: it arrived on
    // its own rather than queued into a turn under way.
    bool addPeerMessage(const json::Value &origin, int64_t micros, bool newTurn);
    // What a command Claude Code runs itself printed: an answer, and the turn's end.
    void addCommandOutput(std::string_view output, int64_t micros);
    void addPrompt(
        std::string_view         text,
        int64_t                  micros,
        std::vector<std::string> images     = {},
        std::vector<std::string> imageNames = {}
    );

    std::string                              _partial;
    std::vector<TranscriptItem>              _items;
    std::vector<uint64_t>                    _revs; // parallel to _items, caught up after each line
    uint64_t                                 _rev          = 0;
    int64_t                                  _lastMicros   = 0;
    int64_t                                  _lastActivity = 0;
    std::vector<int64_t>                     _activity;
    bool                                     _keepActivity = false;
    std::unordered_map<std::string, int64_t> _taskStopped;      // by task id
    std::vector<BackgroundCommand>           _commands;         // newest last, stopped dropped
    std::unordered_map<std::string, TaskEnd> _taskEnds;         // by task id
    std::unordered_map<std::string, std::string> _bashCommands; // Bash calls awaiting results
    int         _openToolGroup = -1;                            // index into _items, -1 when none
    int         _pendingText   = -1; // index of the Pending text, -1 when none
    int         _commandOutput = -1; // index of the latest command output, -1 when none
    bool        _turnOpen      = false;
    int64_t     _turnStartedAt = 0;
    int64_t     _loginFailedAt = 0;
    std::string _aiTitle;
    std::string _version;
    std::string _model;
    std::string _permissionMode;
    std::string _role;
    std::string _roleName;
    std::unordered_set<std::string> _agentTypes;
    std::string                     _lineUuid; // the record being read
    // Every record read (its uuid's hash), so a copy's repeats are skipped.
    std::unordered_set<uint64_t>    _seenUuids;
};

// What msga sends the session for a reply in a subagent's thread: there is no
// way to type to a subagent, but the session can pass a message on to one — a
// finished one too — with its SendMessage tool, addressed by the agentId. The
// subagent's transcript then records it as the coordinator's (hidden, isMeta),
// so the thread shows this prompt, parsed back into the reply (relayTo).
std::string subagentReplyPrompt(std::string_view agentId, std::string_view reply);

// What the user typed, out of a prompt msga sent: without msga's teammate note,
// and a relayed thread reply (subagentReplyPrompt) as the reply alone, its
// subagent in `relayTo`.
std::string typedPrompt(std::string_view prompt, std::string *relayTo = nullptr);

// Text pasted into Claude Code's prompt box is recorded wrapped in
// <pasted_content id="e482">…</pasted_content id="e482"> (2.1.278 on); shown
// as a "> " quote instead. Anything but a matched pair is left as it is.
std::string quotePastes(std::string_view prompt);

// What someone typed, from a transcript record of type "user", as the chat
// shows it (slash commands as "/name args", msga's additions taken off as
// typedPrompt does); "" for what nobody typed — tool output, what Claude Code
// tells the model, a command's terminal output, notifications and every other
// machine-made turn. Pasted images aren't read.
std::string promptOfRecord(const json::Value &record);

// Claude Code's prompt history — history.jsonl, the list its prompt box steps
// through with ↑ — for the sessions of folder `project`, newest first: session
// `sessionId`'s own prompts, then the folder's other sessions' (no `sessionId`:
// all the folder's prompts alike). Up to `max`, as many as Claude Code offers.
// msga's prompts are in it too (they're typed into the session) and come back
// as typed (typedPrompt); long pastes come back whole (inline, or
// <pasteDir>/<contentHash>.txt); pasted images are dropped.
std::vector<std::string> promptHistory(
    std::string_view historyPath,
    std::string_view pasteDir,
    std::string_view project,
    std::string_view sessionId,
    int              max = 100
);
// promptHistory for the UI thread: only once the folder's history was read
// (an earlier call, a worker's read ahead) — then just what was appended
// since is read — and never waiting on a worker busy with the file. False
// (*out empty) when that is still to do: read it on a worker first.
bool loadedPromptHistory(
    std::string_view          historyPath,
    std::string_view          pasteDir,
    std::string_view          project,
    std::string_view          sessionId,
    std::vector<std::string> *out,
    int                       max = 100
);

// Whether the transcript at `path` has a prompt or an answer (any user or
// assistant record) after the record `uuid` — whether a session went on
// since the record a branch of it was forked at. True when that record isn't
// there (or `uuid` is ""): nothing says it didn't. Read from the end, only
// as far back as the answer needs. Blocking: a worker's.
bool hasTurnAfter(std::string_view path, std::string_view uuid);

// The uuids of up to `max` records just before the record `uuid` in the
// transcript at `path`, newest first (none when `uuid` isn't there) — of a
// branch, the records it copied from its session end there. Read from the
// end. Blocking: a worker's.
std::vector<std::string> recordsBefore(std::string_view path, std::string_view uuid, size_t max);

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
bool removeFromTranscript(
    std::string_view path, std::string_view uuid, std::string *error = nullptr
);

// Whether the transcript at `path` has a conversation record (a prompt or an
// answer) past byte `from`. What Claude Code appends besides — last-prompt,
// cost-state and title records, e.g. when its daemon retires an idle
// background worker — is no activity in the session. With `afterMs`, only a
// record timestamped after it counts.
bool hasTurnSince(std::string_view path, int64_t from, int64_t afterMs = 0);

// A pasted image (a prompt's base64 "image" block) saved once in msga's cache
// (<dirs().cache>/images), named by `key` (the record's uuid and the image's
// place in it: found again without hashing it), else by its content hash;
// returns the file's path ("" when it can't be saved).
std::string
cachePastedImage(std::string_view mediaType, std::string_view base64, std::string_view key = {});

// Files sent with a message ride its prompt as "@path" mentions, which Claude
// Code expands into attachments (an image arrives as an image) — the same in a
// `--bg` prompt as typed into a live worker's prompt box. So the files are
// copied into msga's cache first (cacheUpload: <sha1>/<name>, where no temp
// folder cleanup reaches them), and the parser turns mentions of that folder
// back into the message's files (takeAttachments). Typed live, a mention must
// not end the prompt: the terminal UI's completion list takes the Enter
// (verified with Claude Code 2.1.283, 2026-09-26) — so the mentions go first,
// with a stand-in text after them when the message has none.
std::string uploadsDir();                       // <dirs().cache>/uploads
std::string cacheUpload(std::string_view path); // "" when it can't be copied
std::string withAttachments(std::string_view text, const std::vector<std::string> &paths);
std::string takeAttachments(std::string_view prompt, std::vector<std::string> *paths);

// Human line for one tool call's input ("git status", "src/main.cpp", …).
std::string summarizeToolInput(std::string_view toolName, const json::Value &input);

// Visible = shown in history / announced as new. A Pending text is only shown
// once the session is no longer working on that turn (`sessionBusy` false).
inline bool isVisible(const TranscriptItem &item, bool sessionBusy) {
    return item.state != TranscriptItem::State::Pending || !sessionBusy;
}

} // namespace claude
