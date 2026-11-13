#include "app/claude/progress.h"

#include "base/crypto.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"

#include <algorithm>
#include <cstdio>

namespace claude {

namespace {

// A report's file: msga-status.json, or msga-status-<name>.json for a named one.
constexpr std::string_view kReportPrefix = "msga-status";
// A job name is a label, not a log line.
constexpr size_t           kMaxName      = 60;
// A report's line is a status, not a log: longer is cut.
constexpr size_t           kMaxReport    = 300;

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
bool isAlnum(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (unsigned char)c >= 0x80;
}

// A decimal number at s[i] — digits, "," thousands groups ("9,846"), one "."
// fraction — read into *value; returns the index after it, i when none.
size_t readNumber(std::string_view s, size_t i, double *value, bool allowFraction) {
    size_t j      = i;
    double v      = 0;
    bool   digits = false;
    while (j < s.size()) {
        if (isDigit(s[j])) {
            v      = v * 10 + (s[j] - '0');
            digits = true;
            ++j;
        } else if (
            s[j] == ',' && digits && j + 3 < s.size() && isDigit(s[j + 1]) && isDigit(s[j + 2]) &&
            isDigit(s[j + 3]) && (j + 4 >= s.size() || !isDigit(s[j + 4]))
        ) {
            ++j; // a thousands separator
        } else {
            break;
        }
    }
    if (!digits)
        return i;
    if (allowFraction && j + 1 < s.size() && s[j] == '.' && isDigit(s[j + 1])) {
        double scale = 0.1;
        for (++j; j < s.size() && isDigit(s[j]); ++j, scale /= 10)
            v += (s[j] - '0') * scale;
    }
    *value = v;
    return j;
}

std::string usage(std::string_view exe) {
    return str::concat(
        {"Usage: ",
         exe,
         " --claude-status \"<one short line>\" [--progress <p>] [--name <job>]\n"
         "  Shows how a long job goes in msga's chat header.\n"
         "  <p>: done/total (9846/10000), a percentage (98.5%) or a fraction (0.985).\n"
         "  <job>: a short name, so jobs running side by side each keep their own report.\n"
         "  Run it from inside a Claude Code background session (CLAUDE_JOB_DIR set).\n"}
    );
}

} // namespace

std::string reportPath(std::string_view jobDir, std::string_view name) {
    if (name.empty())
        return str::concat({jobDir, "/", kReportPrefix, ".json"});
    // The file's name: the job's, safe on any file system (the real one is inside).
    std::string safe;
    for (const char c : name)
        safe.push_back(isAlnum(c) && (unsigned char)c < 0x80 ? c : '_');
    return str::concat(
        {jobDir,
         "/",
         kReportPrefix,
         "-",
         safe,
         "-",
         str::number(int64_t(crypto::fnv1a(name) % 100000)),
         ".json"}
    );
}

std::string reportJson(const Report &r) {
    json::Writer w;
    w.beginObject().key("text").value(r.text);
    if (!r.name.empty())
        w.key("name").value(r.name);
    if (!r.ratio.empty())
        w.key("ratio").value(r.ratio);
    if (r.progress >= 0)
        w.key("progress").value(double(r.progress));
    w.key("at").value(r.atMs).endObject();
    return w.take();
}

namespace {

bool readReportFile(const std::string &path, Report *out) {
    std::string bytes;
    if (!file::readRange(path, 0, 4096, &bytes))
        return false;
    json::Document doc;
    if (!doc.parse(std::move(bytes), nullptr) || !doc.root().isObject())
        return false;
    const json::Value o = doc.root();
    Report            r;
    r.text         = std::string(str::trimSpace(o["text"].str()));
    r.name         = std::string(str::trimSpace(o["name"].str()));
    r.ratio        = std::string(o["ratio"].str());
    r.atMs         = o["at"].integer();
    const double p = o["progress"].number(-1);
    r.progress     = p >= 0 && p <= 1 ? float(p) : -1.f;
    if (r.atMs <= 0 || (r.text.empty() && r.progress < 0))
        return false;
    *out = std::move(r);
    return true;
}

} // namespace

bool readReport(std::string_view jobDir, Report *out) {
    return !jobDir.empty() && readReportFile(reportPath(jobDir), out);
}

std::vector<Report> readReports(std::string_view jobDir) {
    std::vector<Report>         out;
    std::vector<file::DirEntry> entries;
    if (jobDir.empty() || !file::listDir(jobDir, &entries))
        return out;
    for (const file::DirEntry &e : entries) {
        Report r;
        if (str::startsWith(e.name, kReportPrefix) && str::endsWith(e.name, ".json") &&
            readReportFile(str::concat({jobDir, "/", e.name}), &r))
            out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const Report &a, const Report &b) {
        return a.name < b.name;
    });
    return out;
}

