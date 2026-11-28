// The machine Claude Code runs on, as the backend sees it: everything that
// touches that machine's processes, or writes to Claude Code's files there,
// goes through a Host (docs/claude-remote-relay.md).
//
//   • LocalHost: this computer — the roster scanned here, the CLI run here,
//     live typing through `claude attach` in a terminal of ours. What the
//     backend always did.
//   • RemoteHost (host_remote.cpp): another machine, reached through
//     `ssh <destination> msga --cc-relay`, which runs a LocalHost there and
//     speaks relay_protocol.h. Claude Code's transcripts, job files and
//     prompt history come over as appended records and are kept in a mirror
//     folder here, which paths() points at — so the backend READS sessions
//     the same way from either host, and only its writes and process work
//     cross the wire.
//
// Callbacks run on the plat loop, never re-entrantly from inside the call,
// and never after the Host is destroyed — except a HostTask's (as
// AttachInput's: see attach.h), which callers guard.
#pragma once

#include "app/claude/approvals.h"
#include "app/claude/attach.h"
#include "app/claude/catalog.h"
#include "app/claude/common.h"
#include "app/claude/launcher.h"
#include "app/claude/roster.h"
#include "app/claude/worktrees.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace claude {

// What a Host reports of itself, for the workspace's error banner: the
// remote connection being set up, lost, refused.
struct HostState {
    enum class Link : uint8_t { Up, Connecting, Down };
    Link        link = Link::Up;
    std::string error; // Down: why (translated), "" while it just retries
};

class Host {
public:
    virtual ~Host();

    // Where the backend reads Claude Code's own files: its config folder
    // here, or the mirror of the remote one.
    virtual const Paths &paths() const   = 0;
    // This computer: files Claude names (its answers' output files, a
    // command's log) are files here.
    virtual bool         isLocal() const = 0;
    // Whether there is a `claude` CLI to run (sessions can be started).
    virtual bool         hasCli() const  = 0;
    // A remote host's name for messages ("build-box"); "" when local.
    virtual std::string  name() const { return {}; }
    virtual HostState    state() const { return {}; }
    // Where msga's permission hook leaves the questions of this computer's
    // sessions, for the backend to watch ("" = none to watch).
    virtual std::string  approvalsDir() const { return {}; }
    // Whether what the backend reads is all there: false while a remote
    // host connects or catches up, when a missing transcript or job means
    // nothing (sessions are never dropped meanwhile).
    virtual bool         settled() const { return true; }

    // ── The roster ──────────────────────────────────────────────────────────
    // Every session the host lists now (scanSessions).
    virtual std::vector<SessionInfo>                  scan(JobStateCache *cache) = 0;
    // The same, as a call that may run on a worker thread (the first scan).
    virtual std::function<std::vector<SessionInfo>()> scanner() const            = 0;
    // The sessions the backend follows (known and listed): a remote host
    // keeps their transcripts in its mirror. Cheap to call on every refresh.
    virtual void    follow(const std::vector<std::string> &sessionIds) { (void)sessionIds; }
    // A transcript's birth time (epoch µs), 0 when unknown: the mirror's
    // files are born when they are copied, not when Claude Code made them.
    virtual int64_t bornMicros(const std::string &path) const;

    // ── Running the CLI (Launcher's, see launcher.h) ───────────────────────
    virtual void start(
        const std::string &cwd,
        const std::string &prompt,
        bool               skipPermissionChecks,
        const std::string &rolePrompt,
        const std::string &agentsJson,
        Launcher::Done     done
    ) = 0;
    virtual void resume(
        const std::string &sessionId,
        const std::string &cwd,
        const std::string &prompt,
        bool               isBackground,
        bool               stopFirst,
        Launcher::Done     done
    ) = 0;
    virtual void fork(
        const std::string              &sessionId,
        const std::string              &cwd,
        const std::string              &prompt,
        Launcher::Done                  done,
        const std::vector<std::string> &deniedTools
    ) = 0;
    virtual void
    stop(const std::string &sessionId, const std::string &cwd, std::function<void()> done) = 0;
    virtual void remove(
        const std::string                       &sessionId,
        const std::string                       &jobId,
        const std::string                       &cwd,
        std::function<void(std::string refusal)> done
    )                                                        = 0;
    virtual void checkLogin(std::function<void(Login)> done) = 0;
    virtual void listCommands(
        const std::string &cwd, std::function<void(std::vector<SlashCommand>, Account)> done
    ) = 0;

