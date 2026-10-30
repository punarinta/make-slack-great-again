// What backend.cpp, backend_send.cpp and backend_manage.cpp share: a tracked
// session, the constants, small helpers. Private to the Claude Code backend.
#pragma once

#include "app/claude/attach.h"
#include "app/claude/backend.h"
#include "app/claude/vt.h"
#include "app/claude/worktrees.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace claude {

// Who Claude's messages (and "is thinking…") come from: the session's
// teammate — one user per role (roles.h), shared by all its sessions. Each
// session's own user ("claude:<conv>") only names the DM and carries its dot.
// The generalist keeps the id the one generic "Agent" had.
inline constexpr char kMeId[]            = "me";
inline constexpr char kAgentUser[]       = "claude:agent";
inline constexpr char kRoleUserPrefix[]  = "claude:role:";
inline constexpr char kAssistantPrefix[] = "claude:";
inline constexpr char kGeneralist[]      = "generalist";
inline constexpr char kNewPrefix[]       = "new-"; // conversation id of a "+" session

// A turn msga sent that shows no sign of life (no prompt in the transcript, no
// busy worker) is given up on after this long.
inline constexpr int64_t kLaunchTimeoutMs = 60'000;
// How long a login `claude auth status` confirmed is taken as still there
// before a send checks again. One found missing is checked again every time.
inline constexpr int64_t kLoginFreshMs    = 5 * 60'000;
// After our prompt landed: a session that went quiet without writing the
// turn's end (interrupted, crashed) stops counting as busy after this long.
inline constexpr int64_t kQuietTurnMs     = 15'000;
// Typing a message into a live worker found its terminal UI busy with
// something else (a permission question, a panel): tried again this much later.
inline constexpr int64_t kTypeRetryMs     = 5'000;

inline constexpr int kApprovalReads       = 3;      // quick tries at reading a question's options
inline constexpr int kApprovalRetryMs     = 4'000;  // between them
inline constexpr int kApprovalSlowRetryMs = 30'000; // and after them, for as long as it waits
// A session with msga's permission hook: how long its question may take to
// come through the hook before it's read off the screen instead.
inline constexpr int kHookQuestionMs      = 3'000;

int64_t     nowMs();
// `path` from its start into `parser`, a megabyte at a time, as far as it
// reads now: the bytes fed (-1 when it can't be read). Blocking: a worker's.
int64_t     feedTranscript(const std::string &path, TranscriptParser &parser);
std::string loginName();
// A background session stopped on a permission prompt: it reads "working" +
// "approve Bash: …" (verified 2026-09-25). Only its terminal can answer that
// one (msga does, see readApproval); a plain question ("blocked" with the
// question as needs) is answered by message. A worker that has exited took
// its prompt with it: a message resumes the session.
bool        awaitsApproval(const SessionInfo &s);
// A background session whose turn failed for want of a login reads "blocked"
// + "login required — run /login" (verified 2.1.283): not a question for the user.
bool        needsLogin(const SessionInfo &s);

// What a message looks like, for telling a changed one from the same: what
// it says (who, its text, files, buttons…), hashed once per rendered
// message, then where it sits and what it gathered (ts, thread, replies,
// reactions) mixed in. fingerprint(m) = fingerprint(contentFingerprint(m), m's).
uint64_t contentFingerprint(const model::Message &m);
uint64_t fingerprint(
    uint64_t                            content,
    model::Ts                           ts,
    model::Ts                           threadTs,
    uint32_t                            replyCount,
    model::Ts                           latestReply,
    const std::vector<model::Reaction> &reactions
);
uint64_t fingerprint(const model::Message &m);

// The key an answer's output files are kept under (OutputContext::messageKey);
// a subagent's (`agentId`) apart from the session's.
std::string outputKey(const TranscriptItem &item, const std::string &agentId);

// How many items two transcripts start with in common (a fork's copy).
size_t      sharedStart(const std::vector<TranscriptItem> &a, const std::vector<TranscriptItem> &b);
std::string knownSessionsPath();
std::string profilePath();

// A transcript item as a message: made again only when the item's revision
// (TranscriptParser::revision) moves on — 0 = to be made again.
struct Backend::Rendered {
    uint64_t       rev       = 0;
    uint64_t       contentFp = 0; // contentFingerprint(msg)
    model::Message msg;
};