std::string jobName(std::string_view command) {
    // Words, quotes kept off; leading VAR=value settings skipped.
    std::vector<std::string_view> words;
    for (size_t i = 0; i < command.size() && words.size() < 8;) {
        while (i < command.size() && (command[i] == ' ' || command[i] == '\t'))
            ++i;
        size_t j = i;
        while (j < command.size() && command[j] != ' ' && command[j] != '\t' && command[j] != '|' &&
               command[j] != ';' && command[j] != '&' && command[j] != '>' && command[j] != '<')
            ++j;
        if (j == i)
            break; // an operator: the first command is all there is to name
        std::string_view w = command.substr(i, j - i);
        while (!w.empty() && (w.front() == '"' || w.front() == '\''))
            w.remove_prefix(1);
        while (!w.empty() && (w.back() == '"' || w.back() == '\''))
            w.remove_suffix(1);
        if (!(words.empty() && w.find('=') != std::string_view::npos))
            words.push_back(w);
        i = j;
    }
    // "cd dir && make": the command after the cd.
    if (words.size() >= 2 && words[0] == "cd") {
        const size_t amp = command.find("&&");
        if (amp != std::string_view::npos)
            return jobName(command.substr(amp + 2));
    }
    if (words.empty())
        return {};
    std::string_view first = file::baseName(words[0]);
    if (const size_t dot = first.rfind('.'); dot != std::string_view::npos && dot > 0)
        first = first.substr(0, dot);
    static constexpr std::string_view kRunners[] = {
        "npm",
        "npx",
        "pnpm",
        "yarn",
        "bun",
        "cargo",
        "make",
        "cmake",
        "go",
        "python",
        "python3",
        "node",
        "bash",
        "sh",
        "uv"
    };
    std::string name(first);
    if (std::find(std::begin(kRunners), std::end(kRunners), first) != std::end(kRunners))
        for (size_t k = 1; k < words.size(); ++k) {
            // The next word that isn't a flag's value or a path-like option.
            const std::string_view w = words[k];
            if (w.empty() || (w[0] == '-' && w.size() > 2 && w[1] != '-'))
                continue; // -j8, -C: a short option
            if (w[0] == '-' && (w == "-C" || w == "-j"))
                continue;
            std::string_view shown = w;
            if (w.find('/') != std::string_view::npos && w[0] != '-') {
                shown = file::baseName(w);
                if (const size_t dot = shown.rfind('.'); dot != std::string_view::npos && dot > 0)
                    shown = shown.substr(0, dot);
                // "bash x.sh", "python3 run.py": the script names the job.
                if (first == "bash" || first == "sh" || str::startsWith(first, "python") ||
                    first == "node" || first == "uv")
                    return utf8::ellipsize(shown, 40);
            }
            name = str::concat({first, " ", shown});
            break;
        }
    return utf8::ellipsize(name, 40);
}

float parseProgress(std::string_view s, std::string *ratio) {
    s              = str::trimSpace(s);
    double       a = 0, b = 0;
    const size_t n = readNumber(s, 0, &a, true);
    if (n == 0)
        return -1;
    std::string_view rest = str::trimSpace(s.substr(n));
    if (rest.empty())
        return a <= 1 ? float(a) : -1.f;
    if (rest == "%")
        return a <= 100 ? float(a / 100) : -1.f;
    if (rest[0] != '/')
        return -1;
    rest           = str::trimSpace(rest.substr(1));
    const size_t m = readNumber(rest, 0, &b, false);
    if (m == 0 || m != rest.size() || b <= 0 || a > b)
        return -1;
    if (ratio)
        *ratio = str::concat({str::trimSpace(s.substr(0, n)), "/", rest});
    return float(a / b);
}

float progressInLine(std::string_view s, std::string *ratio) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (!isDigit(s[i]) || (i > 0 && (isAlnum(s[i - 1]) || s[i - 1] == '/' || s[i - 1] == '.' ||
                                         s[i - 1] == ',' || s[i - 1] == ':' || s[i - 1] == '-')))
            continue;
        double       a = 0;
        const size_t n = readNumber(s, i, &a, true);
        size_t       j = n;
        // "98.5%"
        if (j < s.size() && s[j] == '%') {
            if (a <= 100)
                return float(a / 100);
            i = n;
            continue;
        }
        // "9846/10000", "9846 / 10000"
        while (j < s.size() && s[j] == ' ')
            ++j;
        if (j < s.size() && s[j] == '/' && s.substr(i, n - i).find('.') == std::string_view::npos) {
            ++j;
            while (j < s.size() && s[j] == ' ')
                ++j;
            double       b         = 0;
            const size_t m         = readNumber(s, j, &b, false);
            const bool   endsClean = m < s.size()
                                         ? !(isAlnum(s[m]) || s[m] == '/' || s[m] == ':' ||
                                             (s[m] == '.' && m + 1 < s.size() && isDigit(s[m + 1])))
                                         : true;
            if (m > j && endsClean && b > 0 && a <= b) {
                if (ratio) {
                    std::string r(s.substr(i, m - i));
                    std::erase(r, ' ');
                    *ratio = std::move(r);
                }
                return float(a / b);
            }
        }
        i = n;
    }
    return -1;
}