    // ── The terminal UI of a live worker (attach.h) ────────────────────────
    // Types `prompt` into it (AttachInput). The handle cancels; keep it weak.
    virtual std::shared_ptr<HostTask> sendLive(
        const std::string &sessionId,
        const std::string &cwd,
        const std::string &prompt,
        AttachInput::Done  done
    ) = 0;
    // Reads (number 0) or answers its permission question — the one the
    // job's `needs` names (questionIsFor) — as AttachAnswer::choose does.
    virtual std::shared_ptr<HostTask> answer(
        const std::string   &sessionId,
        const std::string   &cwd,
        const std::string   &needs,
        int                  number,
        const std::string   &label,
        AttachAnswer::Result done
    ) = 0;

    // ── msga's permission hook (approvals.h) ───────────────────────────────
    virtual std::vector<HookQuestion> pendingQuestions()                          = 0;
    virtual void                      takeQuestion(const std::string &id)         = 0;
    virtual bool answerQuestion(const std::string &id, const std::string &answer) = 0;

    // ── Files and processes ─────────────────────────────────────────────────
    // A background command's output (or a file it writes to): its size, -1
    // when there is none; its last `max` bytes. A remote host answers from
    // what it last heard, and starts listening for the file.
    virtual int64_t fileSize(const std::string &path)                                          = 0;
    virtual bool    statFile(const std::string &path, int64_t *size, int64_t *mtimeMicros)     = 0;
    virtual bool readTail(const std::string &path, int64_t size, size_t max, std::string *out) = 0;
    // Whether background commands can be found and stopped (commandProcessGroup).
    virtual bool canStopCommands() const                                                       = 0;
    // done(stopped): the command writing `output` signalled.
    virtual void stopCommand(
        const std::string &sessionId, const std::string &output, std::function<void(bool)> done
    ) = 0;
    // done(gone): no process writes `output` any more.
    virtual void commandGone(
        const std::string &sessionId, const std::string &output, std::function<void(bool)> done
    ) = 0;
    // Takes the record `uuid` out of a transcript (removeFromTranscript).
    // The transcript reads anew once done runs.
    virtual void deleteRecord(
        const std::string                     &path,
        const std::string                     &uuid,
        std::function<void(bool, std::string)> done
    ) = 0;
    // Deletes worktrees (reapWorktrees, worktreeInUse there).
    virtual void reapWorktrees(
        std::vector<WorktreeRef> refs, std::function<void(std::vector<std::string>)> done
    )                                                               = 0;
    // Whether `dir` is a folder there.
    virtual bool        isDir(const std::string &dir) const         = 0;
    // Why no background session can start in `dir` there ("" = it can).
    virtual std::string folderProblem(const std::string &dir) const = 0;
    // Files to attach to a prompt, made readable there: then(paths there,
    // "") or then({}, the file that couldn't be read).
    virtual void        upload(
        std::vector<std::string>                                                 files,
        std::function<void(std::vector<std::string> copies, std::string failed)> then
    )                                                                          = 0;
    // Every session Claude Code keeps there (the "add a session" list).
    virtual void catalog(std::function<void(std::vector<CatalogEntry>)> done)  = 0;
    // One session's entry, for adding it (false when there's none).
    virtual bool catalogEntry(const std::string &sessionId, CatalogEntry *out) = 0;

    // Something the backend reads changed (a remote host's mirror, roster or
    // questions): the backend looks again. Unused by LocalHost, whose files
    // the backend watches itself.
    std::function<void()> onChanged;
    // state() changed.
    std::function<void()> onState;
};

// This computer. `approvalsDir`: where msga's permission hook leaves its
// questions ("" = sessions started without the hook).
class LocalHost final : public Host {
public:
    LocalHost(plat::App &app, std::string claudePath, Paths paths, std::string approvalsDir);
    ~LocalHost() override;

