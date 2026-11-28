// Agent thread links (app/claude/links.h) between two fake backends: a Slack
// workspace whose threads are a list in memory, and a Claude Code workspace
// whose branches the test answers by hand. Linking and the first turn, the
// status line and its edits, the labelled answer, who starts a turn and
// what is only context, the quiet wait and the queue, a branch kept or
// replaced as its session goes on, failures, unlinking, a removed session,
// and a restart in the middle of a turn. Nothing talks to Slack or Claude.
#include "app/claude/links.h"
#include "app/model/jobs.h"
#include "app/model/null_backend.h"
#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "plat/plat.h"
#include "support/test.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using model::ConvRef;
using model::Ts;
using Turn    = model::Backend::AgentTurn;
using Phase   = model::Backend::AgentPhase;
using Reply   = model::Backend::ThreadReply;
using Replies = std::vector<Reply>;

namespace {

struct AppDeleter {
    void operator()(plat::App *a) const {
        model::waitBackground();
        delete a;
    }
};
using TestApp = std::unique_ptr<plat::App, AppDeleter>;

TestApp headlessApp() {
    base::test::setEnv("PLAT_BACKEND", "headless");
    std::string err;
    TestApp     app(plat::App::create(&err).release());
    if (!app)
        std::fprintf(stderr, "    plat::App::create: %s\n", err.c_str());
    return app;
}

const Ts kRoot = model::parseTs("1700000000.000100");

bool contains(std::string_view s, std::string_view part) {
    return s.find(part) != std::string_view::npos;
}

// The Slack side: one channel's threads as lists, every call answered on
// the loop's next turn.
class FakeOw final : public model::NullBackend {
public:
    FakeOw(model::Store &st, plat::App &app) : NullBackend(st), _app(app) {}

    struct Post {
        Ts          ts = 0;
        std::string text, blocks;
        bool        files = false;
    };
    std::vector<Post>        posts;
    std::vector<std::string> edits; // "ts:text"
    std::vector<Ts>          deleted;
    Replies                  thread; // the root and its replies, oldest first
    bool                     busy = false, watched = false, failDelete = false;
    Ts                       watchAfter = 0;
    bool                     postsFail  = false;

    // A ts after everything in the thread (1 ms on).
    Ts newer() const { return (thread.empty() ? kRoot : thread.back().message.ts) + 1000; }

    Reply &add(Ts ts, model::UserRef user, std::string text, bool agent = false) {
        Reply r;
        r.message.ts       = ts;
        r.message.threadTs = kRoot;
        r.message.user     = user;
        r.message.text     = std::move(text);
        r.agentReply       = agent;
        thread.push_back(std::move(r));
        return thread.back();
    }
    // A watch report of the replies newer than `after`.
    void report(ConvRef c, Ts after) {
        Replies out;
        for (const Reply &r : thread)
            if (r.message.ts > after) {
                Reply x;
                x.message    = r.message.clone();
                x.agentReply = r.agentReply;
                out.push_back(std::move(x));
            }
        if (onThreadReplies)
            onThreadReplies(c, kRoot, std::move(out), {});
    }

    void
    postAgentReply(ConvRef, Ts root, std::string text, std::string blocks, PostDone done) override {
        const Ts ts = postsFail ? 0 : newer();
        posts.push_back({ts, text, blocks, false});
        if (ts)
            add(ts, _store.me, text, true).message.threadTs = root;
        _app.post([done, ts] { done(ts, ts ? "" : "not_in_channel"); });
    }
    void editAgentReply(ConvRef, Ts ts, std::string text, std::string, Done done) override {
        edits.push_back(model::formatTs(ts) + ":" + text);
        for (Reply &r : thread)
            if (r.message.ts == ts)
                r.message.text = text;
        if (done)
            _app.post([done] { done(true, {}); });
    }
    void deleteAgentReply(ConvRef, Ts ts, Done done) override {
        if (!failDelete) {
            deleted.push_back(ts);
            std::erase_if(thread, [ts](const Reply &r) { return r.message.ts == ts; });
        }
        const bool ok = !failDelete;
        _app.post([done, ok] { done(ok, ok ? "" : "cant_delete_message"); });
    }
    void postAgentFiles(
        ConvRef, Ts, std::string, std::vector<std::string> files, PostDone done
    ) override {
        const Ts ts = newer();
        posts.push_back(
            {ts, str::concat({"files:", str::number(int64_t(files.size()))}), {}, true}
        );
        add(ts, _store.me, {});
        _app.post([done, ts] { done(ts, {}); });
    }
    void loadThreadReplies(ConvRef, Ts, Ts after, RepliesDone done) override {
        Replies out;
        for (const Reply &r : thread)
            if (r.message.ts > after) {
                Reply x;
                x.message    = r.message.clone();
                x.agentReply = r.agentReply;
                out.push_back(std::move(x));
            }
        auto shared = std::make_shared<Replies>(std::move(out));
        _app.post([done, shared] { done(std::move(*shared), {}); });
    }
    void watchThread(ConvRef, Ts, Ts after, bool b) override {
        watched    = true;
        watchAfter = after;
        busy       = b;
    }
    void unwatchThread(ConvRef, Ts) override { watched = false; }

private:
    plat::App &_app;
};

// The Claude Code side: what was asked, and the turns' observers the test
// answers through.
class FakeAgents final : public model::NullBackend {
public:
    FakeAgents(model::Store &st, plat::App &app) : NullBackend(st), _app(app) {}

