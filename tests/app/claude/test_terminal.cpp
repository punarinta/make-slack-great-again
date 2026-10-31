// The terminal side of typing into live sessions: the pseudo-terminal, the
// screen reader (VtScreen and what it finds on Claude Code's screen), and
// AttachInput / AttachAnswer against a fake `claude attach`.
//
// The fake `attach` is this test binary run again with MSGA_FAKE_ATTACH_RUN=
// <short id> (see fakeAttachMain): a terminal UI that draws like Claude Code
// 2.1.282's — the prompt box between two rules, the cursor in it — takes
// bracketed pastes and LF as typing, and logs each prompt submitted with Enter.
#include "app/claude/attach.h"
#include "app/claude/pty.h"
#include "app/claude/vt.h"
#include "base/file.h"
#include "base/process.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/plat.h"
#include "app/model/jobs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace claude;

// ── The fake `claude attach` ────────────────────────────────────────────────
//
// Driven by files in $CLAUDE_CONFIG_DIR:
//   attach-mode   "question": a permission question has the keyboard instead
//                 of the prompt box (keys then go to question-keys.log): ↑/↓
//                 move "❯" over its options, Enter picks one — written to
//                 answered.log — and the question goes (attach-mode emptied);
//                 "sticky": after Enter the box goes on showing the prompt
//                 (whatever Claude Code draws while it queues one);
//                 "hint": after Enter the box shows a queued-prompt hint
//   typed.log     every prompt submitted, one per line ("\n" as "\\n")
// When the job's state.json names its transcript (linkScanPath), Enter also
// writes the prompt and an answer ("echo <text>") there: what the backend
// tests read back.
// A spinner redraws all the time, as Claude Code's does mid-turn.
//
// POSIX only, as is everything here that runs it or another program in a
// pseudo-terminal: the fake is a termios program and the cases drive
// /bin/sh. The screen reader and the keystroke cases run everywhere.
#ifndef _WIN32

namespace fake_attach {

std::string home;
std::string transcript; // the job's (linkScanPath), "" = none
std::string input;
std::string shown; // what the box shows instead of the input ("sticky", "hint")
int         spin = 0;
int         pick = 1; // the question's option "❯" is on

const char *const kOptions[] = {"Yes", "Yes, and don't ask again for rm commands", "No"};

std::string readText(const std::string &path) {
    std::string s;
    file::readAll(path, &s);
    return s;
}

void appendText(const std::string &path, const std::string &s) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!::write(fd, s.data(), s.size());
        ::close(fd);
    }
}

bool mode(const char *name) {
    return readText(home + "/attach-mode").rfind(name, 0) == 0;
}

bool question() {
    return mode("question");
}

void out(const std::string &s) {
    (void)!::write(1, s.data(), s.size());
}

void draw() {
    std::string s = "\x1b[?25l\x1b[H\x1b[2J";
    s += "\x1b[1;1H Fake Claude Code";
    s += "\x1b[3;1H\xe2\x9d\xaf earlier prompt"; // ❯ earlier prompt
    s += "\x1b[5;1H\xe2\x97\x8f an answer";      // ● an answer
    static const char *frames[] = {"\xc2\xb7", "\xe2\x9c\xa2", "\xe2\x9c\xb3", "\xe2\x9c\xb6"};
    s += "\x1b[7;1H" + std::string(frames[spin % 4]) + " Working\xe2\x80\xa6 (" +
         std::to_string(spin) + "s)";
    std::string rule;
    for (int i = 0; i < 60; ++i)
        rule += "\xe2\x94\x80"; // ─
    if (question()) {
        s += "\x1b[10;1H" + rule;
        s += "\x1b[11;1H Bash command";
        s += "\x1b[12;1H   \xe2\x94\x82 rm -rf build"; // │ rm -rf build
        s += "\x1b[13;1H Do you want to proceed?";
        for (int i = 0; i < 3; ++i)
            s += "\x1b[" + std::to_string(14 + i) + ";1H " +
                 (i + 1 == pick ? "\xe2\x9d\xaf " : "  ") + std::to_string(i + 1) + ". " +
                 kOptions[i];
        s += "\x1b[18;1H Esc to cancel \xc2\xb7 Tab to amend";
        s += "\x1b[" + std::to_string(13 + pick) + ";2H\x1b[?25h";
        out(s);
        return;
    }
    s += "\x1b[10;1H" + rule.substr(0, 3 * 50) + " fake \xe2\x94\x80";
    int         row = 11, col = 3;
    std::string line;
    bool        first = true;
    auto        flush = [&] {
        s += "\x1b[" + std::to_string(row) + ";1H" + (first ? "\xe2\x9d\xaf " : "  ") + line;
        col   = 3 + static_cast<int>(line.size()); // ASCII in these tests
        first = false;
        ++row;
        line.clear();
    };
    for (const char c : input.empty() ? shown : input) {
        if (c == '\n')
            flush();
        else
            line += c;
    }
    flush();
    s += "\x1b[" + std::to_string(row) + ";1H" + rule;
    s += "\x1b[" + std::to_string(row + 1) + ";1H  \xe2\x8f\xb8 manual mode on";
    s += "\x1b[" + std::to_string(row - 1) + ";" + std::to_string(col) + "H\x1b[?25h";
    out(s);
}

