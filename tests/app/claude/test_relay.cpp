// The relay (docs/claude-remote-relay.md): Claude Code on another machine,
// through `msga --cc-relay` there. The protocol's shapes and rules; then a
// relay in this process on a fake ~/.claude, reached through a loopback
// channel as ssh would carry it — the backend lists, reads and sends to its
// sessions, the link drops and comes back, files are uploaded, questions
// answered; then a real `--cc-relay` child process (this binary).
#include "app/claude/backend.h"
#include "app/claude/channel.h"
#include "app/claude/host_remote.h"
#include "app/claude/relay_protocol.h"
#include "app/claude/relay_server.h"
#include "app/model/jobs.h"
#include "app/model/store.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/host.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "base/thread.h"
#include "base/time.h"
#include "plat/plat.h"
#include "support/test.h"

#include <algorithm>
#include <csignal>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace claude;
using model::ConvRef;
using model::kNoConv;

namespace {

std::string tempDir(const char *what) {
    return base::test::makeTempDir((std::string(what) + "-").c_str(), base::env("HOME"));
}

std::string q(std::string_view s) {
    std::string out;
    json::escapeString(out, s);
    return out;
}

void writeFile(const std::string &path, std::string_view bytes, int mode = 0644) {
    file::makeDirs(file::dirName(path));
    file::writeAtomic(path, bytes, mode);
}

void appendFile(const std::string &path, std::string_view bytes) {
    file::makeDirs(file::dirName(path));
    if (FILE *f = std::fopen(path.c_str(), "ab")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

std::string readText(const std::string &path) {
    std::string s;
    file::readAll(path, &s);
    return s;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

std::string prompt(std::string_view text, std::string_view ts) {
    return str::concat(
        {R"({"type":"user","timestamp":")",
         ts,
         R"(","uuid":")",
         ts,
         R"(-u","message":{"role":"user","content":)",
         q(text),
         R"(},"origin":{"kind":"human"}})",
         "\n"}
    );
}

std::string assistantText(std::string_view text, std::string_view ts) {
    return str::concat(
        {R"({"type":"assistant","timestamp":")",
         ts,
         R"(","uuid":")",
         ts,
         R"(-a","message":{"content":[{"type":"text","text":)",
         q(text),
         "}]}}\n"}
    );
}

std::string turnEnd(std::string_view ts) {
    return str::concat(
        {R"({"type":"system","subtype":"turn_duration","timestamp":")", ts, "\"}\n"}
    );
}

bool waitFor(plat::App &app, const std::function<bool()> &pred, int ms = 8000) {
    const int64_t end = base::monotonicMs() + ms;
    while (!pred()) {
        if (base::monotonicMs() > end)
            return false;
        app.pump(10);
    }
    return true;
}

// The server's ~/.claude: a terminal session S1 (this process stands in for
// its pid) and msga's own folders, both in the test's temp HOME.
struct ServerHome {
    std::string dir, msga, transcript;
    ServerHome() {
        dir  = tempDir("server-claude");
        msga = tempDir("msga-dirs");
        base::test::setEnv("CLAUDE_CONFIG_DIR", dir);
        file::makeDirs(dir + "/sessions");
        file::makeDirs(dir + "/jobs");
        transcript = dir + "/projects/-src-app/S1.jsonl";
        setDirs({msga + "/data", msga + "/cache"});
    }
    ~ServerHome() { base::test::unsetEnv("CLAUDE_CONFIG_DIR"); }
    void writeSession(std::string_view status) {
        json::Writer w;
        w.beginObject().key("pid").value(int64_t(getpid())).key("sessionId").value("S1");
        w.key("cwd").value("/src/app").key("name").value("app-1").key("status").value(status);
        w.key("entrypoint").value("cli").endObject();
        file::writeAtomic(dir + "/sessions/1.json", w.str());
    }
    void append(std::string_view bytes) { appendFile(transcript, bytes); }
};

// The relay in this process, on the same loop: what msga sends goes to a
// RelayServer on a LocalHost over the server's ~/.claude, what it says comes
// back — one RelayServer per connection, as one ssh session each would be.
struct Loopback {
    plat::App                            &app;
    std::string                           approvals, uploads;
    std::unique_ptr<LocalHost>            host;
    std::unique_ptr<RelayServer>          server;
    Channel::Line                         toMsga;
    Channel::Closed                       closed;
    int                                   opened = 0;
    // Every line the relay said, per connection (1-based).
    std::vector<std::vector<std::string>> said;
    // What the relay says first instead of its own hello ("" = its own).
    std::string                           fakeHello;
    std::shared_ptr<int>                  generation = std::make_shared<int>(0);

    Loopback(plat::App &a, const std::string &claudePath = {}) : app(a) {
        approvals = tempDir("relay-approvals");
        uploads   = tempDir("relay-uploads");
        host      = std::make_unique<LocalHost>(app, claudePath, Paths::detect(), approvals);
    }
    ~Loopback() { server.reset(); }

    bool open(Channel::Line onLine, Channel::Closed onClosed) {
        ++opened;
        said.emplace_back();
        toMsga        = std::move(onLine);
        closed        = std::move(onClosed);
        const int gen = ++*generation;
        server = std::make_unique<RelayServer>(app, *host, uploads, [this, gen](std::string line) {
            line.pop_back(); // its newline
            if (!fakeHello.empty() && str::startsWith(line, R"({"t":"hello")"))
                line = fakeHello;
            said.back().push_back(line);
            app.post([this, gen, line, g = generation] {
                if (*g == gen && toMsga)
                    toMsga(line);
            });
        });
        server->start();
        return true;
    }
    void fromMsga(std::string line) {
        const int gen = *generation;
        app.post([this, gen, line = std::move(line)] {
            if (*generation == gen && server)
                server->receive(line);
        });
    }
    // The link dies (as an ssh session does): msga is told, the relay ends.
    void drop() {
        ++*generation;
        server.reset();
        if (auto c = std::exchange(closed, {}))
            c("Connection reset by peer");
        toMsga = {};
    }
    // Lines of connection `n` (1-based) of type `t` naming `path`.
    int count(int n, std::string_view t, std::string_view path = {}) const {
        int k = 0;
        for (const std::string &l : said[size_t(n - 1)])
            k += contains(l, str::concat({R"({"t":")", t, "\""})) &&
                 (path.empty() || contains(l, path));
        return k;
    }
};

class LoopChannel final : public Channel {
public:
    explicit LoopChannel(Loopback &lb) : _lb(lb) {}
    ~LoopChannel() override { close(); }
    bool open(Line onLine, Closed onClosed, std::string *) override {
        _gen = *_lb.generation + 1;
        return _lb.open(std::move(onLine), std::move(onClosed));
    }
    void send(std::string line) override {
        if (*_lb.generation == _gen)
            _lb.fromMsga(std::move(line));
    }
    void close() override {
        if (*_lb.generation == _gen) {
            ++*_lb.generation;
            _lb.server.reset();
            _lb.closed = {};
            _lb.toMsga = {};
        }
    }

private:
    Loopback &_lb;
    int       _gen = 0;
};

std::unique_ptr<RemoteHost> loopHost(plat::App &app, Loopback &lb, const std::string &mirror) {
    return std::make_unique<RemoteHost>(app, "box", mirror, [&lb] {
        return std::make_unique<LoopChannel>(lb);
    });
}

// A backend on a remote host, connected.
struct RemoteRig {
    std::unique_ptr<plat::App> app = plat::App::create();
    model::Store               store;
    std::unique_ptr<Loopback>  lb;
    std::string                mirror;
    RemoteHost                *host = nullptr;
    std::unique_ptr<Backend>   backend;
    std::vector<std::string>   errors;

    explicit RemoteRig(const std::string &claudePath = {}) {
        lb               = std::make_unique<Loopback>(*app, claudePath);
        mirror           = tempDir("mirror");
        auto h           = loopHost(*app, *lb, mirror);
        host             = h.get();
        backend          = std::make_unique<Backend>(store, *app, Credentials{}, std::move(h));
        backend->onError = [this](const std::string &m) { errors.push_back(m); };
        bool connected   = false;
        backend->connect([&](bool, const std::string &) { connected = true; });
        waitFor(*app, [&] { return connected; });
    }
    ~RemoteRig() {
        backend.reset();
        lb.reset();
        model::waitBackground();
    }
    bool wait(const std::function<bool()> &pred, int ms = 8000) { return waitFor(*app, pred, ms); }
    ConvRef ref(std::string_view id) const { return store.findConversation(id); }
    bool    listed(std::string_view id) const {
        const ConvRef r = ref(id);
        return r != kNoConv && store.conversation(r).member;
    }
    const std::vector<model::Message> &load(ConvRef c) {
        bool done = false;
        backend->loadHistory(c, 0, [&](bool, const std::string &) { done = true; });
        wait([&] { return done; }, 5000);
        return store.conversation(c).messages;
    }
    bool answered(ConvRef c, std::string_view text) const {
        for (const auto &m : store.conversation(c).messages)
            if (m.user != store.me && mrkdwn::parse(m.text).text == text)
                return true;
        return false;
    }
};

#ifndef _WIN32
// The CLI on the server, as Claude Code 2.1.282 behaves for a new
// background session: `--bg … -- <prompt>` makes a job, its transcript (the
// prompt, "echo <prompt>", the turn's end) and says "backgrounded · <short>".
constexpr const char *kServerCli = R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
printf '%s\n' "$*" >> "$H/calls.log"
prompt=""
while [ $# -gt 0 ]; do
  case "$1" in --) prompt="$2"; shift ;; esac; shift
done
sid="abcdef11-0000-4000-8000-000000000001"; short=abcdef11
T="$H/projects/-fake/$sid.jsonl"
mkdir -p "$H/jobs/$short" "$H/projects/-fake"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
echo "{\"type\":\"user\",\"uuid\":\"u-1\",\"timestamp\":\"2026-11-18T10:00:00.000Z\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "{\"type\":\"assistant\",\"uuid\":\"u-2\",\"timestamp\":\"2026-11-18T10:00:01.000Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"echo $prompt\"}]}}" >> "$T"
echo "{\"type\":\"system\",\"subtype\":\"turn_duration\",\"uuid\":\"u-3\",\"timestamp\":\"2026-11-18T10:00:02.000Z\"}" >> "$T"
echo "backgrounded · $short"
)SH";
#endif

} // namespace

// ── The protocol ────────────────────────────────────────────────────────────

TEST("relay protocol: shapes go across as they are") {
    SessionInfo s;
    s.sessionId       = "S1";
    s.name            = "app \"1\"";
    s.cwd             = "/src/app";
    s.kind            = SessionInfo::Kind::Background;
    s.status          = "working";
    s.running         = true;
    s.pid             = 4242;
    s.entrypoint      = "cli";
    s.statusSinceMs   = 1'700'000'000'123;
    s.needs           = "approve Bash: rm -rf build";
    s.awaitsApproval  = true;
    s.permissionHook  = true;
    s.suggestedReply  = "yes";
    s.transcriptPath  = "/home/u/.claude/projects/-src-app/S1.jsonl";
    s.workerStatus    = "busy";
    s.jobId           = "abcd1234";
    s.worktreePath    = "/src/app-wt";
    s.commandsRunning = true;
    json::Writer w;
    relay::writeSession(w, s);
    json::Document doc;
    REQUIRE(doc.parse(w.str()));
    SessionInfo back;
    relay::readSession(doc.root(), &back);
    CHECK(back == s);

    HookQuestion hq{"q1", "S1", {{"Yes", R"({"a":1})"}, {"No", "{}"}}};
    json::Writer qw;
    relay::writeQuestion(qw, hq);
    REQUIRE(doc.parse(qw.str()));
    HookQuestion hq2;
    relay::readQuestion(doc.root(), &hq2);
    CHECK_STR(hq2.id, "q1");
    REQUIRE(hq2.options.size() == 2);
    CHECK_STR(hq2.options[0].answer, R"({"a":1})");

    PermissionQuestion pq{"Bash command", {{1, "Yes"}, {2, "No"}}, 1};
    json::Writer       pw;
    relay::writePermissionQuestion(pw, pq);
    REQUIRE(doc.parse(pw.str()));
    PermissionQuestion pq2;
    relay::readPermissionQuestion(doc.root(), &pq2);
    CHECK_STR(pq2.text, "Bash command");
    REQUIRE(pq2.options.size() == 2);
    CHECK(pq2.options[1].number == 2);
    CHECK(pq2.selected == 1);

    CatalogEntry e{"S1", "/t.jsonl", "/src", "title", "first", "last", "engineer", "Engineer", 77};
    json::Writer ew;
    relay::writeCatalogEntry(ew, e);
    REQUIRE(doc.parse(ew.str()));
    CatalogEntry e2;
    relay::readCatalogEntry(doc.root(), &e2);
    CHECK_STR(e2.roleName, "Engineer");
    CHECK(e2.modifiedMs == 77);

    CHECK(relay::parseLogin(relay::loginWord(Login::Out)) == Login::Out);
    CHECK(
        relay::parseInputOutcome(relay::inputOutcomeWord(AttachInput::Outcome::Unconfirmed)) ==
        AttachInput::Outcome::Unconfirmed
    );
    // A word this msga doesn't know is "nothing typed": the message waits.
    CHECK(relay::parseInputOutcome("whatever") == AttachInput::Outcome::NotReady);
    CHECK(relay::parseAnswerOutcome("done") == AttachAnswer::Outcome::Done);
}

TEST("relay protocol: versions agree on the highest both speak, or not at all") {
    relay::Hello a, b;
    a.protocol = 3, a.protocolMin = 1;
    b.protocol = 2, b.protocolMin = 2;
    CHECK(relay::agreedProtocol(a, b) == 2);
    b.protocol = 5, b.protocolMin = 4;
    CHECK(relay::agreedProtocol(a, b) == 0);

    a.caps   = {"attach", "hook"};
    a.home   = "/home/u/.claude";
    a.claude = true;
    json::Document doc;
    REQUIRE(doc.parse(relay::helloLine(a, R"("have":{"x":[1,"2"]})")));
    relay::Hello back;
    REQUIRE(relay::readHello(doc.root(), &back));
    CHECK(back.protocol == 3);
    CHECK(back.protocolMin == 1);
    CHECK(back.caps == a.caps);
    CHECK(back.claude);
    CHECK(doc.root()["have"]["x"][0].integer() == 1);
}

TEST("relay protocol: the mirror takes Claude Code's files only") {
    CHECK(relay::mirrorPathOk("projects/-src-app/S1.jsonl"));
    CHECK(relay::mirrorPathOk("projects/-src-app/S1/subagents/agent-a1.jsonl"));
    CHECK(relay::mirrorPathOk("jobs/abcd1234/state.json"));
    CHECK(relay::mirrorPathOk("history.jsonl"));
    CHECK_FALSE(relay::mirrorPathOk(""));
    CHECK_FALSE(relay::mirrorPathOk("/etc/passwd"));
    CHECK_FALSE(relay::mirrorPathOk("projects/../../.bashrc"));
    CHECK_FALSE(relay::mirrorPathOk("projects//x"));
    CHECK_FALSE(relay::mirrorPathOk("settings.json"));
    CHECK_FALSE(relay::mirrorPathOk("projects\\..\\x"));
    CHECK_FALSE(relay::mirrorPathOk("C:/x"));
    // The ssh command line: no terminal, no password prompt, the destination
    // never taken for an option.
    const auto args = RemoteHost::sshArgs("-oProxyCommand=evil", "/opt/msga dir/msga");
    CHECK(std::find(args.begin(), args.end(), "BatchMode=yes") != args.end());
    const auto dashes = std::find(args.begin(), args.end(), "--");
    REQUIRE(dashes != args.end());
    CHECK_STR(*(dashes + 1), "-oProxyCommand=evil");
    CHECK_STR(args.back(), "'/opt/msga dir/msga' --cc-relay");
    // Not said where: looked for there, a Flatpak install last.
    const std::string found = RemoteHost::sshArgs("box", "").back();
    CHECK(found.find("exec msga --cc-relay") != std::string::npos);
    CHECK(found.find("exec flatpak run com.nisdos.msga --cc-relay") != std::string::npos);
    CHECK(found.find("flatpak") > found.find("/.local/bin/msga"));
#ifndef _WIN32
    {
        // As the server's shell runs it: no msga, a Flatpak one.
        const std::string bin  = tempDir("fake-flatpak");
        const std::string home = tempDir("fake-home");
        writeFile(
            bin + "/flatpak", "#!/bin/sh\n[ \"$1\" = info ] && exit 0\necho \"flatpak $*\"\n", 0755
        );
        base::RunOptions o;
        o.env        = {"PATH=" + bin + ":/usr/bin:/bin", "HOME=" + home};
        const auto r = base::run("/bin/sh", {"-c", found}, o);
        CHECK(r.code == 0);
        CHECK_STR(r.output, "flatpak run com.nisdos.msga --cc-relay\n");
        file::remove(bin + "/flatpak");
        CHECK_STR(base::run("/bin/sh", {"-c", found}, o).output, "msga is not installed there\n");
    }
#endif
    CHECK_STR(hostKey("User@Box:22"), "user_box_22");
}

// ── A relay in this process ─────────────────────────────────────────────────

TEST("relay: the backend lists and reads a remote session, as it grows") {
    ServerHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-11-18T10:00:00.000Z") +
        assistantText("Hello!", "2026-11-18T10:00:01.000Z") + turnEnd("2026-11-18T10:00:02.000Z")
    );
    RemoteRig rig;
    REQUIRE(rig.wait([&] { return rig.listed("S1"); }));
    CHECK(rig.host->state().link == HostState::Link::Up);
    const ConvRef c = rig.ref("S1");
    // A terminal on the server drives it: read-only here too.
    CHECK(contains(rig.store.conversation(c).readOnly, "terminal"));
    REQUIRE(rig.wait([&] { return rig.load(c).size() == 2; }));
    CHECK(rig.answered(c, "Hello!"));
    // The mirror holds the transcript as the server has it.
    const std::string mirrored = rig.mirror + "/home/projects/-src-app/S1.jsonl";
    CHECK(readText(mirrored) == readText(home.transcript));