// One message a list shows, as a recipe: a rendered message (src[index]) or
// one of its own, with where it sits and what it gathered on top. Taken
// apart so a sync tells what changed by `fp` alone and copies only that
// (make). Valid until the next render of its source.
struct Backend::Visible {
    model::Ts                             ts = 0, threadTs = 0, latestReply = 0;
    uint32_t                              replyCount = 0;
    model::UserRef                        user       = model::kNoUser;
    bool                                  progress   = false; // subtype kProgressSubtype
    const std::vector<Rendered>          *src        = nullptr;
    size_t                                index      = 0;
    std::shared_ptr<const model::Message> own;
    const std::vector<model::Reaction>   *reactions = nullptr;
    uint64_t                              contentFp = 0;
    uint64_t                              fp        = 0; // fingerprint(make()): applyReactions

    // An item listed unrendered (visibleList's renderFrom) has neither: `index`
    // is the item's, Backend::realize renders it.
    bool                  rendered() const { return own || src; }
    const model::Message &base() const { return own ? *own : (*src)[index].msg; }
};

// A subagent's own transcript, read as it grows (like a session's, tail):
// its items, and its thread's messages as rendered last.
struct Backend::SubagentFeed {
    TranscriptParser      parser;
    int64_t               offset     = 0;
    // Its first read, a long one, under way on a worker (the one numbered
    // parseToken): nothing is read here meanwhile.
    bool                  parsing    = false;
    uint64_t              parseToken = 0;
    int                   zenCount   = 0; // items but the tool-call cards
    uint64_t              counted    = 0; // the parser revision zenCount is of
    model::UserRef        renderedMe = model::kNoUser, renderedAuthor = model::kNoUser;
    std::vector<Rendered> rendered; // parallel to parser.items()
};

struct Backend::Tracked {
    std::string        convId;
    SessionInfo        info; // latest roster data; sessionId empty until a "+" session started
    bool               listed = false; // in the latest roster scan
    std::string        transcriptPath;
    TranscriptParser   parser;
    int64_t            offset               = 0;
    model::Ts          lastRead             = 0;
    bool               skipPermissionChecks = false; // a "+" session not started yet
    std::string        localName; // the user's own name for it ("Rename session…")
    // The teammate msga started it with (roles.h); the transcript's own record
    // of it wins once there is one (roleOf).
    std::string        role;
    // msga's own marks on it (Claude Code has no such thing).
    bool               starred = false, muted = false;
    model::NotifyLevel notify = model::NotifyLevel::Default;
    model::ConvRef     ref    = model::kNoConv; // its conversation in the Store

    model::UserRef renderedAuthor = model::kNoUser; // who `rendered` has Claude's messages from
    // Rendered messages, parallel to parser.items(): re-rendered only when the
    // item changed, so a growing transcript doesn't re-parse all its markdown.
    std::vector<Rendered> rendered;
    // What was last taken as shown (all of it, whether the Store has its
    // history or not): by ts, whose and what kind — new ones are news (its
    // own prompt landing), and the unread counts are made of them.
    using Seen = Backend::Seen;
    std::map<model::Ts, Seen> announced;
    bool                      announcedInit     = false;
    bool                      announcedAsThread = false; // how `announced` was taken
    bool                      lastBusy          = false;
    int64_t     busySinceMs = 0;     // epoch ms the turn under way began (pumpTyping); 0 = not busy
    bool        wasLive     = false; // listed or sending at the previous refresh
    // Its user as last pushed (syncMeta): only pushed when it changed.
    std::string userSig;
    // `announced` changes (announcedGen), and what syncMeta counted in it as of
    // which change and read mark.
    uint64_t    announcedGen = 0, countedGen = 0;
    model::Ts   countedRead = -1;
    uint32_t    unread = 0, mentions = 0;
    model::Ts   latest    = 0;
    uint64_t    syncedKey = 0; // syncKey() after the last sync
    // Where its Subagent items are in parser.items(), as of subagentsKey
    // (the parser's revision, items and offset then).
    std::vector<size_t>                subagentItems;
    uint64_t                           subagentsKey = 0;
    // Its transcript read anew: `p` holds its first `from` bytes (none: from
    // the start), nothing rendered of it yet.
    void                               restart(TranscriptParser &&p = {}, int64_t from = 0);
    // The whole transcript is being read on a worker (parseOnWorker,
    // Backend::remove): nothing is read here meanwhile, nothing synced; then
    // these run.
    bool                               parsing = false;
    std::vector<std::function<void()>> afterParse;