std::string lastOutputLine(std::string_view tail, size_t maxChars) {
    // Terminal escapes out, every line break (\n, \r) a cut.
    std::string clean;
    clean.reserve(tail.size());
    for (size_t i = 0; i < tail.size(); ++i) {
        const char c = tail[i];
        if (c == '\x1b') {
            if (i + 1 < tail.size() && tail[i + 1] == '[') { // CSI … final byte
                for (i += 2; i < tail.size() && !(tail[i] >= 0x40 && tail[i] <= 0x7e); ++i) {
                }
            } else if (i + 1 < tail.size() && tail[i + 1] == ']') { // OSC … BEL or ST
                for (i += 2; i < tail.size(); ++i) {
                    if (tail[i] == '\x07')
                        break;
                    if (tail[i] == '\x1b' && i + 1 < tail.size() && tail[i + 1] == '\\') {
                        ++i;
                        break;
                    }
                }
            } else {
                ++i; // a two-byte escape
            }
        } else if (c == '\n' || c == '\r') {
            clean.push_back('\n');
        } else if (c == '\t') {
            clean.push_back(' ');
        } else if ((unsigned char)c >= 0x20 && c != 0x7f) {
            clean.push_back(c);
        }
    }
    std::string_view rest = clean;
    while (!rest.empty()) {
        const size_t           nl = rest.rfind('\n');
        const std::string_view line =
            str::trimSpace(nl == std::string_view::npos ? rest : rest.substr(nl + 1));
        if (!line.empty()) {
            // A tail read from the middle of a file may start mid-character.
            std::string_view l = line;
            while (!l.empty() && ((unsigned char)l[0] & 0xc0) == 0x80)
                l.remove_prefix(1);
            l = str::trimSpace(l);
            if (!l.empty())
                return utf8::ellipsize(l, maxChars);
        }
        if (nl == std::string_view::npos)
            break;
        rest = rest.substr(0, nl);
    }
    return {};
}

std::string progressNote(std::string_view exe) {
    return str::concat(
        {"# Progress reports (msga)\n"
         "The user follows this session in msga. When something you run will take more than a "
         "few minutes (a test suite, a batch job, a long build or download), report how it goes "
         "so the user can see it without asking:\n"
         "  \"",
         exe,
         "\" --claude-status \"<one short line, e.g. 9846/10000 programs, 0 failures>\" "
         "[--progress <done>/<total>] [--name <short job name>]\n"
         "Call it from the script or loop itself, at most every few seconds, and once when it "
         "ends (with --progress 1 when it finished). Give jobs running side by side each its own "
         "--name. A command whose output already ends in such a line (\"9846/10000\", \"98%\") "
         "needs no reports: msga shows its last line."}
    );
}

int runStatusCommand(int argc, char **argv) {
    const std::string_view exe = argc > 0 ? std::string_view(argv[0]) : "msga";
    std::string            text, name, ratio;
    float                  progress = -1;
    bool                   badArgs = false, badName = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            std::fputs(usage(exe).c_str(), stdout);
            return 0;
        }
        if (a == "--name" || str::startsWith(a, "--name=")) {
            name = std::string(
                str::trimSpace(a == "--name" ? (++i < argc ? argv[i] : "") : a.substr(7))
            );
            badName = name.empty();
            continue;
        }
        std::string_view p;
        bool             isProgress = false;
        if (a == "--progress") {
            isProgress = true;
            if (++i < argc)
                p = argv[i];
        } else if (str::startsWith(a, "--progress=")) {
            isProgress = true;
            p          = a.substr(11);
        }
        if (isProgress) {
            ratio.clear();
            progress = parseProgress(p, &ratio);
            badArgs  = badArgs || progress < 0;
            continue;
        }
        if (!text.empty())
            text.push_back(' ');
        text.append(a);
    }
    text = std::string(str::trimSpace(text));
    if (badArgs || badName || (text.empty() && progress < 0)) {
        std::fputs(
            badArgs   ? "--progress takes done/total (9846/10000), a percentage (98.5%) or a "
                        "fraction (0.985).\n"
            : badName ? "--name takes a short name for the job.\n"
                      : "Nothing to report.\n",
            stderr
        );
        std::fputs(usage(exe).c_str(), stderr);
        return 2;
    }
    const std::string jobDir = base::env("CLAUDE_JOB_DIR");
    if (jobDir.empty() || !file::isDir(jobDir)) {
        std::fputs(
            "Not inside a Claude Code background session (CLAUDE_JOB_DIR isn't set): "
            "nowhere to report to.\n",
            stderr
        );
        return 1;
    }
    Report r;
    r.text     = utf8::ellipsize(text, kMaxReport);
    r.name     = utf8::ellipsize(name, kMaxName);
    r.ratio    = ratio;
    r.progress = progress;
    r.atMs     = base::nowMicros() / 1000;
    if (!file::writeAtomic(reportPath(jobDir, r.name), reportJson(r), 0644, false)) {
        std::fputs("Couldn't write the report.\n", stderr);
        return 1;
    }
    return 0;
}

} // namespace claude
