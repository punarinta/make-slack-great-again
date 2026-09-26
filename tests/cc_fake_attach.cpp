// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Stand-in for `claude attach <short>` in test_claude_code: a terminal UI that
// draws like Claude Code 2.1.282's — the prompt box between two rules, the
// cursor in it — takes bracketed pastes and LF as typing, and on Enter writes
// the prompt and an answer ("echo <text>") into the session's transcript.
//
// Driven by files in $CLAUDE_CONFIG_DIR:
//   attach-mode   "question": a permission question has the keyboard instead
//                 of the prompt box (keys then go to question-keys.log): ↑/↓
//                 move "❯" over its options, Enter picks one — written to
//                 answered.log — and the question goes (attach-mode emptied)
//   typed.log     every prompt submitted, one per line ("\n" as "\\n")
// A spinner redraws all the time, as Claude Code's does mid-turn.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {

std::string home;
std::string transcript;
std::string input;
int         spin = 0;
int         pick = 1; // the question's option "❯" is on

const char *const kOptions[] = {"Yes", "Yes, and don't ask again for rm commands", "No"};

std::string readFile(const std::string &path) {
    std::ifstream     f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool question() {
    return readFile(home + "/attach-mode").rfind("question", 0) == 0;
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
    for (const char c : input) {
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

std::string jsonString(const std::string &v) {
    std::string o = "\"";
    for (const char c : v) {
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
    std::ofstream(home + "/typed.log", std::ios::app) << logged << "\n";
    static int    n  = 0;
    const auto    id = std::to_string(::getpid()) + "-" + std::to_string(++n);
    std::ofstream t(transcript, std::ios::app);
    t << R"({"type":"user","uuid":"a-)" << id
      << R"(-1","origin":{"kind":"human"},"message":{"content":)" << jsonString(input) << "}}\n";
    t << R"({"type":"assistant","uuid":"a-)" << id
      << R"(-2","message":{"content":[{"type":"text","text":)" << jsonString("echo " + input)
      << "}]}}\n";
    t << R"({"type":"system","subtype":"turn_duration","uuid":"a-)" << id << "-3\"}\n";
    input.clear();
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || !std::getenv("CLAUDE_CONFIG_DIR"))
        return 2;
    home                    = std::getenv("CLAUDE_CONFIG_DIR");
    const std::string state = readFile(home + "/jobs/" + argv[1] + "/state.json");
    const auto        at    = state.find("\"linkScanPath\":\"");
    if (at == std::string::npos) {
        out("no such session\r\n");
        return 1;
    }
    const auto from = at + 16;
    transcript      = state.substr(from, state.find('"', from) - from);

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
            std::ofstream(home + "/question-keys.log", std::ios::app) << pending;
            for (size_t at = 0; at < pending.size(); ++at) {
                if (pending.compare(at, 3, "\x1b[A") == 0)
                    pick = pick > 1 ? pick - 1 : pick;
                else if (pending.compare(at, 3, "\x1b[B") == 0)
                    pick = pick < 3 ? pick + 1 : pick;
                else if (pending[at] == '\r') {
                    std::ofstream(home + "/answered.log", std::ios::app) << pick << "\n";
                    std::ofstream(home + "/attach-mode", std::ios::trunc);
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