    struct Ask {
        bool                     fresh = false;
        std::string              target; // the session (fresh) or the branch
        std::string              prompt;
        std::vector<std::string> files, denied;
    };
    std::vector<Ask>                                 asks;
    AgentBranchDone                                  started;
    AgentTurnFn                                      turn;
    std::vector<std::pair<std::string, std::string>> labels;
    bool                                             movedOn = false;
    int                                              watches = 0;

    void startAgentBranch(
        const std::string       &session,
        std::string              prompt,
        std::vector<std::string> files,
        std::vector<std::string> denied,
        AgentBranchDone          s,
        AgentTurnFn              t
    ) override {
        asks.push_back({true, session, std::move(prompt), std::move(files), std::move(denied)});
        started = std::move(s);
        turn    = std::move(t);
    }
    void continueAgentBranch(
        const std::string &branch, std::string prompt, std::vector<std::string> files, AgentTurnFn t
    ) override {
        asks.push_back({false, branch, std::move(prompt), std::move(files), {}});
        turn = std::move(t);
    }
    void watchAgentBranch(const std::string &, AgentTurnFn t) override {
        ++watches;
        turn = std::move(t);
    }
    void agentSessionMovedOn(
        const std::string &, const std::string &, std::function<void(bool)> d
    ) override {
        const bool m = movedOn;
        _app.post([d, m] { d(m); });
    }
    void setAgentBranchLabel(const std::string &branch, std::string label) override {
        labels.emplace_back(branch, std::move(label));
    }
    // Answers the branch start, then the turn.
    void start(std::string id, Ts root, std::string forkPoint = "fp1") {
        model::Backend::AgentBranch b;
        b.id        = std::move(id);
        b.root      = root;
        b.forkPoint = std::move(forkPoint);
        b.conv      = 0;
        started(std::move(b), {});
    }
    void progress(Phase p, std::string status) {
        Turn t;
        t.phase  = p;
        t.status = std::move(status);
        turn(t);
    }
    void answer(std::string text, std::string id, std::vector<model::File> files = {}) {
        Turn t;
        t.done     = true;
        t.answer   = std::move(text);
        t.answerId = std::move(id);
        t.files    = std::move(files);
        turn(t);
    }
    void fail(std::string error) {
        Turn t;
        t.done  = true;
        t.error = std::move(error);
        turn(t);
    }

private:
    plat::App &_app;
};

// Both workspaces and the links between them.
struct Rig {
    TestApp                        app = headlessApp();
    std::string                    dir = base::test::makeTempDir("links_test_");
    model::Store                   ow, cc;
    std::unique_ptr<FakeOw>        owBe;
    std::unique_ptr<FakeAgents>    agents;
    std::unique_ptr<claude::Links> links;
    model::UserRef                 me = 0, mira = 0, bob = 0;
    ConvRef                        chan = model::kNoConv, session = model::kNoConv;
    std::vector<std::string>       errors;