std::string jsonString(const std::string &s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\')
            o += '\\', o += c;
        else if (c == '\n')
            o += "\\n";
        else if (c == '\t')
            o += "\\t";
        else
            o += c;
    }
    return o + "\"";
}

void submit() {
    std::string logged;
    for (const char c : input)
        logged += c == '\n' ? std::string("\\n") : std::string(1, c);
    appendText(home + "/typed.log", logged + "\n");
    if (!transcript.empty()) {
        static int        n  = 0;
        const std::string id = std::to_string(::getpid()) + "-" + std::to_string(++n);
        appendText(
            transcript,
            R"({"type":"user","uuid":"a-)" + id +
                R"(-1","origin":{"kind":"human"},"message":{"content":)" + jsonString(input) +
                "}}\n" + R"({"type":"assistant","uuid":"a-)" + id +
                R"(-2","message":{"content":[{"type":"text","text":)" +
                jsonString("echo " + input) + "}]}}\n" +
                R"({"type":"system","subtype":"turn_duration","uuid":"a-)" + id + "-3\"}\n"
        );
    }
    shown = mode("sticky") ? input : mode("hint") ? "Press up to edit queued messages" : "";
    input.clear();
}

int run(const std::string &shortId) {
    const char *h = std::getenv("CLAUDE_CONFIG_DIR");
    if (!h)
        return 2;
    home = h;
    if (!file::exists(home + "/jobs/" + shortId + "/state.json")) {
        out("no such session\r\n");
        return 1;
    }
    const std::string state = readText(home + "/jobs/" + shortId + "/state.json");
    if (const auto at = state.find("\"linkScanPath\":\""); at != std::string::npos)
        transcript = state.substr(at + 16, state.find('"', at + 16) - (at + 16));
    termios raw{};
    ::tcgetattr(0, &raw);
    ::cfmakeraw(&raw);
    ::tcsetattr(0, TCSANOW, &raw);
    out("Attaching\xe2\x80\xa6\r\n");
    ::usleep(200'000);
    out("\x1b[?1049h");
    draw();

    std::string pending; // bytes of an escape sequence or paste not complete yet
    bool        pasting = false;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        timeval tv{0, 100'000};
        if (::select(1, &fds, nullptr, nullptr, &tv) <= 0) {
            ++spin;
            draw(); // the spinner: output that never stops for long
            continue;
        }
        char          buf[4096];
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n <= 0)
            return 0;
        pending.append(buf, static_cast<size_t>(n));
        if (question()) {
            appendText(home + "/question-keys.log", pending);
            for (size_t at = 0; at < pending.size(); ++at) {
                if (pending.compare(at, 3, "\x1b[A") == 0)
                    pick = pick > 1 ? pick - 1 : pick;
                else if (pending.compare(at, 3, "\x1b[B") == 0)
                    pick = pick < 3 ? pick + 1 : pick;
                else if (pending[at] == '\r') {
                    appendText(home + "/answered.log", std::to_string(pick) + "\n");
                    file::overwrite(home + "/attach-mode", "");
                    pick = 1;
                }
            }
            pending.clear();
            draw();
            continue;
        }
        for (;;) {
            if (pasting) {
                const auto end = pending.find("\x1b[201~");
                if (end == std::string::npos)
                    break;
                input += pending.substr(0, end);
                pending.erase(0, end + 6);
                pasting = false;
                continue;
            }
            if (pending.empty())
                break;
            if (pending.rfind("\x1b[200~", 0) == 0) {
                pending.erase(0, 6);
                pasting = true;
                continue;
            }
            if (pending[0] == '\x1b' && pending.size() < 6)
                break; // maybe the start of a paste
            const char c = pending[0];
            pending.erase(0, 1);
            if (c == '\r')
                submit();
            else if (c == '\n')
                input += '\n';
            else if (c != '\x1b')
                input += c;
        }
        draw();
    }
}

