#include "app/claude/approvals.h"

#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/str.h"
#include "base/time.h"

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace claude {

using i18n::tr;

namespace {

constexpr int     kPollMs       = 200;
constexpr int64_t kBeatMs       = 2'000;  // the hook touches its question this often
constexpr int64_t kTakeMs       = 10'000; // no msga took it by then: the dialog asks
constexpr int64_t kMsgaGoneMs   = 60'000; // msga's mark not touched for this long: gone
constexpr int64_t kStaleMs      = 15'000; // a question not touched for this long: its hook is gone
// Long enough never to cut a question short: msga's answer, or msga going
// away, ends the wait.
constexpr int     kHookTimeoutS = 7 * 24 * 3600;

std::string pathOf(std::string_view dir, std::string_view id, std::string_view ext) {
    return str::concat({dir, "/", id, ext});
}

void forget(std::string_view dir, std::string_view id) {
    for (std::string_view ext : {".answer", ".seen", ".json"})
        file::remove(pathOf(dir, id, ext));
}

int64_t ageMs(const std::string &path) {
    file::Stat s;
    if (!file::stat(path, &s))
        return -1;
    return (base::nowMicros() - s.mtimeMicros) / 1000;
}

std::string decision(void (*write)(json::Writer &), const json::Value *suggestion) {
    json::Writer w;
    w.beginObject().key("hookSpecificOutput").beginObject();
    w.key("hookEventName").value("PermissionRequest");
    w.key("decision").beginObject();
    write(w);
    if (suggestion)
        w.key("updatedPermissions").beginArray().value(*suggestion).endArray();
    w.endObject().endObject().endObject();
    return w.take();
}

std::string allow(const json::Value *suggestion) {
    return decision([](json::Writer &w) { w.key("behavior").value("allow"); }, suggestion);
}

std::string deny() {
    // As the dialog's "No": the call is refused and the turn ends.
    return decision(
        [](json::Writer &w) {
            w.key("behavior").value("deny");
            w.key("message").value("The user declined this in msga.");
            w.key("interrupt").value(true);
        },
        nullptr
    );
}

// "a", "a and b", "a, b and c".
std::string listed(const std::vector<std::string> &items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            out += i + 1 == items.size() ? " and " : ", ";
        out += items[i];
    }
    return out;
}

// The dialog's label for a suggestion; "" for one it doesn't offer.
std::string suggestionLabel(const json::Value &s, std::string_view cwd) {
    const std::string_view type = s["type"].str();
    if (type == "addRules" && s["behavior"].str() == "allow") {
        std::vector<std::string> commands, others;
        for (const json::Value r : s["rules"]) {
            const std::string_view tool    = r["toolName"].str();
            std::string_view       content = r["ruleContent"].str();
            if (tool == "Bash" && !content.empty()) {
                if (str::endsWith(content, ":*"))
                    content.remove_suffix(2);
                commands.emplace_back(content);
            } else if (!tool.empty()) {
                others.push_back(
                    content.empty() ? std::string(tool) : str::concat({tool, "(", content, ")"})
                );
            }
        }
        if (!commands.empty() && others.empty())
            return i18n::arg(
                tr("Yes, and don't ask again for %1 commands in %2"), listed(commands), cwd
            );
        for (std::string &c : commands)
            c = str::concat({"Bash(", c, ")"});
        others.insert(others.begin(), commands.begin(), commands.end());
        return others.empty() ? std::string()
                              : i18n::arg(tr("Yes, and don't ask again for %1"), listed(others));
    }
    if (type == "addDirectories") {
        std::vector<std::string> dirs;
        for (const json::Value d : s["directories"])
            if (!d.str().empty())
                dirs.emplace_back(d.str());
        return dirs.empty()
                   ? std::string()
                   : i18n::arg(
                         tr("Yes, and always allow access to %1 from this project"), listed(dirs)
                     );
    }
    if (type == "setMode" && s["mode"].str() == "acceptEdits")
        return tr("Yes, allow all edits during this session");
    return {};
}

void pause() {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
}

} // namespace

std::string permissionHookSettings(std::string_view exe, std::string_view dir) {
    json::Writer w;
    w.beginObject().key("hooks").beginObject().key("PermissionRequest").beginArray();
    w.beginObject().key("hooks").beginArray().beginObject();
    w.key("type").value("command");
    w.key("command").value(exe);
    w.key("args").beginArray().value(kPermissionHookFlag).value(dir).endArray();
    w.key("timeout").value(kHookTimeoutS);
    w.key("statusMessage").value("Waiting for msga");
    w.endObject().endArray().endObject();
    w.endArray().endObject().endObject();
    return w.take();
}