    Rig() {
        owBe             = std::make_unique<FakeOw>(ow, *app);
        agents           = std::make_unique<FakeAgents>(cc, *app);
        ow.workspaceId   = "T1";
        ow.workspaceName = "Lumen";
        me               = user("U0ME", "Me Myself");
        mira             = user("U0MIRA", "Mira");
        bob              = user("U0BOB", "Bob");
        ow.me            = me;
        model::Conversation c;
        c.id   = "C1";
        c.name = "general";
        chan   = ow.addConversation(std::move(c));
        model::Message root;
        root.ts       = kRoot;
        root.threadTs = kRoot;
        root.user     = mira;
        root.text     = "how does the importer work?";
        ow.addMessage(chan, root.clone());
        owBe->add(kRoot, mira, root.text);
        model::Conversation s;
        s.id    = "s1";
        s.name  = "importer";
        s.kind  = model::ConvKind::Dm;
        session = cc.addConversation(std::move(s));
        open();
    }
    ~Rig() {
        links.reset();
        base::test::removeTree(dir);
    }
    model::UserRef user(const char *id, const char *name) {
        model::User u;
        u.id          = id;
        u.displayName = name;
        return ow.addUser(std::move(u));
    }
    // Links over both workspaces, from what links.json kept.
    void open() {
        links = std::make_unique<claude::Links>(*app, dir + "/links.json", dir + "/cache");
        links->setTimings(60, 150);
        links->onError = [this](const std::string &m) { errors.push_back(m); };
        links->attach(ow, *owBe);
        links->ready(ow);
        links->setAgents(&cc, agents.get());
        links->agentsReady();
    }
    void restart() {
        links.reset(); // writes what waited
        agents->turn    = nullptr;
        agents->started = nullptr;
        open();
    }
    bool wait(const std::function<bool()> &pred, int ms = 3000) {
        const int64_t end = base::monotonicMs() + ms;
        while (!pred()) {
            if (base::monotonicMs() > end)
                return false;
            app->pump(5);
        }
        return true;
    }
    void pumpFor(int ms) {
        const int64_t end = base::monotonicMs() + ms;
        while (base::monotonicMs() < end)
            app->pump(5);
    }
    const claude::Links::Link *link() const {
        return links->all().empty() ? nullptr : links->all().front().get();
    }
    // Linked on the root, its branch started: the first turn under way.
    void linkAndStart() {
        const size_t before = agents->asks.size();
        links->link(ow, chan, kRoot, "s1");
        REQUIRE(wait([&] { return agents->asks.size() == before + 1; }));
        agents->start("b1", model::parseTs("1700000500.000000"));
    }
    void answerFirst() {
        agents->answer("It reads the CSV.", "a1");
        REQUIRE(wait([&] { return link() && !link()->running; }));
    }
};

} // namespace