// Before main: this binary run as the fake `attach` never gets to the tests.
const int hook = [] {
    if (const char *shortId = std::getenv("MSGA_FAKE_ATTACH_RUN"))
        ::_exit(run(shortId));
    return 0;
}();

} // namespace fake_attach
#endif

// The program that is the fake `attach` (this binary), for test_launcher too.
std::string fakeAttachProgram() {
    return base::executablePath();
}

namespace {

bool str_starts(const std::string &s, const char *p) {
    return s.rfind(p, 0) == 0;
}

#ifndef _WIN32

std::string tempDir() {
    return base::test::makeTempDir("claude_terminal_test_");
}

// A case's own App: the workers still under way (they post to it) are done
// before it goes.
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

bool waitFor(plat::App &app, const std::function<bool()> &pred, int ms = 5000) {
    const int64_t until = base::monotonicMs() + ms;
    while (!pred() && base::monotonicMs() < until)
        app.pump(10);
    return pred();
}

void pumpFor(plat::App &app, int ms) {
    const int64_t until = base::monotonicMs() + ms;
    while (base::monotonicMs() < until)
        app.pump(10);
}

std::string readText(const std::string &path) {
    std::string s;
    file::readAll(path, &s);
    return s;
}

void removeTree(const std::string &dir) {
    if (!dir.empty() && str_starts(dir, "/tmp/"))
        base::run("/bin/rm", {"-rf", dir}, {});
}
#endif

#ifndef _WIN32
// A throwaway Claude Code home with one background job (abcdef11), the
// variables the fake `attach` reads set for the processes started from here.
struct FakeAttachHome {
    std::string dir = tempDir();
    std::string oldConfig;