    // The hook and the progress note for the sessions it starts, run as
    // `exe` (this msga): Launcher::setStartSettings, setPromptNote.
    void               useHook(const std::string &exe);
    std::string        approvalsDir() const override { return _approvalsDir; }
    const std::string &claudePath() const { return _claudePath; }

    const Paths &paths() const override { return _paths; }
    bool         isLocal() const override { return true; }
    bool         hasCli() const override { return !_claudePath.empty(); }

    std::vector<SessionInfo>                  scan(JobStateCache *cache) override;
    std::function<std::vector<SessionInfo>()> scanner() const override;
    int64_t                                   bornMicros(const std::string &path) const override;

    void start(
        const std::string &cwd,
        const std::string &prompt,
        bool               skipPermissionChecks,
        const std::string &rolePrompt,
        const std::string &agentsJson,
        Launcher::Done     done
    ) override;
    void resume(
        const std::string &sessionId,
        const std::string &cwd,
        const std::string &prompt,
        bool               isBackground,
        bool               stopFirst,
        Launcher::Done     done
    ) override;
    void fork(
        const std::string              &sessionId,
        const std::string              &cwd,
        const std::string              &prompt,
        Launcher::Done                  done,
        const std::vector<std::string> &deniedTools
    ) override;
    void
    stop(const std::string &sessionId, const std::string &cwd, std::function<void()> done) override;
    void remove(
        const std::string                       &sessionId,
        const std::string                       &jobId,
        const std::string                       &cwd,
        std::function<void(std::string refusal)> done
    ) override;
    void checkLogin(std::function<void(Login)> done) override;
    void listCommands(
        const std::string &cwd, std::function<void(std::vector<SlashCommand>, Account)> done
    ) override;

    std::shared_ptr<HostTask> sendLive(
        const std::string &sessionId,
        const std::string &cwd,
        const std::string &prompt,
        AttachInput::Done  done
    ) override;
    std::shared_ptr<HostTask> answer(
        const std::string   &sessionId,
        const std::string   &cwd,
        const std::string   &needs,
        int                  number,
        const std::string   &label,
        AttachAnswer::Result done
    ) override;

    std::vector<HookQuestion> pendingQuestions() override;
    void                      takeQuestion(const std::string &id) override;
    bool answerQuestion(const std::string &id, const std::string &answer) override;

    int64_t fileSize(const std::string &path) override;
    bool    statFile(const std::string &path, int64_t *size, int64_t *mtimeMicros) override;
    bool    readTail(const std::string &path, int64_t size, size_t max, std::string *out) override;
    bool    canStopCommands() const override;
    void    stopCommand(
        const std::string &sessionId, const std::string &output, std::function<void(bool)> done
    ) override;
    void commandGone(
        const std::string &sessionId, const std::string &output, std::function<void(bool)> done
    ) override;
    void deleteRecord(
        const std::string                     &path,
        const std::string                     &uuid,
        std::function<void(bool, std::string)> done
    ) override;
    void reapWorktrees(
        std::vector<WorktreeRef> refs, std::function<void(std::vector<std::string>)> done
    ) override;
    bool        isDir(const std::string &dir) const override;
    std::string folderProblem(const std::string &dir) const override;
    void        upload(
        std::vector<std::string>                                                 files,
        std::function<void(std::vector<std::string> copies, std::string failed)> then
    ) override;
    void catalog(std::function<void(std::vector<CatalogEntry>)> done) override;
    bool catalogEntry(const std::string &sessionId, CatalogEntry *out) override;

private:
    plat::App            &_app;
    std::string           _claudePath;
    Paths                 _paths;
    std::string           _approvalsDir;
    Launcher              _launcher;
    std::shared_ptr<bool> _alive;
};

// The host `creds` name: this computer, or the remote one its SSH
// destination reaches (host_remote.cpp).
struct Credentials;
std::unique_ptr<Host> makeHost(plat::App &app, const Credentials &creds);
// `ssh <destination> <msga> --cc-relay`, its mirror in msga's cache.
// `msga` "" = msga on the server's PATH.
std::unique_ptr<Host>
makeRemoteHost(plat::App &app, const std::string &destination, const std::string &msga);
// A file-name-safe key for a host's own files ("user@box:2222" → "user_box_2222").
std::string hostKey(std::string_view destination);

} // namespace claude