TEST("links: asking posts a status line, a branch answers, the answer goes up labelled") {
    Rig r;
    r.links->link(r.ow, r.chan, kRoot, "s1");
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 1; }));
    REQUIRE(r.link() != nullptr);
    CHECK(r.link()->running);
    CHECK(r.link()->askers == std::vector<std::string>{"U0MIRA"});
    // The status line at once, before the answer.
    REQUIRE(r.owBe->posts.size() == 1);
    CHECK_STR(r.owBe->posts[0].text, "\xF0\x9F\xA4\x96 Looking into it\xE2\x80\xA6");
    CHECK(r.owBe->watched && r.owBe->busy);
    // A fresh branch of the session, framed, the thread pasted, tools off.
    const FakeAgents::Ask &ask = r.agents->asks[0];
    CHECK(ask.fresh);
    CHECK_STR(ask.target, "s1");
    CHECK(contains(ask.prompt, "Workspace: Lumen, #general. Asked by: Mira."));
    CHECK(contains(ask.prompt, "untrusted input"));
    CHECK(contains(
        ask.prompt,
        "<pasted_content>\nMira (to you): how does the importer work?\n</pasted_content>"
    ));
    for (const char *tool : {"Bash", "Edit", "Write", "WebFetch", "Agent", "mcp__*"})
        CHECK(std::find(ask.denied.begin(), ask.denied.end(), tool) != ask.denied.end());

    // The branch is there: its root shows a short line, the link keeps it.
    r.agents->start("b1", model::parseTs("1700000500.000000"));
    REQUIRE(r.agents->labels.size() == 1);
    CHECK_STR(r.agents->labels[0].first, "b1");
    CHECK_STR(
        r.agents->labels[0].second, "\xF0\x9F\x94\x97 Mira in Lumen: how does the importer work?"
    );
    CHECK(r.link()->branches.size() == 1 && r.link()->forkPoint == "fp1");
    CHECK(r.links->findBranch(r.session, model::parseTs("1700000500.000000")) == r.link());

    // Progress edits the line: a change at once, the next after the gap.
    r.agents->progress(Phase::Reading, "Reading files\xE2\x80\xA6");
    REQUIRE(r.wait([&] { return r.owBe->edits.size() == 1; }));
    CHECK(contains(r.owBe->edits[0], "Reading files"));
    r.agents->progress(Phase::Searching, "Searching\xE2\x80\xA6");
    r.agents->progress(Phase::Searching, "Searching\xE2\x80\xA6");
    r.app->pump(5);
    CHECK(r.owBe->edits.size() == 1); // within the gap
    REQUIRE(r.wait([&] { return r.owBe->edits.size() == 2; }));
    CHECK(contains(r.owBe->edits[1], "Searching"));

    // The answer: rendered, marked 🤖 in text and blocks; the line goes. The
    // link's first answer says once what 🤖 marks.
    const Ts status = r.owBe->posts[0].ts;
    r.agents->answer("**Yes.** It reads the CSV.", "a1");
    REQUIRE(r.wait([&] { return !r.link()->running; }));
    REQUIRE(r.owBe->posts.size() == 2);
    CHECK_STR(r.owBe->posts[1].text, "\xF0\x9F\xA4\x96: *Yes.* It reads the CSV.");
    json::Document blocks;
    REQUIRE(blocks.parse(r.owBe->posts[1].blocks, nullptr));
    CHECK_STR(blocks.root()[0]["type"].str(), "context"); // the note first
    CHECK_STR(
        blocks.root()[0]["elements"][0]["text"].str(),
        "\xF0\x9F\xA4\x96-marked replies are generated by AI."
    );
    CHECK_STR(blocks.root()[1]["text"]["text"].str(), "\xF0\x9F\xA4\x96: *Yes.* It reads the CSV.");
    CHECK(r.owBe->deleted == std::vector<Ts>{status});
    CHECK(r.link()->status == 0 && r.link()->answered == "a1");
    CHECK(r.owBe->watched && !r.owBe->busy);
    CHECK(r.errors.empty());
}

TEST("links: only a 🤖 from an asker or the user starts a turn; the rest is context") {
    Rig r;
    r.linkAndStart();
    r.answerFirst();
    const size_t asks = r.agents->asks.size();
    // People talking (no 🤖), a 🤖 from Bob (not allowed), msga's own post:
    // no turn.
    r.owBe->add(r.owBe->newer(), r.mira, "Bob, did you see this?");
    r.owBe->add(r.owBe->newer(), r.bob, "\xF0\x9F\xA4\x96 same question here");
    r.owBe->add(r.owBe->newer(), r.me, "check staging, not prod");
    r.owBe->add(r.owBe->newer(), r.me, "\xF0\x9F\xA4\x96 stray status", true);
    r.owBe->report(r.chan, r.link()->lastSeen);
    r.pumpFor(200);
    CHECK(r.agents->asks.size() == asks);
    CHECK(r.link()->pending.empty());
    const size_t posts = r.owBe->posts.size();
    // Mira calls it twice in a row (emoji, then Slack's shortcode): one
    // turn, once she's quiet.
    r.owBe->add(r.owBe->newer(), r.mira, "\xF0\x9F\xA4\x96 and the exporter?");
    r.owBe->report(r.chan, r.link()->lastSeen);
    r.owBe->add(r.owBe->newer(), r.mira, "the new one :robot_face:");
    r.owBe->report(r.chan, r.link()->lastSeen);
    CHECK(r.link()->pending.size() == 2);
    // Seen: the status line goes up at once, before the quiet wait is over,
    // and only once.
    REQUIRE(r.owBe->posts.size() == posts + 1);
    CHECK(contains(r.owBe->posts.back().text, "Looking into it"));
    CHECK(r.agents->asks.size() == asks);
    REQUIRE(r.wait([&] { return r.agents->asks.size() == asks + 1; }));
    CHECK(r.owBe->posts.size() == posts + 1);
    // The session hasn't moved on: the same branch goes on, told what came
    // since the last answer; the questions marked, without their 🤖.
    const FakeAgents::Ask &ask = r.agents->asks.back();
    CHECK_FALSE(ask.fresh);
    CHECK_STR(ask.target, "b1");
    CHECK(contains(ask.prompt, "Mira asked you again"));
    CHECK(contains(ask.prompt, "Mira: Bob, did you see this?\n"));
    CHECK(contains(ask.prompt, "Bob: \xF0\x9F\xA4\x96 same question here\n"));
    CHECK(contains(ask.prompt, "Me Myself (the person whose session this is): check staging"));
    CHECK(contains(ask.prompt, "Mira (to you): and the exporter?\nMira (to you): the new one\n"));
    CHECK_FALSE(contains(ask.prompt, "stray status"));
    CHECK_FALSE(contains(ask.prompt, "It reads the CSV")); // answered already
    CHECK(r.link()->pending.empty());
    // The user calls it too, even on a thread they started themselves.
    r.agents->answer("The exporter writes JSON.", "a2");
    REQUIRE(r.wait([&] { return !r.link()->running; }));
    // Answers after the first: just the 🤖, no note.
    CHECK_STR(r.owBe->posts.back().text, "\xF0\x9F\xA4\x96: The exporter writes JSON.");
    json::Document later;
    REQUIRE(later.parse(r.owBe->posts.back().blocks, nullptr));
    CHECK(later.root().size() == 1);
    r.owBe->add(r.owBe->newer(), r.me, "\xF0\x9F\xA4\x96 and in which folder?");
    r.owBe->report(r.chan, r.link()->lastSeen);
    REQUIRE(r.wait([&] { return r.agents->asks.size() == asks + 2; }));
    CHECK(contains(
        r.agents->asks.back().prompt,
        "Me Myself (the person whose session this is) (to you): and in which folder?"
    ));
}

