#include "app/claude/host_remote.h"

#include "app/claude/relay_protocol.h"
#include "app/model/jobs.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/thread.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace claude {

using namespace relay;
using i18n::tr;

namespace {

int64_t nowMs() {
    return base::nowMicros() / 1000;
}

constexpr int kWatchdogMs      = 5'000;
constexpr int kMaxBackoffMs    = 60'000;
// A relay that speaks another protocol is asked again this much later (it
// may have been updated meanwhile), not every minute.
constexpr int kMismatchRetryMs = 5 * 60'000;
// A file's end the backend stopped asking about is dropped after this long.
constexpr int kTailForgetMs    = 60'000;

std::string hex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    std::string       out(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4)
        out[size_t(i)] = digits[v & 15];
    return out;
}

// Single-quoted for a POSIX shell: the relay's command line runs in the
// server's login shell.
std::string shellQuote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

void writeHave(json::Writer &w, const std::string &rel, const std::string &path, int64_t size) {
    std::string   tail;
    const int64_t from = std::max<int64_t>(0, size - int64_t(kTailHashBytes));
    if (size > 0 && file::readRange(path, from, size_t(size - from), &tail))
        w.key(rel).beginArray().value(size).value(hex(tailHash(tail))).endArray();
}

// The mirror's appended files (transcripts) under `dir`, for the hello's
// "have": relative path → [size, hash of its end].
void collectHave(const std::string &dir, const std::string &rel, json::Writer &w, int depth = 0) {
    std::vector<file::DirEntry> entries;
    if (depth > 6 || !file::listDir(dir, &entries))
        return;
    for (const auto &e : entries) {
        const std::string path = dir + "/" + e.name;
        const std::string r    = rel + "/" + e.name;
        if (e.isDir)
            collectHave(path, r, w, depth + 1);
        else if (str::endsWith(e.name, ".jsonl") && mirrorPathOk(r))
            writeHave(w, r, path, e.size);
    }
}