    // Sending: messages wait here while Claude is on a turn, and go out one
    // per turn. Each is shown as a message of msga's own from the moment it's
    // sent until its prompt is in the transcript — it can wait for as long as
    // Claude's turn takes.
    struct Outgoing {
        std::string text; // what Claude is sent
        model::Ts   ts         = 0;
        // A reply in a subagent's thread: the text is its relay to the
        // subagent (subagentReplyPrompt); the copy shows the reply, in the thread.
        model::Ts   threadRoot = 0;
        std::string shown;
        Unsent      back; // the composer's, for onUnsent should it fail
    };
    std::deque<Outgoing>       outbox;
    std::optional<Outgoing>    flying; // taken from the outbox, prompt not landed yet
    // The turn msga started: from launching it until its end is in the transcript.
    bool                       sending         = false;
    bool                       launching       = false; // the launcher hasn't reported back yet
    bool                       stopRequested   = false; // "Stop" while launching: once it has
    bool                       stopping        = false; // `claude stop` under way
    int64_t                    sendStartedMs   = 0;
    bool                       promptLanded    = false;
    int64_t                    promptLandedMs  = 0;
    // The message on its way was typed into the live worker (typeLive):
    // Claude Code holds it now, and a turn under way takes it at its next
    // step — however long that is — so it isn't given up on while it works.
    bool                       handedOver      = false;
    int64_t                    typeLiveAfterMs = 0; // not typed live before this (kTypeRetryMs)
    Done                       inFlight;            // a /btw's done(), until its root lands
    Unsent                     inFlightBack;        // its question, for onUnsent
    // typeLive's, while it runs. Weak: an AttachInput holds itself until
    // `attach` has exited (a moment after its done ran), and its done can
    // run from inside the call that starts it (nothing to type, no terminal)
    // or from cancel() — set only when it's still under way after the call,
    // dropped by its done, so a live handle means "typing now".
    std::weak_ptr<AttachInput> typing;
    // The permission question it waits on (awaitsApproval), as read off its
    // screen: the `needs` it was read for, and the options it offers.
    std::string                approvalNeeds;
    std::vector<PermissionQuestion::Option> approvalOptions;
    int                                     approvalReads       = 0;
    int64_t                                 approvalReadAfterMs = 0; // next try, after a failed one
    bool                                    approvalAnswered    = false;
    int64_t                                 approvalSinceMs     = 0; // when it began waiting
    // Asked through msga's permission hook (approvals.h): the question's id
    // (kept once answered), and what the hook prints for each option.
    std::string                             hookQuestion;
    std::vector<std::string>                hookAnswers;
    std::weak_ptr<AttachAnswer>             answering; // reading or answering it (as `typing`)
    // A session branched off another (a /btw, or `--fork-session` anywhere):
    // shown as a thread in its parent rather than in the list (detectForks).
    std::string forkOf;          // the parent's conversation id; "" = a session of its own
    int         forkAt     = -1; // parser.items() index of the thread's first prompt; -1 = none yet
    model::Ts   forkRoot   = 0;  // that prompt's ts: the root message in the parent
    bool        standalone = false;   // "Open as session": listed as a session of its own
    bool        inFamily   = false;   // others start as it does (detectForks' last look)
    bool        awaitingRoot = false; // msga launched it: its first prompt settles the send
    // An agent branch's (startAgentBranch): the tools it was started without.
    // Each turn of msga's goes typed into its live worker, or else as a fork
    // of it passing them again (adopted as a copy): never a plain resume,
    // which may start a copy without its options (see dispatch).
    std::vector<std::string> deniedTools;
    // What its root shows instead of its first prompt (setAgentBranchLabel).
    std::string              branchLabel;
    // Someone following its turn (watchTurn): the turn's progress, its end.
    struct TurnWatch {
        model::Backend::AgentTurnFn fn;
        // Send: msga's turn, from the moment it's sent (`sent`) until settled;
        // else the turn under way, if any.
        bool                        send = false, sent = false;
        // Items from here on are the turn's (npos: from the branch's first
        // prompt, once it's known).
        size_t                      from  = 0;
        int                         phase = -1;    // as last told; -1 = nothing yet
        bool                        held  = false; // until the branch's start is told
    };
    std::unique_ptr<TurnWatch> watch;
};

struct Backend::SearchText {
    model::Ts   ts = 0, thread = 0;
    std::string text;
    bool        markdown = false; // `text` is Markdown, rendered before it's matched
};

struct Backend::Cleanup {
    int                      pending = 1; // the remover's own hold
    std::vector<WorktreeRef> refs;
};

} // namespace claude