bool hasPermissionHook(std::string_view settingsJson) {
    return settingsJson.find(kPermissionHookFlag) != std::string_view::npos;
}

int runPermissionHook(std::string_view dir) {
    std::string in;
    char        buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, stdin)) > 0;)
        in.append(buf, n);
    HookQuestion q;
    if (dir.empty() || !parseHookQuestion(in, &q) || !file::makeDirs(dir))
        return 0;
#ifdef _WIN32
    const int64_t pid = _getpid();
#else
    const int64_t pid = getpid();
#endif
    const std::string id = str::concat({str::number(pid), "-", str::number(base::nowMicros())});
    const std::string question = pathOf(dir, id, ".json");
    const std::string seen     = pathOf(dir, id, ".seen");
    const std::string answer   = pathOf(dir, id, ".answer");
    if (!file::writeAtomic(question, in, 0600, false))
        return 0;
    const int64_t start = base::monotonicMs();
    int64_t       beat  = start;
    for (;;) {
        pause();
        std::string out;
        if (file::readAll(answer, &out)) {
            forget(dir, id);
            std::fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);
            return 0;
        }
        if (!file::exists(question))
            return 0; // msga let it go
        const int64_t now  = base::monotonicMs();
        const int64_t mark = ageMs(seen);
        if ((mark < 0 && now - start >= kTakeMs) || mark >= kMsgaGoneMs) {
            forget(dir, id); // no msga (any more): the dialog asks
            return 0;
        }
        if (now - beat >= kBeatMs) {
            beat = now;
            file::touch(question);
        }
    }
}

bool parseHookQuestion(std::string_view json, HookQuestion *out) {
    json::Document doc;
    if (!doc.parse(std::string(json)))
        return false;
    const json::Value o = doc.root();
    if (o["hook_event_name"].str() != "PermissionRequest" || o["session_id"].str().empty())
        return false;
    out->sessionId = std::string(o["session_id"].str());
    out->options.clear();
    out->options.push_back({tr("Yes"), allow(nullptr)});
    for (const json::Value s : o["permission_suggestions"]) {
        std::string label = suggestionLabel(s, o["cwd"].str());
        if (!label.empty())
            out->options.push_back({std::move(label), allow(&s)});
    }
    out->options.push_back({tr("No"), deny()});
    return true;
}

std::vector<HookQuestion> pendingHookQuestions(std::string_view dir) {
    std::vector<HookQuestion>   out;
    std::vector<file::DirEntry> entries;
    if (!file::listDir(dir, &entries))
        return out;
    for (const auto &e : entries) {
        if (e.isDir)
            continue;
        const std::string_view name = e.name;
        const size_t           dot  = name.rfind('.');
        if (dot == std::string_view::npos)
            continue;
        const std::string_view id = name.substr(0, dot), ext = name.substr(dot);
        const std::string      question = pathOf(dir, id, ".json");
        if (ext != ".json") {
            // What a question left behind once its hook was gone.
            if ((ext == ".seen" || ext == ".answer") && !file::exists(question) &&
                ageMs(std::string(dir) + "/" + e.name) > kStaleMs)
                file::remove(std::string(dir) + "/" + e.name);
            continue;
        }
        if (ageMs(question) > kStaleMs) {
            forget(dir, id); // its hook was ended without taking an answer
            continue;
        }
        if (file::exists(pathOf(dir, id, ".answer")))
            continue; // answered: the hook is on its way
        std::string  json;
        HookQuestion q;
        if (!file::readAll(question, &json) || !parseHookQuestion(json, &q))
            continue;
        q.id = std::string(id);
        out.push_back(std::move(q));
    }
    return out;
}

void takeHookQuestion(std::string_view dir, std::string_view id) {
    const std::string seen = pathOf(dir, id, ".seen");
    if (!file::touch(seen))
        file::writeAtomic(seen, {}, 0600, false);
}

bool answerHookQuestion(std::string_view dir, std::string_view id, std::string_view answer) {
    if (!file::exists(pathOf(dir, id, ".json")))
        return false;
    return file::writeAtomic(pathOf(dir, id, ".answer"), answer, 0600, false);
}

} // namespace claude