    // It grows there: the new turn arrives here, sent as what was appended.
    home.append(
        prompt("again", "2026-11-18T10:01:00.000Z") +
        assistantText("Once more.", "2026-11-18T10:01:01.000Z")
    );
    CHECK(rig.wait([&] { return rig.answered(c, "Once more."); }));
    CHECK(rig.lb->count(1, "data", "S1.jsonl") >= 2);
    CHECK(rig.lb->count(1, "reset", "S1.jsonl") == 0);
    CHECK(rig.errors.empty());
}

TEST("relay: a lost link comes back and catches up from where the mirror is") {
    ServerHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-11-18T10:00:00.000Z") +
        assistantText("Hello!", "2026-11-18T10:00:01.000Z")
    );
    RemoteRig rig;
    REQUIRE(rig.wait([&] { return rig.listed("S1"); }));
    const ConvRef c = rig.ref("S1");
    REQUIRE(rig.wait([&] { return rig.load(c).size() == 2; }));

    // The link dies; the session goes on there meanwhile.
    rig.lb->drop();
    CHECK(rig.wait([&] { return rig.host->state().link == HostState::Link::Down; }, 2000));
    // Said once, in the banner.
    CHECK(rig.wait([&] { return !rig.errors.empty(); }, 2000));
    CHECK(contains(rig.errors.front(), "box"));
    // Away, nothing reads as running; the session stays listed.
    CHECK(rig.listed("S1"));
    home.append(
        prompt("meanwhile", "2026-11-18T10:02:00.000Z") +
        assistantText("Done while you were away.", "2026-11-18T10:02:01.000Z")
    );

    // Back within a second or so: the relay's snapshot catches the mirror up.
    REQUIRE(rig.wait([&] { return rig.lb->opened == 2; }, 5000));
    CHECK(rig.wait([&] { return rig.answered(c, "Done while you were away."); }));
    CHECK(rig.host->state().link == HostState::Link::Up);
    // Only what the mirror lacked was sent: no transcript from its start.
    CHECK(rig.lb->count(2, "data", R"("at":0)") == 0);
    CHECK(rig.lb->count(2, "reset") == 0);
    CHECK(readText(rig.mirror + "/home/projects/-src-app/S1.jsonl") == readText(home.transcript));
    CHECK(rig.errors.size() == 1);

    // Rewritten while the link is down (a record deleted there): the mirror's
    // end no longer matches, so the file comes again from its start.
    rig.lb->drop();
    REQUIRE(rig.wait([&] { return rig.host->state().link == HostState::Link::Down; }, 2000));
    writeFile(
        home.transcript,
        prompt("hi", "2026-11-18T10:00:00.000Z") +
            assistantText("Rewritten.", "2026-11-18T10:00:01.000Z")
    );
    REQUIRE(rig.wait([&] { return rig.lb->opened == 3; }, 5000));
    CHECK(rig.wait([&] { return rig.lb->count(3, "reset", "S1.jsonl") == 1; }));
    CHECK(rig.wait([&] {
        return readText(rig.mirror + "/home/projects/-src-app/S1.jsonl") ==
               readText(home.transcript);
    }));
    CHECK(rig.wait([&] { return rig.answered(c, "Rewritten."); }));
}