TEST("links: a session that went on gets a new branch, told the thread so far") {
    Rig r;
    r.linkAndStart();
    r.answerFirst();
    r.agents->movedOn = true;
    r.owBe->add(r.owBe->newer(), r.mira, "\xF0\x9F\xA4\x96 and the exporter?");
    r.owBe->report(r.chan, r.link()->lastSeen);
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 2; }));
    const FakeAgents::Ask &ask = r.agents->asks.back();
    CHECK(ask.fresh);
    CHECK_STR(ask.target, "s1");
    CHECK(contains(ask.prompt, "Earlier in the thread:\nMira: how does the importer work?\n"));
    CHECK(contains(ask.prompt, "You (an earlier answer): It reads the CSV.\n"));
    CHECK(contains(ask.prompt, "\nNew:\nMira (to you): and the exporter?\n"));
    r.agents->start("b2", model::parseTs("1700000600.000000"), "fp2");
    CHECK(r.link()->branches.size() == 2 && r.link()->forkPoint == "fp2");
    CHECK(r.links->branchRoot(*r.link()) == model::parseTs("1700000600.000000"));
}

TEST("links: messages during a turn are the next turn; allowed people ask too") {
    Rig r;
    r.linkAndStart();
    // Bob, allowed now, asks while the first turn runs: queued.
    r.links->allow(r.link()->id, "U0BOB");
    r.owBe->add(r.owBe->newer(), r.bob, "\xF0\x9F\xA4\x96 does it do xlsx?");
    r.owBe->report(r.chan, r.link()->lastSeen);
    r.pumpFor(200);
    CHECK(r.agents->asks.size() == 1);
    CHECK(r.link()->pending.size() == 1);
    r.agents->answer("It reads the CSV.", "a1");
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 2; }));
    CHECK(contains(r.agents->asks.back().prompt, "Bob (to you): does it do xlsx?"));
}

TEST("links: a failed turn says so in the thread and on the banner") {
    Rig r;
    r.linkAndStart();
    r.agents->fail("Claude Code isn't logged in.\nRun claude.");
    REQUIRE(r.wait([&] { return !r.link()->running && !r.owBe->edits.empty(); }));
    CHECK(contains(r.owBe->edits.back(), "Couldn't answer: Claude Code isn't logged in."));
    CHECK_FALSE(contains(r.owBe->edits.back(), "Run claude"));
    REQUIRE(r.errors.size() == 1);
    CHECK(contains(r.errors[0], "#general in Lumen"));
    // The status line stays saying so; the next turn has its own.
    r.owBe->add(r.owBe->newer(), r.mira, "\xF0\x9F\xA4\x96 try again?");
    r.owBe->report(r.chan, r.link()->lastSeen);
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 2; }));
    CHECK(r.owBe->posts.size() == 2);
    // Told the question that went unanswered too.
    CHECK(contains(r.agents->asks.back().prompt, "how does the importer work?"));
}

