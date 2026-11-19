// The relay's side of the relay protocol (relay_protocol.h): this machine's
// Claude Code, served to an msga elsewhere. It runs a LocalHost and
//   • tells msga the roster whenever it changes, the permission questions
//     its hook brings, the files msga watches the end of;
//   • keeps msga's mirror of Claude Code's files up to date: each followed
//     transcript (and its subagents'), the jobs' small files and the prompt
//     history, as the records appended to them — never anything else;
//   • runs msga's requests (start, resume, type into a live worker, answer a
//     question, stop, delete…) and answers each.
// It reads msga's lines and writes its own through the caller (relay_main.cpp:
// stdin and stdout; tests: a pipe in the same process).
#pragma once

#include "app/claude/host.h"
#include "base/json.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace plat {
class App;
}

namespace claude {

class RelayServer {
public:
    using Write = std::function<void(std::string line)>;
    // `host` runs the sessions; `uploadsDir` takes the files msga attaches.
    RelayServer(plat::App &app, LocalHost &host, std::string uploadsDir, Write write);
    ~RelayServer();
    RelayServer(const RelayServer &)            = delete;
    RelayServer &operator=(const RelayServer &) = delete;

    // Says hello and starts looking (a tick every kTickMs).
    void                  start();
    // One line from msga.
    void                  receive(std::string_view line);
    // msga has said nothing for kRelayIdleMs: it's gone (the caller quits).
    std::function<void()> onIdle;

    // Looks now (tests: no waiting for the tick).
    void tick();

    static constexpr int kTickMs = 500;

private:
    struct Upload;
    void         send(std::string line);
    void         send(json::Writer &w) { send(w.take()); }
    // {"t":"re","id":id, …}: the caller adds its members, then endObject.
    json::Writer reply(int64_t id);
    void         handle(const json::Value &v);
    void         handleRequest(std::string_view type, int64_t id, const json::Value &v);
    void         upload(int64_t id, std::string_view type, const json::Value &v);

    // The mirror: a file brought up to date (sent what was appended), or a
    // small one sent whole when it changed. False when it's gone.
    bool syncFile(const std::string &path, bool small);
    void sendData(const std::string &path, int64_t from, int64_t to, int64_t mtime, bool first);
    std::string rel(const std::string &path) const; // "" = not under Claude Code's folder
    std::string transcriptOf(const std::string &sessionId, const SessionInfo *listed);
    void        syncRoster(const std::vector<SessionInfo> &roster);
    void        syncQuestions();
    void        syncTails();
    void        idleCheck(); // a ping when it said nothing for long; msga gone?

    plat::App            &_app;
    LocalHost            &_host;
    Paths                 _paths;
    std::string           _uploadsDir;
    Write                 _write;
    std::shared_ptr<bool> _alive;
    uint64_t              _timer  = 0;
    int64_t               _sentMs = 0, _heardMs = 0;
    bool                  _helloSeen = false;
    int64_t               _followSeq = 0; // msga's latest follow, echoed by "synced"

    JobStateCache                                _cache;
    std::string                                  _roster, _questions; // as last sent
    // msga's sessions to follow, and their transcripts found (by session id;
    // "" = looked for at _missMs[id], not there).
    std::set<std::string>                        _follow;
    std::unordered_map<std::string, std::string> _found;
    std::unordered_map<std::string, int64_t>     _missMs;
    bool                                         _syncPending = false;
    // Mirror files by absolute path: bytes sent (append) or (size, mtime)
    // sent (small).
    struct Mirrored {
        int64_t sent = 0, size = -1, mtime = -1;
        bool    small = false;
    };
    std::map<std::string, Mirrored> _files;
    std::set<std::string>           _jobDirs; // mirrored jobs/<id> folders
    // What msga's mirror holds (its hello): relative path → (size, tail hash).
    std::unordered_map<std::string, std::pair<int64_t, uint64_t>> _have;
    // Files msga watches the end of (tails): (size, mtime) last sent.
    std::map<std::string, std::pair<int64_t, int64_t>>            _tails;
    // Live typing under way, by request id (cancel).
    std::unordered_map<int64_t, std::weak_ptr<HostTask>>          _typing;
    std::unordered_map<int64_t, std::unique_ptr<Upload>>          _uploads;
    std::vector<CatalogEntry>                                     _catalog;
};

// `msga --cc-relay`: serves this machine's Claude Code on stdin/stdout until
// stdin ends. No window, no display, no D-Bus. Returns the exit code.
int runRelay(int argc, char **argv);

} // namespace claude