#ifndef _WIN32
TEST("relay: a message to a new remote session starts it there") {
    ServerHome        home;
    const std::string work = tempDir("work");
    const std::string cli  = work + "/claude";
    writeFile(cli, kServerCli, 0755);
    RemoteRig rig(cli);
    REQUIRE(rig.wait([&] { return rig.host->state().link == HostState::Link::Up; }));
    CHECK(rig.host->hasCli());

    ConvRef     conv = kNoConv;
    std::string error;
    bool        done = false;
    rig.backend->startAgentSession(work, false, {}, [&](ConvRef c, const std::string &e) {
        conv  = c;
        error = e;
        done  = true;
    });
    REQUIRE(rig.wait([&] { return done; }));
    REQUIRE(conv != kNoConv);
    rig.load(conv);
    bool ok = false;
    rig.backend->send(conv, "first", 0, [&](bool success, const std::string &) { ok = success; });
    // Launched by the relay's CLI, its transcript mirrored, its answer here.
    CHECK(rig.wait([&] { return rig.answered(conv, "echo first"); }));
    CHECK(ok);
    CHECK(contains(readText(home.dir + "/calls.log"), "--bg"));
    CHECK(rig.errors.empty());
    // Its job's state is in the mirror too.
    CHECK(file::exists(rig.mirror + "/home/jobs/abcdef11/state.json"));
}
#endif