bool appendFile(const std::string &path, std::string_view bytes) {
    FILE *f = std::fopen(path.c_str(), "ab");
    if (!f)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

// What the channel's thread and the loop share. The thread applies the
// mirror's messages itself (file work stays off the loop) and posts the rest.
struct RemoteHost::Shared {
    explicit Shared(plat::App &a) : app(a) {}
    plat::App                               &app;
    std::string                              mirrorHome;
    base::Mutex                              lock;
    std::unordered_map<std::string, int64_t> born; // mirror path → birth (µs), under lock
    std::atomic<int64_t>                     heardMs{0}, sentMs{0};
    std::atomic<bool>                        changePosted{false};
    std::atomic<uint64_t>                    generation{0}; // the connection lines count for
    std::shared_ptr<bool>                    alive;
    // On the loop: something in the mirror changed; a line for the host; the
    // link closed (why).
    std::function<void()>                    changed;
    std::function<void(const std::string &)> line, closed;
    // Any thread: asks the relay for a file again (the mirror lost step).
    std::function<void(const std::string &)> refetch;

    void postChanged() {
        if (changePosted.exchange(true))
            return;
        app.post([this, alive = alive] {
            if (!*alive)
                return;
            changePosted = false;
            changed();
        });
    }

    // A mirror message, applied: false when it's none (the loop's then).
    bool apply(const json::Value &v) {
        const std::string_view t = v["t"].str();
        if (t != "data" && t != "reset" && t != "gone" && t != "file")
            return false;
        const std::string rel(v["p"].str());
        if (!mirrorPathOk(rel)) {
            LOG_WARN("claude", "the relay named a file outside the mirror: %s", rel.c_str());
            return true;
        }
        const std::string path = mirrorHome + "/" + rel;
        if (t == "data") {
            file::makeDirs(file::dirName(path));
            const int64_t          at  = v["at"].integer();
            const std::string_view d   = v["d"].str();
            const int64_t          cur = std::max<int64_t>(0, file::size(path));
            // From the start: the same file emptied (a reader keeps it), or a new one.
            const bool             ok =
                at == 0 ? (file::exists(path) ? file::overwrite(path, d) : appendFile(path, d))
                        : at == cur && appendFile(path, d);
            if (!ok) {
                // Out of step (a line lost, the file changed here): from the start.
                file::writeAtomic(path, {}, 0644, false);
                if (refetch)
                    refetch(rel);
            } else if (const int64_t b = v["b"].integer(); b > 0) {
                base::Lock g(lock);
                born[path] = b;
            }
        } else if (t == "reset") {
            if (!file::exists(path) || !file::overwrite(path, {}))
                file::writeAtomic(path, {}, 0644, false);
            base::Lock g(lock);
            born.erase(path);
        } else if (t == "gone") {
            if (file::isDir(path))
                file::removeTree(path);
            else
                file::remove(path);
            base::Lock g(lock);
            born.erase(path);
        } else {
            file::makeDirs(file::dirName(path));
            file::writeAtomic(path, v["d"].str(), 0644, false); // a cache: no flush to disk
        }
        postChanged();
        return true;
    }
};

struct RemoteHost::Task final : HostTask {
    std::function<void()> cancelFn;
    bool                  cancel() override {
        if (cancelFn)
            cancelFn();
        return false; // the relay says how far it got, in its answer
    }
};

// ── Construction, the link ──────────────────────────────────────────────────

RemoteHost::RemoteHost(plat::App &app, std::string name, std::string mirrorDir, Connect connect)
    : _app(app), _name(std::move(name)), _mirrorDir(std::move(mirrorDir)),
      _connect(std::move(connect)), _shared(std::make_shared<Shared>(app)),
      _alive(std::make_shared<bool>(true)) {
    _paths.home = _mirrorDir + "/home";
    // The folders the backend watches exist from the start.
    for (const char *sub : {"/sessions", "/jobs", "/projects"})
        file::makeDirs(_paths.home + sub);
    _shared->mirrorHome = _paths.home;
    _shared->alive      = _alive;
    _shared->changed    = [this] {
        if (onChanged)
            onChanged();
    };
    _shared->line   = [this](const std::string &line) { onLoopLine(line); };
    _shared->closed = [this](const std::string &why) {
        linkLost(why.empty() ? std::string(tr("The connection was closed.")) : why);
    };
    loadSnapshot();
    _watchTimer = _app.addTimer(kWatchdogMs, true, [this, alive = _alive] {
        if (*alive)
            watchdog();
    });
    // Connected once the loop runs: the backend that owns this sets its
    // callbacks first.
    _app.post([this, alive = _alive] {
        if (*alive && !_channel && !_retryTimer)
            this->connect();
    });
}

RemoteHost::~RemoteHost() {
    *_alive = false;
    ++_shared->generation; // what the channel still says is no one's
    _app.cancelTimer(_watchTimer);
    if (_retryTimer)
        _app.cancelTimer(_retryTimer);
    if (_channel)
        _channel->close();
}

std::vector<std::string>
RemoteHost::sshArgs(const std::string &destination, const std::string &msga) {
    // msga where it was said to be; else on the PATH a non-interactive shell
    // has, else where its installer puts it.
    const std::string command =
        !msga.empty() ? str::concat({shellQuote(msga), " ", kFlag})
                      : str::concat(
                            {"exec sh -c 'if command -v msga >/dev/null 2>&1; then exec msga ",
                             kFlag,
                             "; fi; if [ -x \"$HOME/.local/bin/msga\" ]; then exec "
                             "\"$HOME/.local/bin/msga\" ",
                             kFlag,
                             "; fi; echo \"msga is not installed there\" >&2; exit 127'"}
                        );
    // No terminal, never a password prompt (BatchMode: keys or an agent
    // only), a dead link noticed within a minute. "--": a destination can't
    // pass for an option.
    return {
        "-T",
        "-o",
        "BatchMode=yes",
        "-o",
        "ServerAliveInterval=15",
        "-o",
        "ServerAliveCountMax=3",
        "-o",
        "ConnectTimeout=20",
        "--",
        destination,
        command
    };
}

void RemoteHost::connect() {
    _retryTimer = 0;
    if (_channel)
        return;
    ++_attempts;
    if (_state.link == HostState::Link::Up)
        setState(HostState::Link::Connecting, {});
    const uint64_t gen  = ++_shared->generation;
    // What the mirror holds is looked at on a worker, then the link made.
    auto           have = std::make_shared<std::string>();
    model::runInBackground(
        _app,
        [have, home = _paths.home] {
            json::Writer w;
            w.beginObject();
            collectHave(home + "/projects", "projects", w);
            writeHave(
                w, "history.jsonl", home + "/history.jsonl", file::size(home + "/history.jsonl")
            );
            w.endObject();
            *have = w.take();
        },
        [this, alive = _alive, have, gen] {
            if (!*alive || gen != _shared->generation || _channel)
                return;
            std::shared_ptr<Channel> channel = _connect ? _connect() : nullptr;
            std::string              error;
            const auto               shared = _shared;
            if (!channel || !channel->open(
                                [shared, gen](std::string_view line) {
                                    if (gen != shared->generation)
                                        return;
                                    shared->heardMs = nowMs();
                                    json::Document doc;
                                    if (!doc.parse(line, nullptr) || !doc.root().isObject() ||
                                        shared->apply(doc.root()))
                                        return;
                                    shared->app.post([shared, gen, line = std::string(line)] {
                                        if (*shared->alive && gen == shared->generation)
                                            shared->line(line);
                                    });
                                },
                                [shared, gen](std::string why) {
                                    shared->app.post([shared, gen, why = std::move(why)] {
                                        if (*shared->alive && gen == shared->generation)
                                            shared->closed(why);
                                    });
                                },
                                &error
                            )) {
                linkLost(
                    channel ? error : std::string(tr("ssh isn't installed on this computer."))
                );
                return;
            }
            _channel         = channel;
            _shared->heardMs = nowMs();
            _shared->refetch = [weak = std::weak_ptr<Channel>(channel)](const std::string &rel) {
                if (const auto c = weak.lock()) {
                    json::Writer w;
                    w.beginObject().key("t").value("refetch").key("p").value(rel).endObject();
                    c->send(w.take());
                }
            };
            Hello h;
            h.protocol    = kProtocol;
            h.protocolMin = kProtocolMin;
            h.msgaVersion = msgaVersion();
            h.os          = osName();
            _channel->send(helloLine(h, str::concat({"\"have\":", *have})));
            _shared->sentMs = nowMs();
        }
    );
}

void RemoteHost::linkUp(const json::Value &v) {
    Hello mine, theirs;
    mine.protocol    = kProtocol;
    mine.protocolMin = kProtocolMin;
    if (!readHello(v, &theirs) || !agreedProtocol(mine, theirs)) {
        linkLost(
            i18n::arg(
                tr("msga on %1 is a version this one can't work with. Install the same msga "
                   "version there."),
                _name
            ),
            true
        );
        return;
    }
    if (!theirs.error.empty()) {
        linkLost(theirs.error, true);
        return;
    }
    _serverHome = theirs.home;
    _claude     = theirs.claude;
    _caps       = theirs.caps;
    _backoffMs  = 0;
    _reported   = false;
    _settled    = false;
    setState(HostState::Link::Up, {});
    sendFollow();
    _tailsDirty = !_tails.empty();
    sendTails();
    saveSnapshot();
    if (onChanged)
        onChanged();
}

void RemoteHost::linkLost(const std::string &why, bool fatal) {
    const bool wasUp = _state.link == HostState::Link::Up;
    ++_shared->generation;
    if (const auto channel = std::exchange(_channel, nullptr))
        channel->close();
    _settled = false;
    // What waited for an answer gets none.
    for (auto &[id, reply] : std::exchange(_pending, {}))
        reply(nullptr);
    LOG_INFO("claude", "relay on %s: %s", _name.c_str(), why.c_str());
    // Said once per outage: the next tries go quietly.
    std::string error;
    if (!_reported) {
        _reported = true;
        error     = fatal ? why : i18n::arg(tr("Couldn't connect to %1: %2"), _name, why);
    }
    setState(HostState::Link::Down, std::move(error));
    _backoffMs = fatal   ? kMismatchRetryMs
                 : wasUp ? 1000
                         : std::min(kMaxBackoffMs, std::max(1000, _backoffMs * 2));
    if (_retryTimer)
        _app.cancelTimer(_retryTimer);
    _retryTimer = _app.addTimer(_backoffMs, false, [this, alive = _alive] {
        if (*alive)
            connect();
    });
}

void RemoteHost::setState(HostState::Link link, std::string error) {
    if (_state.link == link && error.empty())
        return;
    _state = {link, std::move(error)};
    if (onState)
        onState();
}

void RemoteHost::watchdog() {
    const int64_t now = nowMs();
    if (_channel && _state.link == HostState::Link::Up) {
        if (now - _shared->heardMs >= kSilentMs) {
            linkLost(tr("It stopped answering."));
            return;
        }
        if (now - _shared->sentMs >= kPingMs) {
            _channel->send(R"({"t":"ping"})");
            _shared->sentMs = now;
        }
    }
    const size_t before = _tails.size();
    std::erase_if(_tails, [&](const auto &e) { return now - e.second.askedMs > kTailForgetMs; });
    if (_tails.size() != before) {
        _tailsDirty = true;
        sendTails();
    }
}

void RemoteHost::onLoopLine(const std::string &line) {
    json::Document doc;
    if (!doc.parse(line, nullptr))
        return;
    const json::Value      v = doc.root();
    const std::string_view t = v["t"].str();
    if (t == "hello") {
        linkUp(v);
    } else if (t == "re") {
        const auto it = _pending.find(v["id"].integer());
        if (it == _pending.end())
            return;
        Reply reply = std::move(it->second);
        _pending.erase(it);
        reply(&v);
    } else if (t == "roster") {
        std::vector<SessionInfo> roster;
        for (const json::Value s : v["sessions"]) {
            SessionInfo info;
            readSession(s, &info);
            info.transcriptPath = toMirror(info.transcriptPath);
            roster.push_back(std::move(info));
        }
        _roster = std::move(roster);
        saveSnapshot();
        if (onChanged)
            onChanged();
    } else if (t == "questions") {
        _questions.clear();
        for (const json::Value q : v["q"]) {
            HookQuestion hq;
            readQuestion(q, &hq);
            _questions.push_back(std::move(hq));
        }
        if (onChanged)
            onChanged();
    } else if (t == "tail") {
        const auto it = _tails.find(std::string(v["p"].str()));
        if (it == _tails.end())
            return; // no longer asked about
        it->second.known = true;
        it->second.size  = v["size"].integer(-1);
        it->second.mtime = v["m"].integer();
        it->second.bytes = v["d"].str();
        if (onChanged)
            onChanged();
    } else if (t == "synced") {
        // The mirror has what the latest follow named, as far as it exists there.
        if (v["n"].integer() == _followSeq) {
            _settled = true;
            saveSnapshot();
            if (onChanged)
                onChanged();
        }
    }
}

// ── Requests ────────────────────────────────────────────────────────────────

json::Writer RemoteHost::begin(std::string_view type, int64_t *id) {
    *id = ++_nextId;
    json::Writer w;
    w.beginObject().key("t").value(type).key("id").value(*id);
    return w;
}

void RemoteHost::call(json::Writer &w, int64_t id, Reply reply) {
    w.endObject();
    if (!_channel || _state.link != HostState::Link::Up) {
        // No link: answered at once, never from inside the call.
        _app.post([alive = _alive, reply = std::move(reply)] {
            if (*alive)
                reply(nullptr);
        });
        return;
    }
    _pending[id] = std::move(reply);
    _channel->send(w.take());
    _shared->sentMs = nowMs();
}

namespace {
std::string lostMessage(const std::string &name) {
    return i18n::arg(tr("Lost the connection to %1."), name);
}
} // namespace

void RemoteHost::start(
    const std::string &cwd,
    const std::string &prompt,
    bool               skipPermissionChecks,
    const std::string &rolePrompt,
    const std::string &agentsJson,
    Launcher::Done     done
) {
    int64_t      id;
    json::Writer w = begin("start", &id);
    w.key("cwd").value(cwd).key("prompt").value(prompt).key("skip").value(skipPermissionChecks);
    w.key("role").value(rolePrompt).key("agents").value(agentsJson);
    call(w, id, [done = std::move(done), name = _name](const json::Value *v) {
        if (!v)
            return done({}, lostMessage(name));
        done(json::owned((*v)["session"]), json::owned((*v)["error"]));
    });
}

void RemoteHost::resume(
    const std::string &sessionId,
    const std::string &cwd,
    const std::string &prompt,
    bool               isBackground,
    bool               stopFirst,
    Launcher::Done     done
) {
    int64_t      id;
    json::Writer w = begin("resume", &id);
    w.key("session").value(sessionId).key("cwd").value(cwd).key("prompt").value(prompt);
    w.key("bg").value(isBackground).key("stopFirst").value(stopFirst);
    call(w, id, [done = std::move(done), name = _name](const json::Value *v) {
        if (!v)
            return done({}, lostMessage(name));
        done(json::owned((*v)["session"]), json::owned((*v)["error"]));
    });
}

void RemoteHost::fork(
    const std::string              &sessionId,
    const std::string              &cwd,
    const std::string              &prompt,
    Launcher::Done                  done,
    const std::vector<std::string> &deniedTools
) {
    int64_t      id;
    json::Writer w = begin("fork", &id);
    w.key("session").value(sessionId).key("cwd").value(cwd).key("prompt").value(prompt);
    w.key("denied");
    writeStrings(w, deniedTools);
    call(w, id, [done = std::move(done), name = _name](const json::Value *v) {
        if (!v)
            return done({}, lostMessage(name));
        done(json::owned((*v)["session"]), json::owned((*v)["error"]));
    });
}

void RemoteHost::stop(
    const std::string &sessionId, const std::string &cwd, std::function<void()> done
) {
    int64_t      id;
    json::Writer w = begin("stop", &id);
    w.key("session").value(sessionId).key("cwd").value(cwd);
    call(w, id, [done = std::move(done)](const json::Value *) { done(); });
}

void RemoteHost::remove(
    const std::string                       &sessionId,
    const std::string                       &jobId,
    const std::string                       &cwd,
    std::function<void(std::string refusal)> done
) {
    int64_t      id;
    json::Writer w = begin("remove", &id);
    w.key("session").value(sessionId).key("job").value(jobId).key("cwd").value(cwd);
    call(w, id, [done = std::move(done), name = _name](const json::Value *v) {
        done(v ? json::owned((*v)["refusal"]) : lostMessage(name));
    });
}

void RemoteHost::checkLogin(std::function<void(Login)> done) {
    int64_t      id;
    json::Writer w = begin("login", &id);
    call(w, id, [done = std::move(done)](const json::Value *v) {
        done(v ? parseLogin((*v)["login"].str()) : Login::Unknown);
    });
}

void RemoteHost::listCommands(
    const std::string &cwd, std::function<void(std::vector<SlashCommand>, Account)> done
) {
    int64_t      id;
    json::Writer w = begin("commands", &id);
    w.key("cwd").value(cwd);
    call(w, id, [done = std::move(done)](const json::Value *v) {
        std::vector<SlashCommand> commands;
        Account                   account;
        if (v) {
            for (const json::Value c : (*v)["commands"]) {
                SlashCommand sc;
                readCommand(c, &sc);
                commands.push_back(std::move(sc));
            }
            readAccount((*v)["account"], &account);
        }
        done(std::move(commands), std::move(account));
    });
}

std::shared_ptr<HostTask> RemoteHost::sendLive(
    const std::string &sessionId,
    const std::string &cwd,
    const std::string &prompt,
    AttachInput::Done  done
) {
    int64_t      id;
    json::Writer w    = begin("type", &id);
    auto         task = std::make_shared<Task>();
    task->cancelFn    = [weak = std::weak_ptr<Channel>(_channel), id] {
        if (const auto c = weak.lock())
            c->send(str::concat({R"({"t":"cancel","id":)", str::number(id), "}"}));
    };
    w.key("session").value(sessionId).key("cwd").value(cwd).key("prompt").value(prompt);
    // The reply holds the task: the backend's weak handle reads "typing now"
    // until the relay has answered.
    call(w, id, [task, done = std::move(done), name = _name](const json::Value *v) {
        if (!v)
            return done(AttachInput::Outcome::NotReady, lostMessage(name));
        done(parseInputOutcome((*v)["outcome"].str()), json::owned((*v)["detail"]));
    });
    return task;
}

std::shared_ptr<HostTask> RemoteHost::answer(
    const std::string   &sessionId,
    const std::string   &cwd,
    const std::string   &needs,
    int                  number,
    const std::string   &label,
    AttachAnswer::Result done
) {
    int64_t      id;
    json::Writer w    = begin("answer", &id);
    auto         task = std::make_shared<Task>();
    w.key("session").value(sessionId).key("cwd").value(cwd).key("needs").value(needs);
    w.key("n").value(number).key("label").value(label);
    call(w, id, [task, done = std::move(done), name = _name](const json::Value *v) {
        if (!v)
            return done(AttachAnswer::Outcome::NotReady, std::nullopt, lostMessage(name));
        std::optional<PermissionQuestion> q;
        if ((*v)["question"].isObject()) {
            q.emplace();
            readPermissionQuestion((*v)["question"], &*q);
        }
        done(parseAnswerOutcome((*v)["outcome"].str()), std::move(q), json::owned((*v)["detail"]));
    });
    return task;
}

std::vector<HookQuestion> RemoteHost::pendingQuestions() {
    // Away from the relay, no question can be answered: none shows.
    return _state.link == HostState::Link::Up ? _questions : std::vector<HookQuestion>{};
}

void RemoteHost::takeQuestion(const std::string &id) {
    if (_channel && _state.link == HostState::Link::Up) {
        json::Writer w;
        w.beginObject().key("t").value("take").key("q").value(id).endObject();
        _channel->send(w.take());
    }
}

bool RemoteHost::answerQuestion(const std::string &qid, const std::string &answer) {
    const auto q = std::find_if(_questions.begin(), _questions.end(), [&](const HookQuestion &x) {
        return x.id == qid;
    });
    if (_state.link != HostState::Link::Up || q == _questions.end())
        return false;
    _questions.erase(q); // answered: it goes as it would here
    int64_t      id;
    json::Writer w = begin("reply", &id);
    w.key("q").value(qid).key("answer").value(answer);
    // The hook takes the first answer: a late one is no error worth a word.
    call(w, id, [](const json::Value *) {});
    return true;
}

int64_t RemoteHost::fileSize(const std::string &path) {
    Tail &t   = _tails[path];
    t.askedMs = nowMs();
    if (!t.known && !_tailsDirty) {
        _tailsDirty = true;
        _app.post([this, alive = _alive] {
            if (*alive)
                sendTails();
        });
    }
    return t.known ? t.size : -1;
}

bool RemoteHost::statFile(const std::string &path, int64_t *size, int64_t *mtimeMicros) {
    const int64_t s = fileSize(path);
    if (s < 0)
        return false;
    *size        = s;
    *mtimeMicros = _tails[path].mtime;
    return true;
}

bool RemoteHost::readTail(const std::string &path, int64_t size, size_t max, std::string *out) {
    const auto it = _tails.find(path);
    if (size <= 0 || it == _tails.end() || !it->second.known)
        return false;
    const std::string &b = it->second.bytes;
    *out                 = b.size() > max ? b.substr(b.size() - max) : b;
    return true;
}

void RemoteHost::sendTails() {
    if (!_tailsDirty || !_channel || _state.link != HostState::Link::Up)
        return;
    _tailsDirty = false;
    json::Writer w;
    w.beginObject().key("t").value("tails").key("p").beginArray();
    for (const auto &[path, t] : _tails)
        w.value(path);
    w.endArray().endObject();
    _channel->send(w.take());
}

bool RemoteHost::canStopCommands() const {
    return _state.link == HostState::Link::Up &&
           std::find(_caps.begin(), _caps.end(), kCapStopCmd) != _caps.end();
}

void RemoteHost::stopCommand(
    const std::string &sessionId, const std::string &output, std::function<void(bool)> done
) {
    int64_t      id;
    json::Writer w = begin("stopcmd", &id);
    w.key("session").value(sessionId).key("output").value(output);
    call(w, id, [done = std::move(done)](const json::Value *v) {
        done(v && (*v)["yes"].boolean());
    });
}

void RemoteHost::commandGone(
    const std::string &sessionId, const std::string &output, std::function<void(bool)> done
) {
    int64_t      id;
    json::Writer w = begin("cmdgone", &id);
    w.key("session").value(sessionId).key("output").value(output);
    // No answer: not known to be gone.
    call(w, id, [done = std::move(done)](const json::Value *v) {
        done(v && (*v)["yes"].boolean());
    });
}

void RemoteHost::deleteRecord(
    const std::string &path, const std::string &uuid, std::function<void(bool, std::string)> done
) {
    int64_t      id;
    json::Writer w = begin("delete", &id);
    w.key("path").value(toServer(path)).key("uuid").value(uuid);
    // The rewritten transcript is in the mirror before the answer comes.
    call(w, id, [done = std::move(done), name = _name](const json::Value *v) {
        if (!v)
            return done(false, lostMessage(name));
        done((*v)["ok"].boolean(), json::owned((*v)["error"]));
    });
}

void RemoteHost::reapWorktrees(
    std::vector<WorktreeRef> refs, std::function<void(std::vector<std::string>)> done
) {
    int64_t      id;
    json::Writer w = begin("reap", &id);
    w.key("refs").beginArray();
    std::vector<std::string> all;
    for (const auto &r : refs) {
        w.beginObject().key("path").value(r.path).key("branch").value(r.branch);
        w.key("origin").value(r.origin).endObject();
        all.push_back(r.path);
    }
    w.endArray();
    call(w, id, [done = std::move(done), all](const json::Value *v) {
        done(v ? readStrings((*v)["notDeleted"]) : all);
    });
}

bool RemoteHost::isDir(const std::string &) const {
    return true; // the relay falls back to its home folder for one that isn't
}

std::string RemoteHost::folderProblem(const std::string &dir) const {
    // What only the server can tell (a folder there, trusted) comes back
    // from it when the session starts.
    if (dir.empty() || dir.front() != '/')
        return i18n::arg(tr("%1 isn't a folder."), dir);
    return {};
}

void RemoteHost::upload(
    std::vector<std::string> files, std::function<void(std::vector<std::string>, std::string)> then
) {
    struct Job {
        std::vector<std::string>                                   files, copies;
        std::vector<int64_t>                                       ids;
        std::string                                                failed;
        size_t                                                     answered = 0;
        bool                                                       over     = false;
        std::function<void(std::vector<std::string>, std::string)> then;
        void                                                       finish(std::string failedPath) {
            if (std::exchange(over, true))
                return;
            if (!failedPath.empty())
                then({}, std::move(failedPath));
            else
                then(std::move(copies), {});
        }
    };
    auto job   = std::make_shared<Job>();
    job->files = std::move(files);
    job->then  = std::move(then);
    job->copies.resize(job->files.size());
    if (job->files.empty() || !_channel || _state.link != HostState::Link::Up) {
        _app.post([alive = _alive, job] {
            if (*alive)
                job->finish(job->files.empty() ? std::string() : job->files.front());
        });
        return;
    }
    for (size_t i = 0; i < job->files.size(); ++i) {
        job->ids.push_back(++_nextId);
        _pending[job->ids.back()] = [job, i](const json::Value *v) {
            if (!v || !(*v)["path"].isString())
                return job->finish(job->files[i]);
            job->copies[i] = json::owned((*v)["path"]);
            if (++job->answered == job->files.size())
                job->finish({});
        };
    }
    // Read and sent on a worker (64 MB at most each), in chunks; the relay
    // answers each file once it's written there.
    model::runInBackground(
        _app,
        [job, channel = _channel] {
            for (size_t i = 0; i < job->files.size(); ++i) {
                const std::string &path = job->files[i];
                const int64_t      size = file::size(path);
                const std::string  id   = str::number(job->ids[i]);
                if (size < 0 || size > kMaxUpload) {
                    job->failed = path;
                    return;
                }
                json::Writer w;
                w.beginObject().key("t").value("upload").key("id").value(job->ids[i]);
                w.key("name").value(file::baseName(path)).key("size").value(size).endObject();
                channel->send(w.take());
                std::string chunk;
                for (int64_t at = 0; at < size; at += int64_t(chunk.size())) {
                    if (!file::readRange(path, at, kUploadChunk, &chunk) || chunk.empty())
                        break;
                    channel->send(
                        str::concat(
                            {R"({"t":"chunk","id":)", id, R"(,"d":")", crypto::base64(chunk), "\"}"}
                        )
                    );
                }
                channel->send(str::concat({R"({"t":"end","id":)", id, "}"}));
            }
        },
        [this, alive = _alive, job] {
            if (!*alive || job->failed.empty())
                return;
            for (int64_t id : job->ids)
                _pending.erase(id);
            job->finish(job->failed);
        }
    );
}