    FakeAttachHome() {
        oldConfig = base::env("CLAUDE_CONFIG_DIR");
        base::test::setEnv("CLAUDE_CONFIG_DIR", dir);
        base::test::setEnv("MSGA_FAKE_ATTACH_RUN", "abcdef11");
        file::writeAtomic(dir + "/jobs/abcdef11/state.json", R"({"state":"done"})");
        AttachInput::setAttachTimeoutMs(3000);
        AttachInput::setSubmitTimeoutMs(10'000);
    }
    ~FakeAttachHome() {
        base::test::unsetEnv("MSGA_FAKE_ATTACH_RUN");
        if (oldConfig.empty())
            base::test::unsetEnv("CLAUDE_CONFIG_DIR");
        else
            base::test::setEnv("CLAUDE_CONFIG_DIR", oldConfig);
        AttachInput::setAttachTimeoutMs(15'000);
        AttachInput::setSubmitTimeoutMs(10'000);
        removeTree(dir);
    }
    std::string text(const char *name) const { return readText(dir + "/" + name); }
    void        setMode(const char *mode) const { file::writeAtomic(dir + "/attach-mode", mode); }
};
#endif

std::string rule(int n, const std::string &title = {}) {
    std::string r;
    for (int i = 0; i < n; ++i)
        r += "─";
    return title.empty() ? r : r + " " + title + " ─";
}

// A frame drawn the way Claude Code 2.1.282 draws its idle screen: absolute
// rows, the prompt box near the bottom, the cursor parked in it.
std::string idleFrame(const std::string &input = {}, bool cursorShown = true) {
    std::string f = "\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l";
    f += "\x1b[2;1H\x1b[38;5;174m ▐▛███▛█\x1b[39m   Claude Code v2.1.282";
    f += "\x1b[6;1H❯ Launch ONE subagent\x1b[8;1H● STARTED";
    f += "\x1b[10;1H✻ Waiting for 1 background agent to finish";
    f += "\x1b[44;1H" + rule(100, "fix the build");
    f += "\x1b[45;1H❯ " + input;
    f += "\x1b[46;1H" + rule(120);
    f += "\x1b[47;1H  ⏸ manual mode on · ← 1 agent · ↓ to manage";
    f += "\x1b[45;" + std::to_string(3 + input.size()) + "H";
    if (cursorShown)
        f += "\x1b[?25h";
    return f;
}

bool endsWith(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

} // namespace

// ── The screen ──────────────────────────────────────────────────────────────

TEST("terminal: the screen shows when Claude Code's prompt takes typing") {
    {
        VtScreen s(50, 160);
        s.feed(idleFrame());
        CHECK(str_starts(s.row(43), "────"));
        CHECK(endsWith(s.row(43), "fix the build ─"));
        CHECK_STR(s.row(44), "❯");
        REQUIRE(findPromptBox(s));
        CHECK(findPromptBox(s)->empty);
        CHECK(readyForInput(s));
    }
    {
        // Mid-draw (cursor hidden), or a draft someone left in the box: no.
        VtScreen hidden(50, 160);
        hidden.feed(idleFrame({}, false));
        CHECK_FALSE(readyForInput(hidden));
        VtScreen draft(50, 160);
        draft.feed(idleFrame("half a thought"));
        CHECK_FALSE(readyForInput(draft));
        REQUIRE(findPromptBox(draft));
        CHECK(findPromptBox(draft)->lines == std::vector<std::string>{"half a thought"});
    }
    {
        // A permission question (here a subagent's) takes the box's place.
        VtScreen s(50, 160);
        s.feed(
            "\x1b[H\x1b[2J\x1b[6;1H● STARTED\x1b[16;1H" + rule(120) +
            "\x1b[17;1H Bash command · from the general-purpose agent"
            "\x1b[22;1H Do you want to proceed?\x1b[23;1H ❯ 1. Yes\x1b[24;1H   2. No"
            "\x1b[27;1H Esc to cancel · Tab to amend\x1b[23;2H\x1b[?25h"
        );
        CHECK_FALSE(findPromptBox(s));
        CHECK_FALSE(readyForInput(s));
        const auto q = findPermissionQuestion(s);
        REQUIRE(q);
        REQUIRE(q->options.size() == 2);
        CHECK_STR(q->options[0].label, "Yes");
        CHECK_STR(q->options[1].label, "No");
        CHECK(q->selected == 1);
        CHECK_STR(q->text, "Bash command · from the general-purpose agent Do you want to proceed?");
    }
    {
        // As Claude Code 2.1.283 drew one for a background session (seen through
        // `claude attach`, 2026-09-26): the command framed and wrapped, a note,
        // the auto-deny countdown — and "❯" moved down to the second option.
        VtScreen s(50, 120);
        s.feed(
            "\x1b[H\x1b[2J\x1b[25;1H● Running 1 shell command…\x1b[31;1H" + rule(120) +
            "\x1b[32;1H Bash command"
            "\x1b[34;1H   │ rm -rf /home/robin/.claude/jobs/b7b46dde/tmp/ccfg"
            "\x1b[35;1H   │ /home/robin/src/msga/build-tests; git -C"
            "\x1b[36;1H   │ /home/robin/src/msga status --short"
            "\x1b[37;1H   Run shell command"
            "\x1b[39;1H │ Dangerous rm operation on working directory or its ancestor:"
            "\x1b[41;1H ⚠ Claude Code will automatically deny this request in 0:14"
            "\x1b[44;1H Do you want to proceed?\x1b[45;1H   1. Yes\x1b[46;1H ❯ 2. No"
            "\x1b[48;1H Esc to cancel · Tab to amend\x1b[46;2H"
        );
        const auto q = findPermissionQuestion(s);
        REQUIRE(q);
        REQUIRE(q->options.size() == 2);
        CHECK(q->selected == 2);
        CHECK(
            str_starts(q->text, "Bash command │ rm -rf /home/robin/.claude/jobs/b7b46dde/tmp/ccfg")
        );
    }
    {
        // As 2.1.314 draws one (2026-10-30): a rule with the session's name
        // under the question, and a dashed one around the command.
        VtScreen s(50, 200);
        s.feed(
            "\x1b[H\x1b[2J\x1b[20;1H● Running 1 shell command…\x1b[22;1H" + rule(200) +
            "\x1b[23;1H Bash command\x1b[24;1H   Stop test servers"
            "\x1b[25;1H ╌╌╌╌╌╌\x1b[26;1H │ pkill -f \"next dev\"; rm -f server/node_modules"
            "\x1b[27;1H ╌╌╌╌╌╌\x1b[28;1H │ server/node_modules resolves through a symlink"
            "\x1b[30;1H Do you want to proceed?\x1b[31;1H ❯ 1. Yes"
            "\x1b[32;1H   2. Yes, and always allow access to server/node_modules"
            "\x1b[33;1H   3. No\x1b[35;1H Esc to cancel · Tab to amend\x1b[36;1H" +
            rule(160, "lingolette b2c signup conversion") + "\x1b[31;2H"
        );
        const auto q = findPermissionQuestion(s);
        REQUIRE(q);
        REQUIRE(q->options.size() == 3);
        CHECK(q->selected == 1);
        CHECK_STR(q->options[2].label, "No");
        CHECK(str_starts(q->text, "Bash command Stop test servers"));
    }
    {
        // The prompt box, or a numbered list in an answer: no question.
        VtScreen box(50, 160);
        box.feed(idleFrame());
        CHECK_FALSE(findPermissionQuestion(box));
        VtScreen list(50, 160);
        list.feed(
            "\x1b[H\x1b[2J\x1b[3;1H" + rule(120) + "\x1b[4;1H Steps:\x1b[5;1H 1. build" +
            "\x1b[6;1H 2. test"
        );
        CHECK_FALSE(findPermissionQuestion(list)); // no "❯" on any of them
    }
    {
        // A panel (/cost) has the keyboard: no box either.
        VtScreen s(50, 160);
        s.feed("\x1b[H\x1b[2J\x1b[3;1H  Current session\x1b[45;1H   Esc to cancel\x1b[3;31H");
        CHECK_FALSE(readyForInput(s));
    }
    {
        // Relative moves, erasing, a wide character, UTF-8 split across reads.
        VtScreen          s(5, 20);
        const std::string beer = "🍺";
        s.feed("ab\x1b[3Gc" + beer.substr(0, 2));
        s.feed(beer.substr(2) + "d\x1b[2;4Hx\x1b[1A\x1b[2Cy");
        CHECK_STR(s.row(0), "abc🍺dy");
        CHECK_STR(s.row(1), "   x");
        s.feed("\x1b[1;3H\x1b[K");
        CHECK_STR(s.row(0), "ab");
        CHECK(s.cursorRow() == 0);
        CHECK(s.cursorCol() == 2);
        // Writing past the last row scrolls up.
        s.feed("\x1b[5;1Hlast\r\nnext");
        CHECK_STR(s.row(3), "last");
        CHECK_STR(s.row(4), "next");
    }
    {
        // A combining mark takes no cell; ESC c starts over.
        VtScreen s(3, 10);
        s.feed("e\xcc\x81x"); // e + COMBINING ACUTE ACCENT, x
        CHECK_STR(s.row(0), "ex");
        CHECK(s.cursorCol() == 2);
        s.feed(
            "\x1b[?25l\x1b"
            "c"
        );
        CHECK_STR(s.row(0), "");
        CHECK(s.cursorVisible());
    }
}

TEST("terminal: the question on screen is matched to the job's needs") {
    const auto question = [](const std::string &text) {
        PermissionQuestion q;
        q.text    = text;
        q.options = {{1, "Yes"}, {2, "No"}};
        return q;
    };
    // A command, wrapped and framed on screen.
    const auto rm =
        question("Bash command │ rm -rf /home/robin/src/msga/ │ build Do you want to proceed?");
    CHECK(questionIsFor("approve Bash: rm -rf /home/robin/src/msga/build", rm));
    CHECK_FALSE(questionIsFor("approve Bash: git push", rm));
    // A label alone, which the question words its own way (EnterWorktree with
    // a path, Claude Code 2.1.283): "approve Entering worktree" asks this.
    const auto worktree = question(
        "Enter the worktree at \"/home/robin/src/citycity/monorepo\"? This moves the session's "
        "working directory and write access there, and loads project configuration (CLAUDE.md, "
        "settings) from that location."
    );
    CHECK(questionIsFor("approve Entering worktree", worktree));
    CHECK(questionIsFor("approve Web fetch", question("Web fetch https://example.com")));
    CHECK_FALSE(questionIsFor("approve Entering worktree", rm));
    CHECK_FALSE(questionIsFor("approve ", worktree));
    // A file shown relative to the folder: matched by its name.
#ifdef _WIN32
    const char *edit = "approve Edit: C:\\src\\msga\\src\\app\\main.cpp";
#else
    const char *edit = "approve Edit: /home/robin/src/msga/src/app/main.cpp";
#endif
    CHECK(questionIsFor(
        edit, question("Edit file src/app/main.cpp Do you want to make this edit to main.cpp?")
    ));
}

TEST("terminal: a message is typed line by line, never as one big paste") {
    const auto paste = [](const std::string &t) { return "\x1b[200~" + t + "\x1b[201~"; };
    using Keys       = std::vector<std::string>;
    CHECK(
        (AttachInput::keystrokes("first line\nsecond \"quoted\"") ==
         Keys{paste("first line"), "\n", paste("second \"quoted\"")})
    );
    // Blank lines stay; a trailing newline and CRLFs don't make extra ones.
    CHECK((AttachInput::keystrokes("a\r\n\r\nb\n") == Keys{paste("a"), "\n", "\n", paste("b")}));
    // "!" would run a shell command, "/" a slash command: a space keeps them text.
    CHECK((AttachInput::keystrokes("!rm -rf /") == Keys{paste(" !rm -rf /")}));
    CHECK((AttachInput::keystrokes("/cost") == Keys{paste(" /cost")}));
    // But for a command that runs and hands the prompt box back: typed as one.
    CHECK((AttachInput::keystrokes("/compact") == Keys{paste("/compact")}));
    CHECK((AttachInput::keystrokes(" /compact keep it\n") == Keys{paste("/compact keep it")}));
    CHECK(
        (AttachInput::keystrokes("/compact\nmore") == Keys{paste(" /compact"), "\n", paste("more")})
    );
    CHECK((AttachInput::keystrokes("/compacting") == Keys{paste(" /compacting")}));
    CHECK(AttachInput::isLiveCommand("/compact"));
    CHECK_FALSE(AttachInput::isLiveCommand("/cost"));
    CHECK_FALSE(AttachInput::isLiveCommand("compact"));
    // Control characters would be keys (Esc, Ctrl+C): dropped.
    CHECK((AttachInput::keystrokes("x\x1b[31my\x03") == Keys{paste("x[31my")}));
    // A long line goes in pieces; a character is never cut in two.
    const std::string long1000 = std::string(399, 'a') + "🍺" + std::string(600, 'b');
    const auto        keys     = AttachInput::keystrokes(long1000);
    REQUIRE(keys.size() == 3);
    std::string joined;
    for (const auto &k : keys) {
        CHECK(str_starts(k, "\x1b[200~"));
        CHECK(endsWith(k, "\x1b[201~"));
        joined += k.substr(6, k.size() - 12);
    }
    CHECK(joined == long1000);
    CHECK(keys[0].size() - 12 == 399); // the beer didn't fit: the next piece starts with it
    CHECK(AttachInput::keystrokes(" \n\n").empty());
}

// ── The pseudo-terminal ─────────────────────────────────────────────────────

#ifndef _WIN32

TEST("terminal: a program in a pseudo-terminal sees one, takes typing and ends") {
    auto app = headlessApp();
    REQUIRE(app);
    Pty         pty(*app);
    std::string output;
    bool        finished = false;
    pty.onOutput         = [&](std::string_view b) { output.append(b); };
    pty.onFinished       = [&] { finished = true; };
    REQUIRE(pty.start(
        "/bin/sh",
        {"-c",
         "stty size; echo \"term=$TERM tty=$(test -t 0 && echo yes)\"; read x; echo \"got $x\""},
        "/tmp",
        24,
        100
    ));
    CHECK(pty.isRunning());
    REQUIRE(waitFor(*app, [&] { return output.find("tty=yes") != std::string::npos; }));
    CHECK(output.find("24 100") != std::string::npos);
    CHECK(output.find("term=xterm-256color") != std::string::npos);
    pty.write("hello\r");
    REQUIRE(waitFor(*app, [&] { return finished; }));
    CHECK(output.find("got hello") != std::string::npos);
    CHECK_FALSE(pty.isRunning());
}

TEST("terminal: terminate ends a program, and one that ignores SIGTERM too") {
    auto app = headlessApp();
    REQUIRE(app);
    {
        Pty  pty(*app);
        bool finished  = false;
        pty.onFinished = [&] { finished = true; };
        REQUIRE(pty.start("/bin/sh", {"-c", "trap '' TERM; echo ready; sleep 30"}, {}, 24, 80));
        std::string output;
        pty.onOutput = [&](std::string_view b) { output.append(b); };
        REQUIRE(waitFor(*app, [&] { return output.find("ready") != std::string::npos; }));
        const int64_t t = base::monotonicMs();
        pty.terminate();
        REQUIRE(waitFor(*app, [&] { return finished; }, 6000));
        CHECK(base::monotonicMs() - t >= 1500); // SIGKILL, 2 s after the SIGTERM
    }
    {
        // A program that isn't there: the terminal shows nothing and closes.
        Pty  pty(*app);
        bool finished  = false;
        pty.onFinished = [&] { finished = true; };
        REQUIRE(pty.start("/nonexistent/attach", {}, {}, 24, 80));
        CHECK(waitFor(*app, [&] { return finished; }));
    }
    {
        // Destroyed while it runs: the program goes, no callback comes after.
        bool called = false;
        {
            Pty pty(*app);
            pty.onOutput   = [&](std::string_view) { called = true; };
            pty.onFinished = [&] { called = true; };
            REQUIRE(pty.start("/bin/sh", {"-c", "sleep 0.2; echo late; sleep 30"}, {}, 24, 80));
        }
        pumpFor(*app, 500);
        CHECK_FALSE(called);
    }
}

// ── Typing into a session ───────────────────────────────────────────────────

namespace {

struct InputRun {
    std::optional<AttachInput::Outcome> outcome;
    std::string                         detail;
};

InputRun type(plat::App &app, const std::string &text, int waitMs = 15'000) {
    auto result = std::make_shared<InputRun>();
    auto handle = AttachInput::send(
        app,
        fakeAttachProgram(),
        {},
        "/tmp",
        text,
        [result](AttachInput::Outcome o, std::string d) {
            result->outcome = o;
            result->detail  = std::move(d);
        }
    );
    std::weak_ptr<AttachInput> weak = handle;
    handle.reset(); // it lives on its own until over
    waitFor(app, [&] { return result->outcome.has_value(); }, waitMs);
    // …and lets go of itself once `attach` has exited.
    waitFor(app, [&] { return weak.expired(); }, 8000);
    if (!weak.expired())
        std::fprintf(stderr, "    the AttachInput outlived its attach\n");
    return *result;
}

} // namespace

TEST("terminal: a message is typed into the prompt box and sent with Enter") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeAttachHome home;
    auto           r = type(*app, "hello there");
    REQUIRE(r.outcome);
    CHECK(*r.outcome == AttachInput::Outcome::Sent);
    CHECK_STR(home.text("typed.log"), "hello there\n");
    // Several lines, one starting with "!": typed as they are, as plain text.
    r = type(*app, "!make it\nwork");
    REQUIRE(r.outcome);
    CHECK(*r.outcome == AttachInput::Outcome::Sent);
    CHECK_STR(home.text("typed.log"), "hello there\n !make it\\nwork\n");
    // After Enter the box shows a queued-prompt hint: the message was taken.
    home.setMode("hint");
    r = type(*app, "queued");
    REQUIRE(r.outcome);
    CHECK(*r.outcome == AttachInput::Outcome::Sent);
}

TEST("terminal: nothing is typed while a question has the keyboard") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeAttachHome home;
    home.setMode("question");
    AttachInput::setAttachTimeoutMs(1500);
    const auto r = type(*app, "third");
    REQUIRE(r.outcome);
    CHECK(*r.outcome == AttachInput::Outcome::NotReady);
    CHECK_STR(r.detail, "the prompt box never showed up ready");
    CHECK_FALSE(file::exists(home.dir + "/question-keys.log"));
    CHECK_FALSE(file::exists(home.dir + "/typed.log"));
}

TEST("terminal: a box that keeps the message after Enter is unconfirmed, not failed") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeAttachHome home;
    home.setMode("sticky");
    AttachInput::setSubmitTimeoutMs(1200);
    const auto r = type(*app, "mid-turn");
    REQUIRE(r.outcome);
    CHECK(*r.outcome == AttachInput::Outcome::Unconfirmed);
    CHECK_STR(home.text("typed.log"), "mid-turn\n");
}

TEST("terminal: no such session, nothing to type, cancelled") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeAttachHome home;
    base::test::setEnv("MSGA_FAKE_ATTACH_RUN", "00000000"); // not a job
    auto r = type(*app, "hello");
    REQUIRE(r.outcome);
    CHECK(*r.outcome == AttachInput::Outcome::NotReady);
    CHECK_STR(r.detail, "claude attach exited");
    base::test::setEnv("MSGA_FAKE_ATTACH_RUN", "abcdef11");