TEST("relay: files attached to a prompt are uploaded to the server") {
    ServerHome                 home;
    std::unique_ptr<plat::App> app = plat::App::create();
    {
        Loopback          lb(*app);
        const std::string mirror = tempDir("mirror");
        auto              host   = loopHost(*app, lb, mirror);
        REQUIRE(waitFor(*app, [&] { return host->state().link == HostState::Link::Up; }));

        // Several chunks' worth of bytes, every value.
        std::string bytes;
        for (int i = 0; i < 600'000; ++i)
            bytes.push_back(char((i * 7) & 0xff));
        const std::string local = tempDir("local") + "/shot one.png";
        writeFile(local, bytes);

        std::vector<std::string> copies;
        std::string              failed = "?";
        host->upload({local}, [&](std::vector<std::string> c, std::string f) {
            copies = std::move(c);
            failed = std::move(f);
        });
        REQUIRE(waitFor(*app, [&] { return failed != "?"; }));
        CHECK(failed.empty());
        REQUIRE(copies.size() == 1);
        CHECK(str::startsWith(copies[0], lb.uploads + "/"));
        CHECK_STR(std::string(file::baseName(copies[0])), "shot one.png");
        CHECK(readText(copies[0]) == bytes);
        CHECK(lb.count(1, "re") == 1);

        // One that can't be read fails the send, by its name.
        failed = "?";
        host->upload({local + ".missing"}, [&](std::vector<std::string>, std::string f) {
            failed = std::move(f);
        });
        REQUIRE(waitFor(*app, [&] { return failed != "?"; }));
        CHECK_STR(failed, local + ".missing");
        host.reset();
    }
    model::waitBackground();
}

TEST("relay: permission questions and command output come from the server") {
    ServerHome                 home;
    std::unique_ptr<plat::App> app = plat::App::create();
    {
        Loopback          lb(*app);
        const std::string mirror = tempDir("mirror");
        auto              host   = loopHost(*app, lb, mirror);
        REQUIRE(waitFor(*app, [&] { return host->state().link == HostState::Link::Up; }));

        // The hook on the server leaves a question; msga holds it, answers it.
        writeFile(
            lb.approvals + "/q7.json",
            R"({"session_id":"S1","cwd":"/src/app","hook_event_name":"PermissionRequest",)"
            R"("tool_name":"Bash",)"
            R"("tool_input":{"command":"make"},"permission_suggestions":[]})"
        );
        REQUIRE(waitFor(*app, [&] { return host->pendingQuestions().size() == 1; }));
        const HookQuestion question = host->pendingQuestions()[0];
        CHECK_STR(question.id, "q7");
        CHECK_STR(question.sessionId, "S1");
        REQUIRE(!question.options.empty());
        host->takeQuestion("q7");
        CHECK(waitFor(*app, [&] { return file::exists(lb.approvals + "/q7.seen"); }));
        CHECK(host->answerQuestion("q7", question.options[0].answer));
        CHECK(waitFor(*app, [&] {
            return readText(lb.approvals + "/q7.answer") == question.options[0].answer;
        }));
        CHECK(host->pendingQuestions().empty());

        // A background command's output on the server: its end, as it grows.
        const std::string output = tempDir("task") + "/b1.output";
        writeFile(output, "building\n3/10 done\n");
        CHECK(host->fileSize(output) == -1); // asked for: it comes
        REQUIRE(waitFor(*app, [&] { return host->fileSize(output) == 19; }));
        std::string tail;
        CHECK(host->readTail(output, 19, 8, &tail));
        CHECK_STR(tail, "10 done\n");
        appendFile(output, "7/10 done\n");
        CHECK(waitFor(*app, [&] { return host->fileSize(output) == 29; }));
        host.reset();
    }
    model::waitBackground();
}

TEST("relay: a relay that speaks another version is refused, and said so") {
    ServerHome                 home;
    std::unique_ptr<plat::App> app = plat::App::create();
    {
        Loopback lb(*app);
        lb.fakeHello = R"({"t":"hello","proto":9,"min":9,"msga":99,"caps":[],"claude":true})";
        const std::string mirror = tempDir("mirror");
        auto              host   = loopHost(*app, lb, mirror);
        int               told   = 0;
        host->onState            = [&] { told += !host->state().error.empty(); };
        REQUIRE(waitFor(*app, [&] { return host->state().link == HostState::Link::Down; }));
        CHECK(contains(host->state().error, "version"));
        CHECK(told == 1);
        CHECK_FALSE(host->settled());
        host.reset();
    }
    model::waitBackground();
}

#ifndef _WIN32
// ── A real relay process ────────────────────────────────────────────────────

TEST("relay: `--cc-relay` as a process of its own, over pipes") {
    ServerHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-11-18T10:00:00.000Z") +
        assistantText("Hello from a child.", "2026-11-18T10:00:01.000Z")
    );
    // The relay keeps its uploads and questions out of the real /tmp, and
    // finds a stand-in `claude` first on its PATH.
    base::test::setEnv("TMPDIR", tempDir("relay-tmp"));
    const std::string bin = tempDir("bin");
    writeFile(
        bin + "/claude",
        "#!/bin/sh\n[ \"$1\" = auth ] && { echo '{\"loggedIn\":true}'; exit 0; }\nexit 1\n",
        0755
    );
    const std::string path = base::env("PATH");
    base::test::setEnv("PATH", bin + ":" + path);
    std::unique_ptr<plat::App> app = plat::App::create();
    {
        const std::string mirror = tempDir("mirror");
        const std::string self   = base::executablePath();
        RemoteHost        host(*app, "child", mirror, [self] {
            return processChannel(self, {std::string(relay::kFlag)});
        });
        int               changes = 0;
        host.onChanged            = [&] { ++changes; };
        REQUIRE(waitFor(*app, [&] { return host.state().link == HostState::Link::Up; }));
        host.follow({"S1"});
        CHECK(waitFor(*app, [&] {
            const auto roster = host.scan(nullptr);
            return roster.size() == 1 && roster[0].sessionId == "S1" && roster[0].running;
        }));
        CHECK(waitFor(*app, [&] { return host.settled(); }));
        CHECK(readText(mirror + "/home/projects/-src-app/S1.jsonl") == readText(home.transcript));
        Login login   = Login::In;
        bool  checked = false;
        host.checkLogin([&](Login l) {
            login   = l;
            checked = true;
        });
        REQUIRE(waitFor(*app, [&] { return checked; }));
        CHECK(login == Login::In); // asked of the stand-in, there
        CHECK(changes > 0);
    }
    base::test::setEnv("PATH", path);
    base::test::unsetEnv("TMPDIR");
    model::waitBackground();
}
#endif

