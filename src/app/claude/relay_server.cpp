#include "app/claude/relay_server.h"

#include "app/claude/relay_protocol.h"
#include "app/claude/transcript.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace claude {

using namespace relay;

namespace {

int64_t nowMs() {
    return base::nowMicros() / 1000;
}

// A transcript not found is looked for again after this long (each look
// lists every project folder).
constexpr int64_t kMissRetryMs = 10'000;
// A small file (a job's state, a report) bigger than this isn't one.
constexpr int64_t kSmallMax    = 1 << 20;

uint64_t unhex(std::string_view s) {
    uint64_t v = 0;
    for (char c : s)
        v = (v << 4) | uint64_t(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    return v;
}

// An attached file's name as given, made safe to be one in a folder.
std::string safeName(std::string_view name) {
    std::string out(file::baseName(name));
    for (char &c : out)
        if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
            c = '_';
    if (out.empty() || out == "." || out == "..")
        out = "file";
    return out;
}

} // namespace

struct RelayServer::Upload {
    std::string path;
    int64_t     size = 0, got = 0;
    FILE       *f      = nullptr;
    bool        failed = false;
    ~Upload() {
        if (f)
            std::fclose(f);
    }
};

RelayServer::RelayServer(plat::App &app, LocalHost &host, std::string uploadsDir, Write write)
    : _app(app), _host(host), _paths(host.paths()), _uploadsDir(std::move(uploadsDir)),
      _write(std::move(write)), _alive(std::make_shared<bool>(true)) {}

RelayServer::~RelayServer() {
    *_alive = false;
    if (_timer)
        _app.cancelTimer(_timer);
    for (auto &[id, t] : _typing)
        if (const auto task = t.lock())
            task->cancel();
}

void RelayServer::send(std::string line) {
    _sentMs = nowMs();
    line.push_back('\n');
    _write(std::move(line));
}

json::Writer RelayServer::reply(int64_t id) {
    json::Writer w;
    w.beginObject().key("t").value("re").key("id").value(id);
    return w;
}

void RelayServer::start() {
    Hello h;
    h.protocol    = kProtocol;
    h.protocolMin = kProtocolMin;
    h.msgaVersion = msgaVersion();
    h.home        = _paths.home;
    h.os          = osName();
    h.claude      = _host.hasCli();
    h.caps        = {std::string(kCapAttach)};
    if (!_host.approvalsDir().empty())
        h.caps.emplace_back(kCapHook);
    if (_host.canStopCommands())
        h.caps.emplace_back(kCapStopCmd);
    send(helloLine(h));
    _heardMs = nowMs();
    _timer   = _app.addTimer(kTickMs, true, [this, alive = _alive] {
        if (*alive)
            tick();
    });
}

void RelayServer::receive(std::string_view line) {
    _heardMs = nowMs();
    json::Document doc;
    if (!doc.parse(line, nullptr) || !doc.root().isObject()) {
        LOG_WARN("relay", "unreadable line from msga (%zu bytes)", line.size());
        return;
    }
    handle(doc.root());
}

void RelayServer::handle(const json::Value &v) {
    const std::string_view t = v["t"].str();
    if (t == "hello") {
        _helloSeen = true;
        for (const json::Value e : v["have"]) {
            const std::string_view p = e.key();
            if (mirrorPathOk(p))
                _have[std::string(p)] = {e[0].integer(), unhex(e[1].str())};
        }
        return;
    }
    if (t == "ping")
        return;
    if (t == "follow") {
        const auto ids = readStrings(v["s"]);
        _follow        = {ids.begin(), ids.end()};
        _followSeq     = v["n"].integer();
        _syncPending   = true;
        tick(); // what's new to follow goes at once
        return;
    }
    if (t == "tails") {
        std::map<std::string, std::pair<int64_t, int64_t>> next;
        for (const json::Value p : v["p"]) {
            const std::string path(p.str());
            const auto        old = _tails.find(path);
            next[path] = old != _tails.end() ? old->second : std::pair<int64_t, int64_t>{-2, 0};
        }
        _tails = std::move(next);
        syncTails();
        return;
    }
    if (t == "refetch") {
        const std::string r(v["p"].str());
        const auto        it = _files.find(_paths.home + "/" + r);
        if (mirrorPathOk(r) && it != _files.end()) {
            it->second = Mirrored{0, -1, -1, it->second.small};
            json::Writer w;
            w.beginObject().key("t").value("reset").key("p").value(r).endObject();
            send(w);
        }
        return;
    }
    if (t == "cancel") {
        const auto it = _typing.find(v["id"].integer());
        if (it != _typing.end())
            if (const auto task = it->second.lock())
                task->cancel();
        return;
    }
    if (t == "upload" || t == "chunk" || t == "end") {
        upload(v["id"].integer(), t, v);
        return;
    }
    handleRequest(t, v["id"].integer(), v);
}

void RelayServer::handleRequest(std::string_view t, int64_t id, const json::Value &v) {
    const std::string session(v["session"].str()), cwd(v["cwd"].str());
    const auto        alive    = _alive;
    // A launch's answer: the session it went on in, or why not.
    const auto        launched = [this, alive, id](std::string sessionId, std::string error) {
        if (!*alive)
            return;
        json::Writer w = reply(id);
        w.key("session").value(sessionId).key("error").value(error).endObject();
        send(w);
    };
    if (t == "start") {
        _host.start(
            cwd,
            std::string(v["prompt"].str()),
            v["skip"].boolean(),
            std::string(v["role"].str()),
            std::string(v["agents"].str()),
            launched
        );
    } else if (t == "resume") {
        _host.resume(
            session,
            cwd,
            std::string(v["prompt"].str()),
            v["bg"].boolean(),
            v["stopFirst"].boolean(),
            launched
        );
    } else if (t == "fork") {
        _host.fork(
            session, cwd, std::string(v["prompt"].str()), launched, readStrings(v["denied"])
        );
    } else if (t == "stop") {
        _host.stop(session, cwd, [this, alive, id] {
            if (!*alive)
                return;
            json::Writer w = reply(id);
            send(w.endObject());
        });
    } else if (t == "remove") {
        // msga can't tell which folders exist here: a gone one is the home folder.
        const std::string from = file::isDir(cwd) ? cwd : base::homeDir();
        _host.remove(
            session, std::string(v["job"].str()), from, [this, alive, id](std::string refusal) {
                if (!*alive)
                    return;
                json::Writer w = reply(id);
                w.key("refusal").value(refusal).endObject();
                send(w);
            }
        );
    } else if (t == "login") {
        _host.checkLogin([this, alive, id](Login login) {
            if (!*alive)
                return;
            json::Writer w = reply(id);
            w.key("login").value(loginWord(login)).endObject();
            send(w);
        });
    } else if (t == "commands") {
        _host.listCommands(
            cwd, [this, alive, id](std::vector<SlashCommand> commands, Account account) {
                if (!*alive)
                    return;
                json::Writer w = reply(id);
                w.key("commands").beginArray();
                for (const auto &c : commands)
                    writeCommand(w, c);
                w.endArray().key("account");
                writeAccount(w, account);
                send(w.endObject());
            }
        );
    } else if (t == "type") {
        const auto ran    = std::make_shared<bool>(false);
        const auto handle = _host.sendLive(
            session,
            cwd,
            std::string(v["prompt"].str()),
            [this, alive, id, ran](AttachInput::Outcome outcome, std::string detail) {
                *ran = true;
                if (!*alive)
                    return;
                _typing.erase(id);
                json::Writer w = reply(id);
                w.key("outcome").value(inputOutcomeWord(outcome)).key("detail").value(detail);
                send(w.endObject());
            }
        );
        if (!*ran)
            _typing[id] = handle;
    } else if (t == "answer") {
        _host.answer(
            session,
            cwd,
            std::string(v["needs"].str()),
            int(v["n"].integer()),
            std::string(v["label"].str()),
            [this, alive, id](
                AttachAnswer::Outcome             outcome,
                std::optional<PermissionQuestion> q,
                std::string                       detail
            ) {
                if (!*alive)
                    return;
                json::Writer w = reply(id);
                w.key("outcome").value(answerOutcomeWord(outcome)).key("detail").value(detail);
                if (q) {
                    w.key("question");
                    writePermissionQuestion(w, *q);
                }
                send(w.endObject());
            }
        );
    } else if (t == "take") {
        _host.takeQuestion(std::string(v["q"].str()));
        return; // no answer
    } else if (t == "reply") {
        const bool ok =
            _host.answerQuestion(std::string(v["q"].str()), std::string(v["answer"].str()));
        json::Writer w = reply(id);
        w.key("ok").value(ok).endObject();
        send(w);
        syncQuestions(); // answered: gone from the list at once
    } else if (t == "stopcmd" || t == "cmdgone") {
        const bool stop = t == "stopcmd";
        auto       done = [this, alive, id](bool yes) {
            if (!*alive)
                return;
            json::Writer w = reply(id);
            w.key("yes").value(yes).endObject();
            send(w);
        };
        const std::string output(v["output"].str());
        if (stop)
            _host.stopCommand(session, output, done);
        else
            _host.commandGone(session, output, done);
    } else if (t == "delete") {
        // Only a transcript of Claude Code's is ever rewritten.
        const std::string path(v["path"].str());
        if (!str::startsWith(path, _paths.projectsDir() + "/") || !str::endsWith(path, ".jsonl") ||
            !mirrorPathOk(rel(path))) {
            json::Writer w = reply(id);
            w.key("ok").value(false).key("error").value("not a transcript").endObject();
            send(w);
            return;
        }
        _host.deleteRecord(
            path,
            std::string(v["uuid"].str()),
            [this, alive, id, path](bool ok, std::string error) {
                if (!*alive)
                    return;
                // The rewritten transcript reaches the mirror before the
                // answer does: msga reads it again on the answer.
                syncFile(path, false);
                json::Writer w = reply(id);
                w.key("ok").value(ok).key("error").value(error).endObject();
                send(w);
            }
        );
    } else if (t == "reap") {
        std::vector<WorktreeRef> refs;
        for (const json::Value r : v["refs"])
            refs.push_back(
                {json::owned(r["path"]), json::owned(r["branch"]), json::owned(r["origin"])}
            );
        _host.reapWorktrees(
            std::move(refs), [this, alive, id](std::vector<std::string> notDeleted) {
                if (!*alive)
                    return;
                json::Writer w = reply(id);
                w.key("notDeleted");
                writeStrings(w, notDeleted);
                send(w.endObject());
            }
        );
    } else if (t == "catalog") {
        _host.catalog([this, alive, id](std::vector<CatalogEntry> entries) {
            if (!*alive)
                return;
            json::Writer w = reply(id);
            w.key("entries").beginArray();
            for (const auto &e : entries)
                writeCatalogEntry(w, e);
            send(w.endArray().endObject());
        });
    } else {
        json::Writer w = reply(id);
        w.key("error").value(str::concat({"unknown request ", t})).endObject();
        send(w);
    }
}

void RelayServer::upload(int64_t id, std::string_view t, const json::Value &v) {
    const auto fail = [&](std::string_view why) {
        _uploads.erase(id);
        json::Writer w = reply(id);
        w.key("error").value(why).endObject();
        send(w);
    };
    if (t == "upload") {
        const int64_t size = v["size"].integer(-1);
        if (size < 0 || size > kMaxUpload)
            return fail("too big");
        auto u                = std::make_unique<Upload>();
        u->size               = size;
        // A folder of its own per file, as msga's own cache keeps them: the
        // name stays the one the user sent (Claude sees it).
        const std::string dir = _uploadsDir + "/" + crypto::uuid4();
        u->path               = dir + "/" + safeName(v["name"].str());
        if (!file::makeDirs(dir) || !(u->f = std::fopen(u->path.c_str(), "wb")))
            return fail("can't write");
        _uploads[id] = std::move(u);
        return;
    }
    const auto it = _uploads.find(id);
    if (it == _uploads.end())
        return; // failed already
    Upload &u = *it->second;
    if (t == "chunk") {
        std::string bytes;
        if (!crypto::base64Decode(v["d"].str(), &bytes) || u.got + int64_t(bytes.size()) > u.size ||
            std::fwrite(bytes.data(), 1, bytes.size(), u.f) != bytes.size())
            u.failed = true;
        u.got += int64_t(bytes.size());
        return;
    }
    const bool ok = std::fclose(u.f) == 0 && !u.failed && u.got == u.size;
    u.f           = nullptr;
    if (!ok) {
        file::remove(u.path);
        return fail("upload broken");
    }
    json::Writer w = reply(id);
    w.key("path").value(u.path).endObject();
    _uploads.erase(it);
    send(w);
}

// ── Looking ─────────────────────────────────────────────────────────────────

std::string RelayServer::rel(const std::string &path) const {
    if (!str::startsWith(path, _paths.home + "/"))
        return {};
    std::string r = path.substr(_paths.home.size() + 1);
    return mirrorPathOk(r) ? r : std::string();
}

std::string RelayServer::transcriptOf(const std::string &sessionId, const SessionInfo *listed) {
    if (listed && !listed->transcriptPath.empty() && file::exists(listed->transcriptPath))
        return listed->transcriptPath;
    const auto found = _found.find(sessionId);
    if (found != _found.end() && file::exists(found->second))
        return found->second;
    const auto miss = _missMs.find(sessionId);
    if (miss != _missMs.end() && nowMs() - miss->second < kMissRetryMs)
        return {};
    std::string path = _paths.findTranscript(sessionId);
    if (path.empty()) {
        _missMs[sessionId] = nowMs();
        _found.erase(sessionId);
    } else {
        _missMs.erase(sessionId);
        _found[sessionId] = path;
    }
    return path;
}

void RelayServer::tick() {
    const std::vector<SessionInfo> roster = scanSessions(_paths, nullptr, &_cache);
    syncRoster(roster);
    if (!_helloSeen) {
        // What msga's mirror holds comes with its hello: nothing is sent
        // of the files before.
        idleCheck();
        return;
    }

    // What the mirror holds: every listed session's transcript and the
    // followed ones', each with its subagents; the jobs' small files; the
    // prompt history.
    std::vector<std::pair<std::string, bool>> want; // path, small
    std::set<std::string>                     ids(_follow);
    std::set<std::string>                     jobDirs;
    for (const SessionInfo &s : roster) {
        ids.insert(s.sessionId);
        if (s.kind == SessionInfo::Kind::Background)
            jobDirs.insert(_paths.jobDir(jobIdOr(s.sessionId, s.jobId)));
    }
    std::vector<file::DirEntry> entries;
    for (const std::string &id : ids) {
        const auto listed = std::find_if(roster.begin(), roster.end(), [&](const SessionInfo &s) {
            return s.sessionId == id;
        });
        const std::string transcript =
            transcriptOf(id, listed != roster.end() ? &*listed : nullptr);
        if (transcript.empty())
            continue;
        want.emplace_back(transcript, false);
        const std::string subagents = Paths::subagentsDir(transcript);
        entries.clear();
        if (file::listDir(subagents, &entries))
            for (const auto &e : entries) {
                if (e.isDir)
                    continue;
                if (str::endsWith(e.name, ".jsonl"))
                    want.emplace_back(subagents + "/" + e.name, false);
                else if (str::endsWith(e.name, ".json"))
                    want.emplace_back(subagents + "/" + e.name, true);
            }
    }
    for (const std::string &dir : jobDirs) {
        entries.clear();
        if (file::listDir(dir, &entries))
            for (const auto &e : entries)
                if (!e.isDir && str::endsWith(e.name, ".json"))
                    want.emplace_back(dir + "/" + e.name, true);
    }
    want.emplace_back(_paths.home + "/history.jsonl", false);
    for (const auto &[path, small] : want)
        syncFile(path, small);
    // A job Claude Code dropped (`claude rm`): its folder goes from the mirror.
    for (const std::string &dir : _jobDirs)
        if (!jobDirs.count(dir) && !file::isDir(dir)) {
            if (const std::string r = rel(dir); !r.empty()) {
                json::Writer w;
                w.beginObject().key("t").value("gone").key("p").value(r).endObject();
                send(w);
            }
            const std::string prefix = dir + "/";
            for (auto it = _files.begin(); it != _files.end();)
                it = str::startsWith(it->first, prefix) ? _files.erase(it) : std::next(it);
        }
    _jobDirs = std::move(jobDirs);

    syncQuestions();
    syncTails();
    if (_syncPending) {
        // Every followed session is in the mirror as far as it exists here.
        _syncPending = false;
        json::Writer w;
        w.beginObject().key("t").value("synced").key("n").value(_followSeq).endObject();
        send(w);
    }
    idleCheck();
}

void RelayServer::idleCheck() {
    if (nowMs() - _sentMs >= kPingMs) {
        json::Writer w;
        w.beginObject().key("t").value("ping").endObject();
        send(w);
    }
    if (nowMs() - _heardMs >= kRelayIdleMs && onIdle)
        onIdle();
}

void RelayServer::syncRoster(const std::vector<SessionInfo> &roster) {
    json::Writer w;
    w.beginObject().key("t").value("roster").key("sessions").beginArray();
    for (const SessionInfo &s : roster)
        writeSession(w, s);
    w.endArray().endObject();
    if (w.str() == _roster)
        return;
    _roster = w.str();
    send(w);
}

void RelayServer::syncQuestions() {
    if (_host.approvalsDir().empty())
        return;
    json::Writer w;
    w.beginObject().key("t").value("questions").key("q").beginArray();
    for (const HookQuestion &q : _host.pendingQuestions())
        writeQuestion(w, q);
    w.endArray().endObject();
    if (w.str() == _questions)
        return;
    _questions = w.str();
    send(w);
}

void RelayServer::syncTails() {
    constexpr size_t kTail = 4096;
    for (auto &[path, last] : _tails) {
        int64_t size = -1, mtime = 0;
        if (!_host.statFile(path, &size, &mtime))
            size = -1;
        if (last == std::pair{size, mtime})
            continue;
        last = {size, mtime};
        std::string tail;
        if (size > 0)
            _host.readTail(path, size, kTail, &tail);
        json::Writer w;
        w.beginObject().key("t").value("tail").key("p").value(path);
        w.key("size").value(size).key("m").value(mtime).key("d").value(tail).endObject();
        send(w);
    }
}

bool RelayServer::syncFile(const std::string &path, bool small) {
    file::Stat st;
    auto       it = _files.find(path);
    const auto r  = rel(path);
    if (r.empty())
        return false;
    if (!file::stat(path, &st) || st.isDir) {
        if (it != _files.end()) {
            _files.erase(it);
            json::Writer w;
            w.beginObject().key("t").value("gone").key("p").value(r).endObject();
            send(w);
        }
        return false;
    }
    if (it == _files.end()) {
        Mirrored m;
        m.small = small;
        // What msga's mirror already holds goes on from there — if its end
        // is this file's at the same place (the file wasn't rewritten).
        if (const auto h = _have.find(r); h != _have.end()) {
            const int64_t had = h->second.first;
            std::string   tail;
            const int64_t from = std::max<int64_t>(0, had - int64_t(kTailHashBytes));
            if (!small && had <= st.size &&
                file::readRange(path, from, size_t(had - from), &tail) &&
                tailHash(tail) == h->second.second) {
                m.sent = had;
            } else if (!small) {
                json::Writer w;
                w.beginObject().key("t").value("reset").key("p").value(r).endObject();
                send(w);
            }
            _have.erase(h);
        }
        it = _files.emplace(path, m).first;
    }
    Mirrored &m = it->second;
    if (small) {
        if (st.size == m.size && st.mtimeMicros == m.mtime)
            return true;
        std::string bytes;
        if (st.size > kSmallMax || !file::readAll(path, &bytes))
            return true;
        m.size  = st.size;
        m.mtime = st.mtimeMicros;
        json::Writer w;
        w.beginObject().key("t").value("file").key("p").value(r).key("d").value(bytes);
        w.key("m").value(st.mtimeMicros).endObject();
        send(w);
        return true;
    }
    if (st.size < m.sent) {
        // Rewritten shorter (a record deleted): from the start again.
        m.sent = 0;
        json::Writer w;
        w.beginObject().key("t").value("reset").key("p").value(r).endObject();
        send(w);
    }
    if (st.size > m.sent)
        sendData(path, m.sent, st.size, st.mtimeMicros, m.sent == 0);
    return true;
}

void RelayServer::sendData(
    const std::string &path, int64_t from, int64_t to, int64_t mtime, bool first
) {
    Mirrored  &m    = _files[path];
    const auto r    = rel(path);
    int64_t    born = 0;
    if (first) {
        file::Stat st;
        born = file::stat(path, &st) ? st.birthMicros : 0;
    }
    std::string buf, more;
    while (from < to) {
        if (!file::readRange(path, from, std::min<size_t>(kChunk, size_t(to - from)), &buf) ||
            buf.empty())
            break;
        size_t cut = buf.rfind('\n');
        // A line longer than a chunk (a pasted image) goes whole, in one.
        while (cut == std::string::npos && from + int64_t(buf.size()) < to &&
               buf.size() < kMaxLine / 2) {
            if (!file::readRange(path, from + int64_t(buf.size()), kChunk, &more) || more.empty())
                break;
            const size_t at = buf.size();
            buf += more;
            const size_t nl = more.find('\n');
            if (nl != std::string::npos)
                cut = at + nl;
        }
        if (cut == std::string::npos)
            break; // a line still being written: it goes once it's whole
        buf.resize(cut + 1);
        json::Writer w;
        w.beginObject().key("t").value("data").key("p").value(r).key("at").value(from);
        w.key("d").value(buf).key("m").value(mtime);
        if (first && from == 0 && born)
            w.key("b").value(born);
        w.endObject();
        send(w);
        from += int64_t(buf.size());
        m.sent = from;
    }
}

} // namespace claude
