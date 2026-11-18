// Another machine as the backend's host (host.h): Claude Code there, reached
// through `msga --cc-relay` over a Channel (ssh in the app; tests plug in a
// pipe). The relay's mirror messages keep a copy of Claude Code's files under
// `mirrorDir` (applied on the channel's thread: no file work on the loop),
// which paths() points at; the roster, the questions and the answers to
// requests come to the loop. The link is kept up: lost, it's made again with
// a growing wait (1 s … 1 min), and the relay's snapshot then catches the
// mirror up (sessions run on there meanwhile).
#pragma once

#include "app/claude/channel.h"
#include "app/claude/host.h"
#include "base/json.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace claude {

class RemoteHost final : public Host {
public:
    using Connect = std::function<std::unique_ptr<Channel>()>;
    // `name`: the SSH destination as the user gave it (messages, the files'
    // key). `connect` makes the channel for each attempt.
    RemoteHost(plat::App &app, std::string name, std::string mirrorDir, Connect connect);
    ~RemoteHost() override;

    // The `ssh` command line that reaches `destination`'s relay (`msga` ""
    // = msga on its PATH, else ~/.local/bin/msga).
    static std::vector<std::string>
    sshArgs(const std::string &destination, const std::string &msga);

    const Paths &paths() const override { return _paths; }
    bool         isLocal() const override { return false; }
    bool         hasCli() const override { return _claude; }
    std::string  name() const override { return _name; }
    HostState    state() const override { return _state; }
    bool         settled() const override { return _state.link == HostState::Link::Up && _settled; }

    std::vector<SessionInfo>                  scan(JobStateCache *cache) override;
    std::function<std::vector<SessionInfo>()> scanner() const override;
    void    follow(const std::vector<std::string> &sessionIds) override;
    int64_t bornMicros(const std::string &path) const override;

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
    bool    answerQuestion(const std::string &id, const std::string &answer) override;
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

    // Connects now (tests: no backoff wait); also what the timers call.
    void connect();
    // The link's attempts so far (tests).
    int  attempts() const { return _attempts; }

private:
    struct Shared; // what the channel's thread and the loop share
    struct Task;   // a request's HostTask
    using Reply = std::function<void(const json::Value *)>; // null: no answer (link lost)
    json::Writer begin(std::string_view type, int64_t *id);
    void         call(json::Writer &w, int64_t id, Reply reply);
    void         onLoopLine(const std::string &line);
    void         linkUp(const json::Value &hello);
    void         linkLost(const std::string &why, bool fatal = false);
    void         setState(HostState::Link link, std::string error);
    void         sendFollow();
    void         sendTails();
    void         loadSnapshot();
    void         saveSnapshot();
    void         watchdog();
    std::string  toMirror(const std::string &serverPath) const;
    std::string  toServer(const std::string &mirrorPath) const;

    plat::App                &_app;
    std::string               _name, _mirrorDir;
    Paths                     _paths; // home = the mirror's
    Connect                   _connect;
    std::shared_ptr<Shared>   _shared;
    std::shared_ptr<Channel>  _channel;
    std::shared_ptr<bool>     _alive;
    HostState                 _state{HostState::Link::Connecting, {}};
    bool                      _claude = true, _settled = false, _reported = false;
    std::string               _serverHome;
    std::vector<std::string>  _caps;
    std::vector<SessionInfo>  _roster;
    std::vector<HookQuestion> _questions;
    std::vector<std::string>  _followed;
    int64_t                   _followSeq = 0; // the last follow sent; its "synced" settles
    std::vector<CatalogEntry> _catalog;
    std::map<int64_t, Reply>  _pending;
    int64_t                   _nextId   = 0;
    int                       _attempts = 0, _backoffMs = 0;
    uint64_t                  _retryTimer = 0, _watchTimer = 0;
    // Files whose end the backend reads (a command's output): what the relay
    // last said of each, and when the backend last asked.
    struct Tail {
        int64_t     size = -1, mtime = 0, askedMs = 0;
        bool        known = false;
        std::string bytes;
    };
    std::map<std::string, Tail> _tails;
    bool                        _tailsDirty = false;
};

} // namespace claude