void RemoteHost::catalog(std::function<void(std::vector<CatalogEntry>)> done) {
    int64_t      id;
    json::Writer w = begin("catalog", &id);
    call(w, id, [this, alive = _alive, done = std::move(done)](const json::Value *v) {
        std::vector<CatalogEntry> entries;
        if (v)
            for (const json::Value e : (*v)["entries"]) {
                CatalogEntry ce;
                readCatalogEntry(e, &ce);
                ce.transcriptPath = toMirror(ce.transcriptPath);
                entries.push_back(std::move(ce));
            }
        if (*alive && v)
            _catalog = entries; // what adding one of them goes by
        done(std::move(entries));
    });
}

bool RemoteHost::catalogEntry(const std::string &sessionId, CatalogEntry *out) {
    for (const CatalogEntry &e : _catalog)
        if (e.sessionId == sessionId) {
            *out = e;
            return true;
        }
    return false;
}

// ── The roster, what's followed ─────────────────────────────────────────────

std::vector<SessionInfo> RemoteHost::scan(JobStateCache *) {
    if (_state.link == HostState::Link::Up)
        return _roster;
    // Away: the sessions as last heard of, none running as far as msga can
    // tell (no dots, nothing to type into, no question to answer).
    std::vector<SessionInfo> out = _roster;
    for (SessionInfo &s : out) {
        s.running        = false;
        s.awaitsApproval = false;
        s.status.clear();
        s.workerStatus.clear();
        s.needs.clear();
    }
    return out;
}