    // Nothing but blanks and control characters: nothing is started.
    std::optional<AttachInput::Outcome> outcome;
    AttachInput::send(
        *app, "/nonexistent", {}, {}, " \n\x1b", [&](AttachInput::Outcome o, std::string) {
            outcome = o;
        }
    );
    CHECK(outcome == AttachInput::Outcome::NotReady);

    // Cancelled while attaching: NotReady at once, and nothing typed later.
    outcome.reset();
    std::string detail;
    auto        handle = AttachInput::send(
        *app, fakeAttachProgram(), {}, {}, "never", [&](AttachInput::Outcome o, std::string d) {
            outcome = o;
            detail  = std::move(d);
        }
    );
    pumpFor(*app, 100);
    CHECK(handle->cancel());
    CHECK(outcome == AttachInput::Outcome::NotReady);
    CHECK_STR(detail, "cancelled");
    pumpFor(*app, 1500);
    CHECK_FALSE(file::exists(home.dir + "/typed.log"));
}

// ── Answering a permission question ─────────────────────────────────────────

TEST("terminal: a permission question is read, then answered by its option") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeAttachHome home;
    home.setMode("question");
    const auto isRm = [](const PermissionQuestion &q) {
        return questionIsFor("approve Bash: rm -rf build", q);
    };
    struct Result {
        std::optional<AttachAnswer::Outcome> outcome;
        std::optional<PermissionQuestion>    question;
        std::string                          detail;
    };
    auto run = [&](std::function<std::shared_ptr<AttachAnswer>(AttachAnswer::Result)> start) {
        auto                        r = std::make_shared<Result>();
        std::weak_ptr<AttachAnswer> weak =
            start([r](AttachAnswer::Outcome o, std::optional<PermissionQuestion> q, std::string d) {
                r->outcome  = o;
                r->question = std::move(q);
                r->detail   = std::move(d);
            });
        waitFor(*app, [&] { return r->outcome.has_value(); }, 15'000);
        waitFor(*app, [&] { return weak.expired(); }, 8000);
        return *r;
    };

    auto read = run([&](AttachAnswer::Result done) {
        return AttachAnswer::choose(
            *app, fakeAttachProgram(), {}, {}, isRm, 0, {}, std::move(done)
        );
    });
    REQUIRE(read.outcome);
    CHECK(*read.outcome == AttachAnswer::Outcome::Done);
    REQUIRE(read.question);
    REQUIRE(read.question->options.size() == 3);
    CHECK_STR(read.question->options[1].label, "Yes, and don't ask again for rm commands");
    CHECK(read.question->selected == 1);
    CHECK_STR(read.question->text, "Bash command │ rm -rf build Do you want to proceed?");
    CHECK_FALSE(file::exists(home.dir + "/question-keys.log")); // reading presses nothing

    // An option whose label is another now: nothing is pressed.
    auto other = run([&](AttachAnswer::Result done) {
        return AttachAnswer::choose(
            *app, fakeAttachProgram(), {}, {}, isRm, 2, "Yes, always", std::move(done)
        );
    });
    REQUIRE(other.outcome);
    CHECK(*other.outcome == AttachAnswer::Outcome::NotReady);
    CHECK_FALSE(file::exists(home.dir + "/question-keys.log"));

    // Another session's question: not this one.
    auto notThis = run([&](AttachAnswer::Result done) {
        AttachInput::setAttachTimeoutMs(1500);
        return AttachAnswer::choose(
            *app,
            fakeAttachProgram(),
            {},
            {},
            [](const PermissionQuestion &q) { return questionIsFor("approve Bash: git push", q); },
            0,
            {},
            std::move(done)
        );
    });
    REQUIRE(notThis.outcome);
    CHECK(*notThis.outcome == AttachAnswer::Outcome::NotReady);
    AttachInput::setAttachTimeoutMs(3000);

    // The third: "❯" moved down twice, Enter once it's seen there.
    auto chose = run([&](AttachAnswer::Result done) {
        return AttachAnswer::choose(
            *app, fakeAttachProgram(), {}, {}, isRm, 3, "No", std::move(done)
        );
    });
    REQUIRE(chose.outcome);
    CHECK(*chose.outcome == AttachAnswer::Outcome::Done);
    CHECK_STR(home.text("answered.log"), "3\n");
    CHECK_STR(home.text("question-keys.log"), "\x1b[B\x1b[B\r");
}