TEST("links: a status line that can't be deleted says the answer is below") {
    Rig r;
    r.owBe->failDelete = true;
    r.linkAndStart();
    r.answerFirst();
    REQUIRE(r.wait([&] { return !r.owBe->edits.empty(); }));
    CHECK(contains(r.owBe->edits.back(), "Answered below"));
}

TEST("links: a long answer goes in parts at its paragraphs; files under it") {
    Rig         r;
    std::string para(1000, 'a'), text;
    for (int i = 0; i < 5; ++i)
        text += (i ? "\n\n" : "") + para;
    r.linkAndStart();
    model::File f;
    f.name = "out.csv";
    f.path = r.dir + "/out.csv";
    r.agents->answer(text, "a1", {f});
    REQUIRE(r.wait([&] { return !r.link()->running; }));
    // Status, three parts (two paragraphs fit in one), the files.
    REQUIRE(r.owBe->posts.size() == 5);
    for (int i = 1; i <= 3; ++i)
        CHECK(r.owBe->posts[size_t(i)].text.size() < 3100);
    CHECK(r.owBe->posts[4].files);
    CHECK(r.link()->answers.size() == 3);
    CHECK(r.link()->posts.size() == 5);
}

TEST("links: unlinking, and a session removed from msga, end the link") {
    Rig r;
    r.linkAndStart();
    r.links->unlink(r.link()->id);
    CHECK(r.links->all().empty());
    CHECK_FALSE(r.owBe->watched);
    REQUIRE(r.wait([&] { return r.owBe->deleted.size() == 1; })); // its status line
    r.agents->answer("late", "a1");                               // the turn let go: nothing posted
    r.pumpFor(100);
    CHECK(r.owBe->posts.size() == 1);

    r.linkAndStart();
    r.answerFirst();
    r.cc.updateConversation(r.session, [](model::Conversation &c) { c.member = false; });
    REQUIRE(r.wait([&] { return r.links->all().empty(); }));
}

TEST("links: a restart takes up where it was, never posting an answer twice") {
    Rig r;
    r.linkAndStart();
    r.agents->progress(Phase::Reading, "Reading files\xE2\x80\xA6");
    r.pumpFor(50);
    r.restart(); // mid-turn
    REQUIRE(r.link() != nullptr);
    CHECK(r.link()->running && r.link()->sent);
    CHECK(r.link()->branches.size() == 1);
    CHECK(r.agents->watches == 1); // the branch's turn followed again
    CHECK(
        r.agents->labels.back() == std::make_pair(std::string("b1"), r.link()->branches[0].label)
    );
    r.agents->answer("Done meanwhile.", "a1");
    REQUIRE(r.wait([&] { return !r.link()->running; }));
    CHECK(r.owBe->posts.size() == 2);
    CHECK(contains(r.owBe->posts.back().text, "Done meanwhile."));

    // A turn handed over that never ran (the last answer told again): asked
    // again, late, nothing posted twice.
    r.owBe->add(r.owBe->newer(), r.mira, "\xF0\x9F\xA4\x96 and the exporter?");
    r.owBe->report(r.chan, r.link()->lastSeen);
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 2; }));
    r.restart();
    REQUIRE(r.agents->turn != nullptr);
    r.agents->answer("Done meanwhile.", "a1");
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 3; }));
    CHECK(r.owBe->posts.size() == 4); // two status lines, one answer…
    CHECK(contains(r.agents->asks.back().prompt, "Mira (to you): and the exporter?"));
}

TEST("links: a question asked while msga was closed is answered late, and says so") {
    Rig r;
    r.linkAndStart();
    r.answerFirst();
    r.links.reset();
    r.agents->turn = nullptr;
    // Asked while closed: an older ts than this start.
    r.owBe->add(r.owBe->newer(), r.mira, "\xF0\x9F\xA4\x96 still there?");
    r.open();
    r.owBe->report(r.chan, r.link()->lastSeen);
    REQUIRE(r.wait([&] { return r.agents->asks.size() == 2; }));
    CHECK(contains(r.owBe->posts.back().text, "Picked up late"));
}