std::function<std::vector<SessionInfo>()> RemoteHost::scanner() const {
    return [roster = _roster] { return roster; };
}

void RemoteHost::follow(const std::vector<std::string> &sessionIds) {
    std::vector<std::string> ids = sessionIds;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids == _followed)
        return;
    _followed = std::move(ids);
    sendFollow();
}

void RemoteHost::sendFollow() {
    _settled = false;
    ++_followSeq;
    if (!_channel || _state.link != HostState::Link::Up)
        return;
    json::Writer w;
    w.beginObject().key("t").value("follow").key("n").value(_followSeq).key("s");
    writeStrings(w, _followed);
    w.endObject();
    _channel->send(w.take());
}

int64_t RemoteHost::bornMicros(const std::string &path) const {
    base::Lock g(_shared->lock);
    const auto it = _shared->born.find(path);
    return it != _shared->born.end() ? it->second : 0;
}

std::string RemoteHost::toMirror(const std::string &serverPath) const {
    if (_serverHome.empty() || !str::startsWith(serverPath, _serverHome + "/"))
        return {};
    return _paths.home + serverPath.substr(_serverHome.size());
}

std::string RemoteHost::toServer(const std::string &mirrorPath) const {
    if (!str::startsWith(mirrorPath, _paths.home + "/"))
        return mirrorPath;
    return _serverHome + mirrorPath.substr(_paths.home.size());
}