// Against the real CLI, by hand, after a Claude Code upgrade: point it at a
// live background session of a throwaway CLAUDE_CONFIG_DIR —
//   MSGA_CC_LIVE_SHORT=<short> MSGA_CC_LIVE_TEXT="What is 2+2?"
//   [MSGA_CC_LIVE_EXPECT=NotReady] claude_tests "terminal: typing into a real"
// (all one command line)
// (claude on PATH). Sent means the prompt box took it; the transcript shows
// whether Claude got it as typed. Without MSGA_CC_LIVE_SHORT it does nothing.
TEST("terminal: typing into a real background session (live, by hand)") {
    const std::string shortId = base::env("MSGA_CC_LIVE_SHORT");
    if (shortId.empty())
        return;
    const std::string claude = base::findExecutable("claude");
    if (claude.empty()) {
        std::fprintf(stderr, "    claude on PATH needed\n");
        return;
    }
    std::string text = base::env("MSGA_CC_LIVE_TEXT");
    if (text.empty())
        text = "Reply with only the word PONG.";
    std::string expect = base::env("MSGA_CC_LIVE_EXPECT");
    if (expect.empty())
        expect = "Sent";
    auto app = headlessApp();
    REQUIRE(app);
    std::optional<AttachInput::Outcome> outcome;
    std::string                         detail;
    char                                cwd[4096] = {};
    AttachInput::send(
        *app,
        claude,
        {"attach", shortId},
        ::getcwd(cwd, sizeof cwd) ? cwd : "/",
        text,
        [&](AttachInput::Outcome o, std::string d) {
            outcome = o;
            detail  = std::move(d);
        }
    );
    REQUIRE(waitFor(*app, [&] { return outcome.has_value(); }, 60'000));
    std::fprintf(stderr, "    %s\n", detail.c_str());
    const char *names[] = {"Sent", "NotReady", "Failed", "Unconfirmed"};
    CHECK_STR(names[int(*outcome)], expect);
}

#endif // !_WIN32