#ifdef MSGA_HOST_EXEC
TEST("relay (sandbox): the channel's program runs on the host, its session ended on close") {
    // ssh in a Flatpak sandbox is the host's (base/host.h); a ProxyCommand it
    // started (here a sleep) goes with it when the channel closes.
    const std::string       dir     = tempDir("sandbox-channel");
    const std::string       pidFile = dir + "/child.pid";
    base::test::FakeSandbox box;
    auto                    ch = processChannel(
        "/bin/sh",
        {"-c", "sleep 30 & echo $! > '" + pidFile + "'; while read -r l; do echo \"got $l\"; done"}
    );
    base::Mutex              lock;
    std::vector<std::string> lines;
    std::string              error;
    REQUIRE(ch->open(
        [&](std::string_view l) {
            base::Lock g(lock);
            lines.emplace_back(l);
        },
        [](std::string) {},
        &error
    ));
    CHECK(box.spawns() >= 1);
    CHECK(box.lastOptions().find("--forward-fd=3\n") != std::string::npos);
    ch->send("ping");
    bool echoed = false;
    for (int i = 0; i < 300 && !echoed; ++i) {
        {
            base::Lock g(lock);
            echoed = !lines.empty() && lines[0] == "got ping";
        }
        if (!echoed)
            usleep(10000);
    }
    CHECK(echoed);
    std::string pid = readText(pidFile);
    REQUIRE(!pid.empty());
    ch->close();
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        gone = ::kill(pid_t(std::stoll(pid)), 0) != 0;
        if (!gone)
            usleep(10000);
    }
    CHECK(gone);
}
#endif