// The roster and the transcripts' births as last heard, kept beside the
// mirror: msga started while the server is away shows what it knew.
void RemoteHost::loadSnapshot() {
    json::Document doc;
    if (!doc.parseFile(_mirrorDir + "/state.json"))
        return;
    const json::Value root = doc.root();
    _serverHome            = json::owned(root["home"]);
    for (const json::Value s : root["roster"]) {
        SessionInfo info;
        readSession(s, &info);
        _roster.push_back(std::move(info));
    }
    base::Lock g(_shared->lock);
    for (const json::Value b : root["born"])
        _shared->born[_paths.home + "/" + std::string(b.key())] = b.integer();
}

void RemoteHost::saveSnapshot() {
    json::Writer w;
    w.beginObject().key("home").value(_serverHome).key("roster").beginArray();
    for (const SessionInfo &s : _roster)
        writeSession(w, s); // mirror paths: this machine's
    w.endArray().key("born").beginObject();
    {
        base::Lock g(_shared->lock);
        for (const auto &[path, b] : _shared->born)
            if (str::startsWith(path, _paths.home + "/"))
                w.key(std::string_view(path).substr(_paths.home.size() + 1)).value(b);
    }
    w.endObject().endObject();
    model::runInBackground(
        _app,
        [path = _mirrorDir + "/state.json", text = w.take()] {
            file::writeAtomic(path, text, 0600);
        },
        [] {}
    );
}

// ── Making one ──────────────────────────────────────────────────────────────

std::string hostKey(std::string_view destination) {
    std::string out;
    for (char c : destination) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        out += keep ? c : (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : '_';
    }
    return out.empty() || out == "." || out == ".." ? std::string("remote") : out;
}

std::unique_ptr<Host>
makeRemoteHost(plat::App &app, const std::string &destination, const std::string &msga) {
    const std::string mirror = dirs().cache + "/hosts/" + hostKey(destination);
    return std::make_unique<RemoteHost>(app, destination, mirror, [destination, msga] {
        const std::string ssh = base::findExecutable("ssh");
        return ssh.empty() ? nullptr : processChannel(ssh, RemoteHost::sshArgs(destination, msga));
    });
}

} // namespace claude
