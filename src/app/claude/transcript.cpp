#include "app/claude/transcript.h"

#include "app/claude/common.h"
#include "app/claude/outputs.h" // cleanPath
#include "app/claude/roles.h"

#include "base/crypto.h"
#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <utility>

namespace claude {
namespace {

// What a message of files alone says after its mentions, and is shown without.
constexpr std::string_view kFilesOnly = "(attached)";

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// The first line, trimmed, at most `maxLen` code points ("…" marks a cut).
std::string oneLine(std::string_view in, size_t maxLen = 100) {
    std::string_view s = str::trimSpace(in);
    std::string      out;
    if (const size_t nl = s.find('\n'); nl != std::string_view::npos)
        out = str::concat({str::trimSpace(s.substr(0, nl)), " …"});
    else
        out = std::string(s);
    return ellipsized(std::move(out), maxLen);
}

// The text between <tag> and </tag> (or the end), trimmed; "" when no <tag>.
std::string_view tagContent(std::string_view s, std::string_view tag) {
    const std::string open  = str::concat({"<", tag, ">"});
    const std::string close = str::concat({"</", tag, ">"});
    const size_t      a     = s.find(open);
    if (a == std::string_view::npos)
        return {};
    const size_t from = a + open.size();
    const size_t b    = s.find(close, from);
    return str::trimSpace(b == std::string_view::npos ? s.substr(from) : s.substr(from, b - from));
}

// "<abc-def>" at the start: a tag Claude Code wraps its injections in.
bool startsWithTag(std::string_view s) {
    if (s.size() < 3 || s[0] != '<' || s[1] < 'a' || s[1] > 'z')
        return false;
    for (size_t i = 2; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '>')
            return true;
        if (!((c >= 'a' && c <= 'z') || c == '-'))
            return false;
    }
    return false;
}

// What a typed prompt shows as — empty for the system text Claude Code records
// as "user" turns (command output, notifications, reminders, interruptions).
std::string cleanPrompt(std::string_view raw) {
    const std::string_view s = str::trimSpace(raw);
    if (s.empty())
        return {};
    if (str::startsWith(s, "<command-name>")) {
        const std::string_view name = tagContent(s, "command-name");
        const std::string_view args = tagContent(s, "command-args");
        std::string out = str::startsWith(name, "/") ? std::string(name) : str::concat({"/", name});
        if (!args.empty())
            out = str::concat({out, " ", args});
        return out;
    }
    if (str::startsWith(s, "<bash-input>"))
        return str::concat({"! ", tagContent(s, "bash-input")});
    // Every other leading tag is an injection, not something the user typed:
    // <local-command-stdout>, <bash-stdout>, <task-notification>, <system-reminder>…
    if (startsWithTag(s))
        return {};
    if (str::startsWith(s, "[Request interrupted"))
        return {};
    return std::string(s);
}

// `s` without terminal colour codes (ESC [ params letter).
std::string stripAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && (isDigit(s[j]) || s[j] == ';' || s[j] == '?'))
                ++j;
            if (j < s.size() && ((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z'))) {
                i = j;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// A local command's output ("<local-command-stdout>…</local-command-stdout>"),
// terminal colours stripped, into *out; false when `raw` isn't command output.
bool commandOutput(std::string_view raw, std::string *out) {
    const std::string_view s = str::trimSpace(raw);
    for (const std::string_view tag : {"local-command-stdout", "local-command-stderr"}) {
        if (s.size() < tag.size() + 2 || s[0] != '<' || s.substr(1, tag.size()) != tag ||
            s[tag.size() + 1] != '>')
            continue;
        *out = stripAnsi(tagContent(s, tag));
        return true;
    }
    return false;
}

bool isAgentTool(std::string_view name) {
    return name == "Agent" || name == "Task";
}

// Removes every " [Image #3]" / "[Image: source: …]" placeholder (one leading
// space or tab with it).
std::string dropImagePlaceholders(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t at = s.find("[Image", i);
        if (at == std::string_view::npos)
            break;
        size_t end = std::string_view::npos;
        size_t j   = at + 6;
        if (s.substr(j, 2) == " #" && j + 2 < s.size() && isDigit(s[j + 2])) {
            j += 2;
            while (j < s.size() && isDigit(s[j]))
                ++j;
            if (j < s.size() && s[j] == ']')
                end = j + 1;
        } else if (s.substr(j, 10) == ": source: ") {
            const size_t close = s.find(']', j + 10);
            if (close != std::string_view::npos)
                end = close + 1;
        }
        if (end == std::string_view::npos) {
            out.append(s.substr(i, at + 1 - i));
            i = at + 1;
            continue;
        }
        size_t from = at;
        if (from > i && (s[from - 1] == ' ' || s[from - 1] == '\t'))
            --from;
        out.append(s.substr(i, from - i));
        i = end;
    }
    out.append(s.substr(i));
    return out;
}

// Removes every "[Image #3]" with one space after it (promptHistory).
std::string dropImageMarks(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t at = s.find("[Image #", i);
        if (at == std::string_view::npos)
            break;
        size_t j = at + 8;
        while (j < s.size() && isDigit(s[j]))
            ++j;
        if (j == at + 8 || j >= s.size() || s[j] != ']') {
            out.append(s.substr(i, at + 1 - i));
            i = at + 1;
            continue;
        }
        out.append(s.substr(i, at - i));
        i = j + 1;
        if (i < s.size() && s[i] == ' ')
            ++i;
    }
    out.append(s.substr(i));
    return out;
}

std::string sha1Hex(std::string_view data) {
    return crypto::hex(crypto::bytes(crypto::sha1(data)));
}

// A JSON text walked without being built (skimRecord): what isn't asked for is
// only checked and skipped. It accepts what json::Document accepts, so a line
// it reads is a line the document would have read alike; any failure (a torn
// line, or a string it can't hand out unescaped) sends the line there.
class Skim {
public:
    explicit Skim(std::string_view s) : _s(s) {}

    bool ok() const { return _ok; }
    bool fail() { return _ok = false; }
    // Nothing but whitespace left, and no failure on the way.
    bool done() {
        ws();
        return _ok && _p == _s.size();
    }
    // The next value's first byte ('\0' at the end).
    char peek() {
        ws();
        return _p < _s.size() ? _s[_p] : '\0';
    }
    // Steps into the object or array (`open`) at nesting `depth`; more() then
    // comes before each of its members or elements.
    bool enter(char open, int depth) {
        if (peek() != open || depth >= json::Document::kMaxDepth)
            return fail();
        ++_p;
        return true;
    }
    // False at the container's end (its close taken) or on a failure.
    bool more(char close, bool &first) {
        if (!_ok)
            return false;
        const char c = peek();
        if (c == close) {
            ++_p;
            return false;
        }
        if (!first) {
            if (c != ',')
                return fail();
            ++_p;
        }
        first = false;
        return true;
    }
    // A member's name and its colon; one asked for (`name`) must have no
    // escapes.
    bool key(std::string_view *name) {
        bool escaped = false;
        if (peek() != '"' || !string(name, &escaped) || (escaped && name) || peek() != ':')
            return fail();
        ++_p;
        return true;
    }
    // A string value without escapes; anything else that isn't a string reads
    // as "" (json::Value::str()). An escaped string fails.
    bool plain(std::string_view *out, int depth) {
        if (peek() != '"') {
            *out = {};
            return skip(depth);
        }
        bool escaped = false;
        return string(out, &escaped) && (!escaped || fail());
    }
    // true; anything else reads as false (json::Value::boolean()).
    bool flag(bool *out, int depth) {
        *out = false;
        if (peek() == 't' && word("true"))
            return *out = true;
        return skip(depth);
    }
    bool skip(int depth) {
        switch (peek()) {
        case '"': {
            bool escaped;
            return string(nullptr, &escaped);
        }
        case '{':
        case '[': {
            const bool object = _s[_p] == '{';
            if (!enter(_s[_p], depth))
                return false;
            for (bool first = true; more(object ? '}' : ']', first);)
                if ((object && !key(nullptr)) || !skip(depth + 1))
                    return false;
            return _ok;
        }
        case 't':
            return word("true");
        case 'f':
            return word("false");
        case 'n':
            return word("null");
        default:
            return number();
        }
    }

private:
    void ws() {
        while (_p < _s.size() &&
               (_s[_p] == ' ' || _s[_p] == '\n' || _s[_p] == '\r' || _s[_p] == '\t'))
            ++_p;
    }
    bool word(std::string_view w) {
        if (_s.substr(_p, w.size()) != w)
            return fail();
        _p += w.size();
        return true;
    }
    // At its opening quote: the bytes between the quotes, escapes as written.
    bool string(std::string_view *raw, bool *escaped) {
        // Most of a transcript's bytes go through here: a tight loop over
        // locals, stopping only at a quote, a backslash or a control byte.
        const char *const begin = _s.data();
        const char *const end   = begin + _s.size();
        const char *const start = begin + _p + 1;
        const char       *p     = start;
        *escaped                = false;
        for (;;) {
            while (p < end && uint8_t(*p) >= 0x20 && *p != '"' && *p != '\\')
                ++p;
            if (p >= end || uint8_t(*p) < 0x20)
                return fail();
            if (*p == '"')
                break;
            *escaped = true;
            if (++p >= end)
                return fail();
            if (*p == 'u') {
                if (end - p < 5)
                    return fail();
                for (int k = 1; k <= 4; ++k)
                    if (str::hexDigit(p[k]) < 0)
                        return fail();
                p += 5;
            } else if (std::string_view("\"\\/bfnrt").find(*p) == std::string_view::npos) {
                return fail();
            } else {
                ++p;
            }
        }
        if (raw)
            *raw = std::string_view(start, size_t(p - start));
        _p = size_t(p - begin) + 1;
        return true;
    }
    bool number() {
        const auto digits = [this] {
            const size_t from = _p;
            while (_p < _s.size() && isDigit(_s[_p]))
                ++_p;
            return _p > from;
        };
        if (_p < _s.size() && _s[_p] == '-')
            ++_p;
        if (_p < _s.size() && _s[_p] == '0') {
            if (++_p < _s.size() && isDigit(_s[_p]))
                return fail();
        } else if (!digits()) {
            return fail();
        }
        if (_p < _s.size() && _s[_p] == '.' && (++_p, !digits()))
            return fail();
        if (_p < _s.size() && (_s[_p] == 'e' || _s[_p] == 'E')) {
            if (++_p < _s.size() && (_s[_p] == '+' || _s[_p] == '-'))
                ++_p;
            if (!digits())
                return fail();
        }
        return true;
    }

    std::string_view _s;
    size_t           _p  = 0;
    bool             _ok = true;
};

// message: {"content": [blocks]} at depth 1.
bool skimMessage(Skim &s, RecordSkim *r) {
    if (s.peek() != '{')
        return s.skip(1);
    s.enter('{', 1);
    bool sawContent = false;
    for (bool first = true; s.more('}', first);) {
        std::string_view k;
        if (!s.key(&k))
            return false;
        if (k != "content" || std::exchange(sawContent, true) || s.peek() != '[') {
            if (!s.skip(2))
                return false;
            continue;
        }
        r->contentArray = true;
        s.enter('[', 2);
        for (bool firstBlock = true; s.more(']', firstBlock);) {
            if (s.peek() != '{') {
                if (!s.skip(3))
                    return false;
                continue;
            }
            s.enter('{', 3);
            std::string_view type, id;
            bool             error = false, sawType = false, sawId = false, sawError = false;
            for (bool firstKey = true; s.more('}', firstKey);) {
                std::string_view bk;
                if (!s.key(&bk))
                    return false;
                bool ok;
                if (bk == "type" && !std::exchange(sawType, true))
                    ok = s.plain(&type, 4);
                else if (bk == "tool_use_id" && !std::exchange(sawId, true))
                    ok = s.plain(&id, 4);
                else if (bk == "is_error" && !std::exchange(sawError, true))
                    ok = s.flag(&error, 4);
                else
                    ok = s.skip(4);
                if (!ok)
                    return false;
            }
            if (type == "tool_result")
                r->toolResults.push_back({id, error});
            else if (type == "image")
                r->image = true;
        }
    }
    return s.ok();
}

} // namespace

bool skimRecord(std::string_view line, RecordSkim *r) {
    r->toolResults.clear();
    std::vector<RecordSkim::ToolResult> keep = std::move(r->toolResults);
    *r                                       = {};
    r->toolResults                           = std::move(keep); // its capacity, line after line
    Skim       s(line);
    unsigned   seen = 0; // the members read already: only the first of a name counts
    const auto once = [&seen](unsigned bit) {
        const bool first = !(seen & bit);
        seen |= bit;
        return first;
    };
    if (!s.enter('{', 0))
        return false;
    for (bool first = true; s.more('}', first);) {
        std::string_view k;
        if (!s.key(&k))
            return false;
        ++r->members;
        bool ok;
        if (k == "type" && once(1u << 0))
            ok = s.plain(&r->type, 1);
        else if (k == "uuid" && once(1u << 1))
            ok = s.plain(&r->uuid, 1);
        else if (k == "timestamp" && once(1u << 2))
            ok = s.plain(&r->timestamp, 1);
        else if (k == "version" && once(1u << 3))
            ok = s.plain(&r->version, 1);
        else if (k == "permissionMode" && once(1u << 4))
            ok = s.plain(&r->permissionMode, 1);
        else if (k == "cwd" && once(1u << 5))
            ok = s.plain(&r->cwd, 1);
        else if (k == "isMeta" && once(1u << 6))
            ok = s.flag(&r->isMeta, 1);
        else if (k == "isCompactSummary" && once(1u << 7))
            ok = s.flag(&r->isCompactSummary, 1);
        else if (k == "isSidechain" && once(1u << 8))
            ok = s.flag(&r->isSidechain, 1);
        else if (k == "origin" && once(1u << 9)) {
            r->origin = s.peek() == '{';
            ok        = s.skip(1);
        } else if (k == "message" && once(1u << 10)) {
            ok = skimMessage(s, r);
        } else if (k == "toolUseResult" && once(1u << 11) && s.peek() == '{') {
            s.enter('{', 1);
            bool sawAgent = false, sawStatus = false, sawTask = false;
            ok = true;
            for (bool firstKey = true; ok && s.more('}', firstKey);) {
                std::string_view rk;
                if (!s.key(&rk))
                    return false;
                if (rk == "agentId" && !std::exchange(sawAgent, true))
                    ok = s.plain(&r->agentId, 2);
                else if (rk == "status" && !std::exchange(sawStatus, true))
                    ok = s.plain(&r->status, 2);
                else if (rk == "backgroundTaskId" && !std::exchange(sawTask, true))
                    ok = s.plain(&r->backgroundTaskId, 2);
                else
                    ok = s.skip(2);
            }
        } else {
            ok = s.skip(1);
        }
        if (!ok)
            return false;
    }
    return s.done();
}

std::string quotePastes(std::string_view prompt) {
    constexpr std::string_view kOpen = "<pasted_content";
    std::string                out;
    size_t                     i = 0;
    for (size_t at = prompt.find(kOpen); at != std::string_view::npos;
         at        = prompt.find(kOpen, at + 1)) {
        // The attribute part (` id="e482"` or nothing), repeated on the close.
        const size_t gt = prompt.find('>', at + kOpen.size());
        if (gt == std::string_view::npos)
            break;
        const std::string_view attrs = prompt.substr(at + kOpen.size(), gt - at - kOpen.size());
        if (!attrs.empty() && attrs[0] != ' ')
            continue;
        const std::string close = str::concat({"</pasted_content", attrs, ">"});
        const size_t      end   = prompt.find(close, gt + 1);
        if (end == std::string_view::npos)
            continue;
        std::string_view body = prompt.substr(gt + 1, end - gt - 1);
        while (!body.empty() && (body.front() == '\n' || body.front() == '\r'))
            body.remove_prefix(1);
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
            body.remove_suffix(1);
        out.append(prompt.substr(i, at - i));
        if (!out.empty() && out.back() != '\n')
            out += '\n';
        const auto lines = str::split(body, '\n');
        for (size_t k = 0; k < lines.size(); ++k) {
            std::string_view line = lines[k];
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (k)
                out += '\n';
            out.append(line.empty() ? std::string_view(">") : std::string_view("> "));
            out.append(line);
        }
        i = end + close.size();
        if (i < prompt.size() && prompt[i] != '\n')
            out += '\n';
        at = i - 1;
    }
    if (i == 0)
        return std::string(prompt);
    out.append(prompt.substr(i));
    return out;
}

std::string summarizeToolInput(std::string_view toolName, const json::Value &input) {
    auto        arg = [&](const char *key) { return input[key].str(); };
    std::string s;
    if (toolName == "Bash")
        s = arg("description").empty() ? arg("command") : arg("description");
    else if (
        toolName == "Read" || toolName == "Write" || toolName == "Edit" ||
        toolName == "NotebookEdit"
    ) {
        const std::string_view path =
            arg("file_path").empty() ? arg("notebook_path") : arg("file_path");
        s = path.empty() ? std::string() : homeRelative(path);
    } else if (toolName == "Grep" || toolName == "Glob")
        s = arg("pattern");
    else if (toolName == "WebFetch")
        s = arg("url");
    else if (toolName == "WebSearch")
        s = arg("query");
    else if (isAgentTool(toolName))
        s = arg("description");
    else if (toolName == "Skill")
        s = arg("skill");
    if (s.empty()) {
        // Anything else: its first string argument says the most.
        for (const json::Value v : input)
            if (v.isString() && !str::trimSpace(v.str()).empty()) {
                s = v.str();
                break;
            }
    }
    return oneLine(s);
}

// ── The parser ──────────────────────────────────────────────────────────────

void TranscriptParser::feed(std::string_view bytes) {
    // One document and skim for every line of the call: a whole transcript
    // read at once doesn't allocate (and free) them per line.
    json::Document doc;
    RecordSkim     skim;
    const auto     line = [&](std::string_view l) {
        l = str::trimSpace(l);
        if (l.empty())
            return;
        handleLine(l, doc, skim);
        while (_revs.size() < _items.size())
            _revs.push_back(++_rev);
    };
    // Only an unfinished last line is kept between calls: a whole transcript
    // (tens of MB, the first scan feeds every session's at once) is parsed in
    // place rather than copied first.
    if (!_partial.empty()) {
        const size_t nl = bytes.find('\n');
        if (nl == std::string_view::npos) {
            _partial.append(bytes);
            return;
        }
        _partial.append(bytes.substr(0, nl));
        line(_partial);
        std::string().swap(_partial);
        bytes.remove_prefix(nl + 1);
    }
    size_t start = 0;
    for (size_t nl; (nl = bytes.find('\n', start)) != std::string_view::npos; start = nl + 1)
        line(bytes.substr(start, nl - start));
    _partial.assign(bytes.substr(start));
}

uint64_t TranscriptParser::revision(size_t index) const {
    return index < _revs.size() ? _revs[index] : 0;
}

void TranscriptParser::touch(size_t index) {
    // An item added by this line gets its revision when the line is done.
    if (index < _revs.size())
        _revs[index] = ++_rev;
}

void TranscriptParser::reserveTs(int64_t micros) {
    _lastMicros = std::max(_lastMicros, micros);
}

int64_t TranscriptParser::taskStoppedAt(const std::string &taskId) const {
    const auto it = _taskStopped.find(taskId);
    return it == _taskStopped.end() ? 0 : it->second;
}

std::vector<TranscriptParser::BackgroundCommand> TranscriptParser::runningCommands() const {
    std::vector<BackgroundCommand> out;
    for (const BackgroundCommand &c : _commands)
        if (taskStoppedAt(c.taskId) < c.startedAt)
            out.push_back(c);
    return out;
}

int64_t TranscriptParser::nextTs(int64_t micros) {
    // Strictly increasing, so two records in the same millisecond (a text block
    // and a tool call streamed together) still get distinct, ordered ids.
    if (micros <= _lastMicros)
        micros = _lastMicros + 1;
    _lastMicros = micros;
    return micros;
}

void TranscriptParser::closeToolGroup() {
    _openToolGroup = -1;
}

void TranscriptParser::resolvePendingText(TranscriptItem::State state) {
    if (_pendingText < 0)
        return;
    _items[size_t(_pendingText)].state = state;
    touch(size_t(_pendingText));
    _pendingText = -1;
}

void TranscriptParser::endTurn() {
    resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    _turnOpen = false;
}

void TranscriptParser::openTurn(int64_t micros) {
    if (!_turnOpen)
        _turnStartedAt = micros; // a prompt joining a turn under way doesn't restart it
    _turnOpen = true;
}

void TranscriptParser::addPrompt(
    std::string_view         text,
    int64_t                  micros,
    std::vector<std::string> images,
    std::vector<std::string> imageNames
) {
    TranscriptItem item;
    item.kind       = TranscriptItem::Kind::UserPrompt;
    item.ts         = nextTs(micros);
    item.images     = std::move(images);
    item.imageNames = std::move(imageNames);
    // Files sent from msga ride the text as mentions (see withAttachments).
    item.text       = takeAttachments(quotePastes(typedPrompt(text, &item.relayTo)), &item.images);
    while (item.imageNames.size() < item.images.size())
        item.imageNames.emplace_back(file::baseName(item.images[item.imageNames.size()]));
    item.uuid = _lineUuid;
    _items.push_back(std::move(item));
    openTurn(micros);
}

bool TranscriptParser::addPeerMessage(const json::Value &origin, int64_t micros, bool newTurn) {
    if (!origin.isObject() || origin["kind"].str() != "peer")
        return false;
    TranscriptItem item;
    item.kind                   = TranscriptItem::Kind::PeerMessage;
    // The message as sent, without Claude Code's wrapping (<agent-message …>).
    const std::string_view body = origin["body"].str();
    std::string            text(body);
    if (origin["handback"].boolean()) {
        // "[Subagent hand-back] …(a note for the model)… The report follows:",
        // then the report with every line indented by two spaces.
        item.agentId                               = std::string(origin["from"].str());
        static constexpr std::string_view kFollows = "The report follows:\n";
        if (const size_t at = body.find(kFollows); at != std::string_view::npos) {
            text.clear();
            std::string_view rest = body.substr(at + kFollows.size());
            for (;;) {
                const size_t     nl   = rest.find('\n');
                std::string_view line = rest.substr(0, nl);
                if (str::startsWith(line, "  "))
                    line.remove_prefix(2);
                text += line;
                if (nl == std::string_view::npos)
                    break;
                text += '\n';
                rest.remove_prefix(nl + 1);
            }
        }
    } else {
        item.peerName = std::string(origin["name"].str());
    }
    item.text = std::string(str::trimSpace(text));
    if (item.text.empty())
        return true; // nothing to show, but still nobody's prompt
    if (newTurn)
        resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    item.ts = nextTs(micros);
    _items.push_back(std::move(item));
    if (newTurn)
        openTurn(micros); // the session answers it
    return true;
}

void TranscriptParser::addCommandOutput(std::string_view output, int64_t micros) {
    closeToolGroup();
    const std::string_view text = str::trimSpace(output);
    if (!text.empty()) {
        TranscriptItem item;
        item.kind = TranscriptItem::Kind::AssistantText;
        item.ts   = nextTs(micros);
        // Several lines are a terminal layout (a chart, a list): kept monospaced.
        item.text = text.find('\n') != std::string_view::npos
                        ? str::concat({"```\n", text, "\n```"})
                        : std::string(text);
        item.uuid = _lineUuid;
        _items.push_back(std::move(item));
        _commandOutput = int(_items.size()) - 1;
    }
    endTurn();
}

void TranscriptParser::noteTaskStopped(std::string_view taskId, int64_t micros) {
    int64_t &at = _taskStopped[std::string(taskId)];
    at          = std::max(at, micros);
    std::erase_if(_commands, [&](const BackgroundCommand &c) {
        return c.taskId == taskId && c.startedAt <= at;
    });
}

void TranscriptParser::noteBackgroundCommand(
    std::string_view taskId, std::string_view toolUseId, const json::Value &content, int64_t micros
) {
    // The output file is named in the result's text, ending "<id>.output".
    std::string text;
    if (content.isString())
        text = content.str();
    else
        for (const json::Value b : content)
            if (b["type"].str() == "text")
                text.append(b["text"].str()).push_back('\n');
    const std::string name = str::concat({taskId, ".output"});
    const size_t      end  = text.find(name);
    if (taskId.empty() || micros <= 0 || end == std::string::npos)
        return;
    size_t begin = end;
    while (begin > 0 && text[begin - 1] != ' ' && text[begin - 1] != '\n' &&
           text[begin - 1] != '\t')
        --begin;
    BackgroundCommand c;
    c.taskId     = taskId;
    c.outputPath = text.substr(begin, end + name.size() - begin);
    c.startedAt  = micros;
    // The command itself: the call's summary is its description when it has one.
    if (const auto cmd = _bashCommands.find(std::string(toolUseId)); cmd != _bashCommands.end())
        c.command = cmd->second;
    for (auto it = _items.rbegin(); it != _items.rend() && c.command.empty(); ++it)
        for (const ToolCall &call : it->tools)
            if (call.toolUseId == toolUseId)
                c.command = call.summary;
    // Already stopped: a notification read before (a copy's history).
    if (taskStoppedAt(c.taskId) >= micros)
        return;
    std::erase_if(_commands, [&](const BackgroundCommand &x) { return x.taskId == taskId; });
    _commands.push_back(std::move(c));
}

namespace {

// How a task ended, as its notification says: "<status>failed</status>" and
// "<summary>Background command "x" failed with exit code 3</summary>".
TranscriptParser::TaskEnd taskEndIn(std::string_view text, int64_t micros) {
    using Kind = TranscriptParser::TaskEnd::Kind;
    TranscriptParser::TaskEnd e;
    e.at                         = micros;
    constexpr std::string_view k = "<status>";
    std::string_view           status;
    if (const size_t at = text.find(k); at != std::string_view::npos) {
        const size_t end = text.find('<', at + k.size());
        status           = str::trimSpace(text.substr(at + k.size(), end - at - k.size()));
    }
    constexpr std::string_view code = "exit code ";
    if (const size_t at = text.find(code); at != std::string_view::npos)
        for (size_t i = at + code.size();
             i < text.size() && text[i] >= '0' && text[i] <= '9' && e.exitCode < 100000;
             ++i)
            e.exitCode = e.exitCode * 10 + (text[i] - '0');
    e.kind = status == "killed" || status == "stopped" ? Kind::Stopped
             : status == "failed" || e.exitCode != 0   ? Kind::Failed
                                                       : Kind::Done;
    return e;
}

} // namespace

void TranscriptParser::noteTaskNotification(std::string_view text, int64_t micros) {
    // Only a notification itself: its <result> may quote anything.
    if (micros <= 0 || !str::startsWith(str::trimSpace(text), "<task-notification>"))
        return;
    constexpr std::string_view kOpen = "<task-id>", kClose = "</task-id>";
    for (size_t at = text.find(kOpen); at != std::string_view::npos; at = text.find(kOpen, at)) {
        at += kOpen.size();
        const size_t end = text.find('<', at);
        if (end == std::string_view::npos)
            break;
        if (end > at && text.substr(end, kClose.size()) == kClose) {
            const std::string_view id = str::trimSpace(text.substr(at, end - at));
            noteTaskStopped(id, micros);
            _taskEnds[std::string(id)] = taskEndIn(text, micros);
        }
        at = end;
    }
}

const TranscriptParser::TaskEnd *TranscriptParser::taskEnd(const std::string &taskId) const {
    const auto it = _taskEnds.find(taskId);
    return it == _taskEnds.end() ? nullptr : &it->second;
}

bool TranscriptParser::hasRecord(std::string_view uuid) const {
    return !uuid.empty() && _seenUuids.count(crypto::fnv1a(uuid));
}

bool TranscriptParser::beginRecord(
    std::string_view uuid,
    std::string_view timestamp,
    std::string_view version,
    std::string_view permissionMode,
    int64_t         *micros
) {
    _lineUuid = uuid;
    // A copy of a session starts with the records it was copied from, same
    // uuids and all — the ones read already, from the session it continues.
    if (!_lineUuid.empty() && !_seenUuids.insert(crypto::fnv1a(_lineUuid)).second)
        return false;
    *micros = base::parseIsoMicros(timestamp);
    if (*micros > _lastActivity)
        _lastActivity = *micros;
    if (*micros > 0 && _keepActivity)
        _activity.push_back(*micros);
    if (!version.empty())
        _version = version;
    if (!permissionMode.empty())
        _permissionMode = permissionMode;
    return true;
}

// What handleUser makes of a record holding nothing but tool results — no
// origin, not meta, no pasted image — from its skim alone.
bool TranscriptParser::handleToolResults(const RecordSkim &r) {
    if (r.type != "user" || r.origin || r.isMeta || r.isCompactSummary || !r.contentArray ||
        r.image || r.toolResults.empty() || !r.backgroundTaskId.empty())
        return false;
    int64_t micros = 0;
    if (!beginRecord(r.uuid, r.timestamp, r.version, r.permissionMode, &micros))
        return true;
    for (const RecordSkim::ToolResult &t : r.toolResults)
        toolResult(t.id, t.error, r.agentId, r.status, micros);
    return true;
}

void TranscriptParser::handleLine(std::string_view line, json::Document &doc, RecordSkim &skim) {
    // Tool output is most of a transcript's bytes, and all the parser takes
    // of it is which call it answers: such a record is only skimmed. (That
    // text can't stand in a JSON string unescaped: only a block says it —
    // near the record's start, as Claude Code writes it; one that says it
    // later is read whole, as any other.)
    constexpr size_t kBlockWithin = 1024;
    if (line.substr(0, kBlockWithin).find(R"("type":"tool_result")") != std::string_view::npos &&
        skimRecord(line, &skim) && handleToolResults(skim))
        return;
    if (!doc.parse(line, nullptr) || !doc.root().isObject() || doc.root().size() == 0)
        return; // a torn or foreign line — skip, never fail the whole transcript
    const json::Value      o      = doc.root();
    const std::string_view type   = o["type"].str();
    int64_t                micros = 0;
    if (!beginRecord(
            o["uuid"].str(),
            o["timestamp"].str(),
            o["version"].str(),
            o["permissionMode"].str(),
            &micros
        ))
        return;
    if (type == "queue-operation") {
        // A notification queued for the session, the moment its task stopped
        // (the prompt that delivers it may come much later).
        if (o["operation"].str() == "enqueue")
            noteTaskNotification(o["content"].str(), micros);
        return;
    }
    if (type == "ai-title") {
        _aiTitle = str::trimSpace(o["aiTitle"].str());
        return;
    }
    if (type == "system") {
        const std::string_view subtype = o["subtype"].str();
        if (subtype == "turn_duration") {
            endTurn();
        } else if (subtype == "local_command") {
            // A command Claude Code runs itself (/context, /model…): the command,
            // then its output — which is all there is to that turn.
            const std::string_view content = o["content"].str();
            std::string            output;
            if (commandOutput(content, &output)) {
                addCommandOutput(output, micros);
            } else if (const std::string cmd = cleanPrompt(content); !cmd.empty()) {
                resolvePendingText(TranscriptItem::State::Final);
                closeToolGroup();
                addPrompt(cmd, micros);
            }
        }
        return;
    }
    if (type == "attachment") {
        // A prompt typed while Claude was busy: recorded as a queued command that
        // the running turn picks up — the turn goes on, so nothing is resolved.
        const json::Value      a     = o["attachment"];
        const std::string_view atype = a["type"].str();
        // The system prompt as sent, recorded once and sent again on resume:
        // a role msga started the session with is its last part (roles.h).
        if (atype == "prompt_snapshot") {
            RoleMark mark = roleInSystemPrompt(a["systemPrompt"]);
            _role         = std::move(mark.id);
            _roleName     = std::move(mark.name);
            return;
        }
        // The subagent types offered: the whole list first, then what changed.
        if (atype == "agent_listing_delta") {
            if (a["isInitial"].boolean())
                _agentTypes.clear();
            for (const json::Value v : a["addedTypes"])
                _agentTypes.emplace(v.str());
            for (const json::Value v : a["removedTypes"])
                _agentTypes.erase(std::string(v.str()));
            return;
        }
        if (atype == "queued_command") {
            noteTaskNotification(a["prompt"].str(), micros);
            if (a["commandMode"].str() == "prompt" && !addPeerMessage(a["origin"], micros, false)) {
                const std::string text = cleanPrompt(a["prompt"].str());
                if (!text.empty()) {
                    closeToolGroup();
                    addPrompt(text, micros);
                }
            }
        }
        return;
    }

    const json::Value message = o["message"];
    if (type == "user")
        handleUser(o, message["content"], micros);
    else if (type == "assistant")
        handleAssistant(o, message, micros);
}

void TranscriptParser::toolResult(
    std::string_view id,
    bool             failed,
    std::string_view agentId,
    std::string_view status,
    int64_t          micros
) {
    // Newest first: the call is almost always in the latest item.
    if (!_bashCommands.empty())
        _bashCommands.erase(std::string(id));
    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
        auto call = std::find_if(it->tools.begin(), it->tools.end(), [&](const ToolCall &c) {
            return c.toolUseId == id;
        });
        if (call == it->tools.end())
            continue;
        call->error = failed;
        touch(size_t(std::distance(it, _items.rend()) - 1));
        if (it->kind == TranscriptItem::Kind::Subagent && !agentId.empty()) {
            it->agentId = agentId;
            // A foreground subagent's result comes when it's done
            // ("completed"), and no notification follows: the result is its
            // stop. A background one's ("async_launched") only says it started.
            if (status != "async_launched")
                noteTaskStopped(agentId, micros);
        }
        break;
    }
}

void TranscriptParser::handleUser(
    const json::Value &o, const json::Value &content, int64_t micros
) {
    if (addPeerMessage(o["origin"], micros, true))
        return;
    // What Claude Code tells the model (skill bodies, caveats) and the
    // summary a compaction starts from aren't anything anyone typed.
    if (o["isMeta"].boolean()) {
        // /context also writes its report as markdown for the model, right
        // after the terminal drawing: that reads far better here.
        const std::string_view md = str::trimSpace(content.str());
        if (_commandOutput >= 0 && _commandOutput == int(_items.size()) - 1 && !md.empty() &&
            md[0] != '<') {
            _items[size_t(_commandOutput)].text = md;
            touch(size_t(_commandOutput));
        }
        _commandOutput = -1;
        return;
    }
    if (o["isCompactSummary"].boolean())
        return;
    const json::Value origin = o["origin"];
    if (origin.isObject() && origin["kind"].str() != "human") {
        if (content.isString())
            noteTaskNotification(content.str(), micros);
        return; // task notifications and other machine-originated turns
    }

    std::string              text;
    std::vector<std::string> images;
    if (content.isString()) {
        text = content.str();
    } else if (content.isArray()) {
        bool sawToolResult = false;
        for (const json::Value b : content) {
            const std::string_view bt = b["type"].str();
            if (bt == "tool_result") {
                sawToolResult            = true;
                const json::Value result = o["toolUseResult"];
                // Before toolResult, which forgets the call's command.
                if (const std::string_view task = result["backgroundTaskId"].str(); !task.empty())
                    noteBackgroundCommand(task, b["tool_use_id"].str(), b["content"], micros);
                toolResult(
                    b["tool_use_id"].str(),
                    b["is_error"].boolean(),
                    result["agentId"].str(),
                    result["status"].str(),
                    micros
                );
            } else if (bt == "text") {
                if (!text.empty())
                    text += '\n';
                text += b["text"].str();
            } else if (bt == "image") {
                // A pasted image rides the prompt as base64.
                const json::Value src = b["source"];
                if (src["type"].str() == "base64") {
                    // Keyed by the record: read again (a copy, a re-read), it's
                    // found without hashing megabytes of base64.
                    const std::string key =
                        _lineUuid.empty()
                            ? std::string()
                            : str::concat({_lineUuid, "-", str::number(int64_t(images.size()))});
                    std::string path =
                        cachePastedImage(src["media_type"].str(), src["data"].str(), key);
                    if (!path.empty())
                        images.push_back(std::move(path));
                }
            }
        }
        if (sawToolResult)
            return; // tool output rides a "user" record; it isn't a prompt
    }
    if (std::string output; commandOutput(text, &output)) {
        addCommandOutput(output, micros); // e.g. what /compact says when done
        return;
    }
    const bool isCommand = str::startsWith(str::trimSpace(text), "<command-name>");
    text                 = cleanPrompt(text);
    // /compact records the prompt as typed, then again as the command it ran.
    if (isCommand && !_items.empty() && _items.back().kind == TranscriptItem::Kind::UserPrompt &&
        _items.back().text == text)
        return;
    if (!images.empty()) {
        // The images are attached; drop their "[Image #3]" placeholders (and
        // the "[Image: source: …]" lines some clients add) from the text.
        text = std::string(str::trimSpace(dropImagePlaceholders(text)));
    }
    if (text.empty() && images.empty())
        return;
    // A new prompt starts a new turn: whatever Claude said last in the
    // previous one was its answer.
    resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    // Named after Claude Code's paste numbers ("[Image #3]"), in order.
    std::vector<std::string> names;
    const json::Value        ids = o["imagePasteIds"];
    for (size_t i = 0; i < images.size(); ++i) {
        const std::string_view ext = file::extension(images[i]);
        names.push_back(
            i < ids.size() ? str::concat({"Image ", str::number(ids[i].integer()), ".", ext})
                           : str::concat({"Image.", ext})
        );
    }
    addPrompt(text, micros, std::move(images), std::move(names));
}

void TranscriptParser::handleAssistant(
    const json::Value &o, const json::Value &message, int64_t micros
) {
    const json::Value content = message["content"];
    if (!content.isArray())
        return;
    const std::string_view model = message["model"].str();
    if (!model.empty() && model != "<synthetic>")
        _model = model;
    // Claude Code's stand-in answer to a turn that asked for none (after a
    // command's output) — not something Claude said.
    if (model == "<synthetic>" && content.size() == 1 &&
        str::trimSpace(content[0]["text"].str()) == "No response requested.")
        return;
    // An API error Claude Code reports as the answer (verified 2.1.283):
    // {"type":"assistant","isApiErrorMessage":true,"error":"authentication_failed",…}.
    const bool loginError =
        o["isApiErrorMessage"].boolean() && o["error"].str() == "authentication_failed";
    if (loginError)
        _loginFailedAt = std::max(_loginFailedAt, micros);
    for (const json::Value b : content) {
        const std::string_view bt = b["type"].str();
        if (bt == "text") {
            const std::string_view text = str::trimSpace(b["text"].str());
            if (text.empty())
                continue;
            // Two texts in a row within one turn: the earlier one wasn't the end.
            resolvePendingText(TranscriptItem::State::Progress);
            closeToolGroup();
            TranscriptItem item;
            item.kind       = TranscriptItem::Kind::AssistantText;
            item.state      = TranscriptItem::State::Pending;
            item.ts         = nextTs(micros);
            item.text       = text;
            item.uuid       = _lineUuid;
            item.loginError = loginError;
            _items.push_back(std::move(item));
            _pendingText = int(_items.size()) - 1;
            openTurn(micros);
        } else if (bt == "tool_use") {
            // Claude kept working after its text, so that text was an update.
            resolvePendingText(TranscriptItem::State::Progress);
            ToolCall call;
            call.toolUseId          = b["id"].str();
            call.name               = b["name"].str();
            const json::Value input = b["input"];
            call.summary            = summarizeToolInput(call.name, input);
            openTurn(micros);
            // A command, kept until its result says whether it went on in
            // the background (noteBackgroundCommand names the job by it).
            if (call.name == "Bash" && !call.toolUseId.empty()) {
                if (_bashCommands.size() >= 64)
                    _bashCommands.clear(); // results that never came
                _bashCommands[call.toolUseId] = std::string(input["command"].str());
            }
            // Claude stopping a task of its own: no notification follows.
            if (call.name == "TaskStop" || call.name == "KillShell" || call.name == "KillBash") {
                std::string_view id = input["task_id"].str();
                if (id.empty())
                    id = input["shell_id"].str();
                if (!id.empty() && micros > 0) {
                    noteTaskStopped(id, micros);
                    TaskEnd &e = _taskEnds[std::string(id)];
                    e          = TaskEnd{micros, TaskEnd::Kind::Stopped, 0};
                }
            }
            if (call.name == "SubagentHandback") {
                // A subagent's report to its session: its answer, in its thread.
                const std::string_view report = str::trimSpace(input["message"].str());
                if (!report.empty()) {
                    closeToolGroup();
                    TranscriptItem item;
                    item.kind = TranscriptItem::Kind::AssistantText;
                    item.ts   = nextTs(micros);
                    item.text = report; // no uuid: a tool call can't go without its result
                    _items.push_back(std::move(item));
                    continue;
                }
            }
            if (isAgentTool(call.name)) {
                closeToolGroup();
                TranscriptItem item;
                item.kind      = TranscriptItem::Kind::Subagent;
                item.ts        = nextTs(micros);
                item.text      = call.summary;
                item.agentType = input["subagent_type"].str();
                item.agentRole = roleInAgentPrompt(input["prompt"].str());
                item.tools.push_back(std::move(call));
                _items.push_back(std::move(item));
                continue;
            }
            if (_openToolGroup < 0) {
                TranscriptItem item;
                item.kind = TranscriptItem::Kind::ToolGroup;
                item.ts   = nextTs(micros);
                _items.push_back(std::move(item));
                _openToolGroup = int(_items.size()) - 1;
            }
            _items[size_t(_openToolGroup)].tools.push_back(std::move(call));
            touch(size_t(_openToolGroup));
        }
        // thinking, redacted_thinking, …: not shown
    }
}

// ── Images and files ────────────────────────────────────────────────────────

std::string
cachePastedImage(std::string_view mediaType, std::string_view base64, std::string_view key) {
    if (!str::startsWith(mediaType, "image/") || base64.empty())
        return {};
    std::string ext = str::asciiLower(mediaType.substr(6));
    if (ext == "jpeg")
        ext = "jpg";
    const bool        usable = !key.empty() && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '-';
    });
    const std::string path   = str::concat(
        {dirs().cache, "/images/", usable ? std::string(key) : sha1Hex(base64), ".", ext}
    );
    if (file::exists(path))
        return path;
    std::string bytes;
    if (!crypto::base64Decode(base64, &bytes) || !file::writeAtomic(path, bytes))
        return {};
    return path;
}

std::string uploadsDir() {
    return dirs().cache + "/uploads";
}

std::string cacheUpload(std::string_view path) {
    std::string data;
    if (file::isDir(path) || !file::readAll(path, &data))
        return {};
    const std::string copy =
        str::concat({uploadsDir(), "/", sha1Hex(data), "/", file::baseName(path)});
    if (file::exists(copy))
        return copy;
    return file::writeAtomic(copy, data) ? copy : std::string();
}

std::string withAttachments(std::string_view text, const std::vector<std::string> &paths) {
    if (paths.empty())
        return std::string(text);
    std::string mentions;
    for (const std::string &p : paths) {
        if (!mentions.empty())
            mentions += ' ';
        const bool spaced = std::any_of(p.begin(), p.end(), isSpace);
        mentions += spaced ? str::concat({"@\"", p, "\""}) : str::concat({"@", p});
    }
    const std::string_view body = str::trimSpace(text);
    // A slash command keeps its place at the start (it goes by `--resume`,
    // never typed, so the mentions may end it).
    if (str::startsWith(body, "/"))
        return str::concat({body, " ", mentions});
    return str::concat({mentions, " ", body.empty() ? kFilesOnly : body});
}

std::string takeAttachments(std::string_view prompt, std::vector<std::string> *paths) {
    // @"<uploads>/…" or @<uploads>/… (to the next whitespace), one space after.
    const std::string dir = uploadsDir() + "/";
    std::string       text;
    text.reserve(prompt.size());
    size_t i = 0;
    for (size_t at = prompt.find('@'); at != std::string_view::npos;
         at        = prompt.find('@', at + 1)) {
        if (at < i)
            continue;
        const std::string_view rest = prompt.substr(at + 1);
        std::string_view       path;
        size_t                 end = 0; // past the mention, in `rest`
        if (str::startsWith(rest, "\"") && str::startsWith(rest.substr(1), dir)) {
            const size_t close = rest.find('"', 1 + dir.size());
            if (close != std::string_view::npos && close > 1 + dir.size()) {
                path = rest.substr(1, close - 1);
                end  = close + 1;
            }
        } else if (str::startsWith(rest, dir)) {
            size_t j = dir.size();
            while (j < rest.size() && !isSpace(rest[j]))
                ++j;
            if (j > dir.size()) {
                path = rest.substr(0, j);
                end  = j;
            }
        }
        if (path.empty())
            continue;
        if (paths)
            paths->emplace_back(path);
        if (end < rest.size() && rest[end] == ' ')
            ++end;
        text.append(prompt.substr(i, at - i));
        i = at + 1 + end;
    }
    text.append(prompt.substr(std::min(i, prompt.size())));
    const std::string_view trimmed = str::trimSpace(text);
    return trimmed == kFilesOnly ? std::string() : std::string(trimmed);
}

// ── Prompts ─────────────────────────────────────────────────────────────────

std::string typedPrompt(std::string_view prompt, std::string *relayTo) {
    std::string text = withoutTeammateNote(prompt); // msga's own addition to what was typed
    // "The user replied in the thread of subagent <id>. Pass their message on
    // to it verbatim with SendMessage (to: "<id>"):\n\n<reply>"
    constexpr std::string_view kHead = "The user replied in the thread of subagent ";
    const std::string_view     t(text);
    if (!str::startsWith(t, kHead))
        return text;
    size_t j = kHead.size();
    while (j < t.size() && ((t[j] >= 'A' && t[j] <= 'Z') || (t[j] >= 'a' && t[j] <= 'z') ||
                            isDigit(t[j]) || t[j] == '_' || t[j] == '-'))
        ++j;
    const std::string_view id = t.substr(kHead.size(), j - kHead.size());
    if (id.empty())
        return text;
    const std::string mid = str::concat(
        {". Pass their message on to it verbatim with SendMessage (to: \"", id, "\"):\n\n"}
    );
    if (t.substr(j, mid.size()) != mid || j + mid.size() >= t.size())
        return text;
    if (relayTo)
        *relayTo = id;
    return std::string(t.substr(j + mid.size()));
}

std::string promptOfRecord(const json::Value &o) {
    // As TranscriptParser::handleUser reads a prompt, less what it keeps
    // track of besides.
    if (o["isMeta"].boolean() || o["isCompactSummary"].boolean())
        return {};
    const json::Value origin = o["origin"];
    if (origin.isObject() && origin["kind"].str() != "human")
        return {};
    const json::Value content = o["message"]["content"];
    std::string       text;
    if (content.isString()) {
        text = content.str();
    } else {
        for (const json::Value b : content) {
            const std::string_view bt = b["type"].str();
            if (bt == "tool_result")
                return {};
            if (bt == "text") {
                if (!text.empty())
                    text += '\n';
                text += b["text"].str();
            }
        }
    }
    if (std::string output; commandOutput(text, &output))
        return {};
    return takeAttachments(quotePastes(typedPrompt(cleanPrompt(text))), nullptr);
}

std::string subagentReplyPrompt(std::string_view agentId, std::string_view reply) {
    return str::concat(
        {"The user replied in the thread of subagent ",
         agentId,
         ". Pass their message on to it verbatim with SendMessage (to: \"",
         agentId,
         "\"):\n\n",
         reply}
    );
}

namespace {

// A history entry of one folder: whose, and the prompt as it comes back.
struct HistoryEntry {
    std::string sessionId, text;
};

// Folder `dir`'s entries in `data` (lines of history.jsonl), oldest first,
// appended to *out.
void historyOf(
    std::string_view           data,
    std::string_view           pasteDir,
    const std::string         &dir,
    std::vector<HistoryEntry> *out
) {
    // A line of the folder's has its name in it, as JSON writes it: lines
    // without are never parsed (most, in a history of many folders).
    std::string            needle;
    const std::string_view name = file::baseName(dir);
    if (!name.empty() && name.find_first_of("\"\\") == std::string_view::npos &&
        std::none_of(name.begin(), name.end(), [](char c) { return uint8_t(c) < 0x20; }))
        needle = name;
    json::Document doc;
    str::Splitter  lines(data, '\n');
    for (std::string_view line; lines.next(&line);) {
        if (line.find("\"display\"") == std::string_view::npos ||
            (!needle.empty() && line.find(needle) == std::string_view::npos))
            continue; // skip the parse for what can't be an entry of the folder
        if (!doc.parse(line, nullptr))
            continue;
        const json::Value o = doc.root();
        if (cleanPath(o["project"].str()) != dir)
            continue;
        std::string text(o["display"].str());
        // A long paste shows as "[Pasted text #3 +24 lines]"; its text is in
        // the entry, or (newer) in paste-cache/<contentHash>.txt.
        for (const json::Value p : o["pastedContents"]) {
            if (p["type"].str() != "text")
                continue;
            std::string content(p["content"].str());
            if (content.empty()) {
                const std::string_view hash = p["contentHash"].str();
                if (hash.empty() ||
                    !file::readAll(str::concat({pasteDir, "/", hash, ".txt"}), &content))
                    continue; // leave the placeholder: better than a silent hole
            }
            // "[Pasted text #<key>]" or "[Pasted text #<key> +<n> lines]".
            const std::string head = str::concat({"[Pasted text #", p.key()});
            for (size_t at = text.find(head); at != std::string::npos;
                 at        = text.find(head, at + 1)) {
                size_t j = at + head.size();
                if (text.compare(j, 2, " +") == 0) {
                    size_t k = j + 2;
                    while (k < text.size() && isDigit(text[k]))
                        ++k;
                    if (k > j + 2 && text.compare(k, 6, " lines") == 0)
                        j = k + 6;
                }
                if (j < text.size() && text[j] == ']') {
                    text.replace(at, j + 1 - at, content);
                    break;
                }
            }
        }
        // A pasted image can't come back as text, and "[Image #1]" alone would
        // only confuse the next prompt.
        text = std::string(str::trimSpace(typedPrompt(dropImageMarks(text))));
        if (!text.empty())
            out->push_back({std::string(o["sessionId"].str()), std::move(text)});
    }
}

// What history.jsonl said of the folders asked for lately. The file only
// grows (a line per prompt, after every one): each folder's entries are
// taken as far as its last whole line, and only what was appended since is
// read then — the whole file again only for a folder new here, or once it
// was rewritten. On a worker too (warmPromptHistory).
struct FolderHistory {
    std::string               folder;
    int64_t                   offset = 0; // past the last whole line taken
    int64_t                   mtime  = -1;
    std::string               lastBytes; // up to 64 bytes before `offset`, as read
    std::vector<HistoryEntry> entries;
    bool                      read = false; // the whole file was read once
};
struct HistoryCache {
    std::mutex                 lock;
    std::string                path, pasteDir;
    std::vector<FolderHistory> folders; // the one asked for last, last
};

HistoryCache &historyCache() {
    static HistoryCache cache;
    return cache;
}

// Folders kept: the sessions' a user goes between.
constexpr size_t kHistoryFolders = 8;

// promptHistory; with `loadedOnly`, only for a folder read before, and
// without waiting for a worker holding the cache: false then.
bool historyFor(
    std::string_view          historyPath,
    std::string_view          pasteDir,
    std::string_view          project,
    std::string_view          sessionId,
    int                       max,
    bool                      loadedOnly,
    std::vector<std::string> *answer
) {
    file::Stat st;
    if (!file::stat(historyPath, &st))
        return !loadedOnly; // no file: nothing to wait for either way
    const std::string            dir   = cleanPath(project);
    HistoryCache                &cache = historyCache();
    std::unique_lock<std::mutex> hold(cache.lock, std::defer_lock);
    if (!loadedOnly)
        hold.lock();
    else if (!hold.try_lock())
        return false;
    if (cache.path != historyPath || cache.pasteDir != pasteDir) {
        if (loadedOnly)
            return false;
        cache.path     = historyPath;
        cache.pasteDir = pasteDir;
        cache.folders.clear();
    }
    auto f = std::find_if(cache.folders.begin(), cache.folders.end(), [&](const auto &h) {
        return h.folder == dir;
    });
    if (loadedOnly && (f == cache.folders.end() || !f->read))
        return false;
    if (f == cache.folders.end()) {
        if (cache.folders.size() >= kHistoryFolders)
            cache.folders.erase(cache.folders.begin());
        cache.folders.push_back({dir});
    } else {
        std::rotate(f, f + 1, cache.folders.end());
    }
    FolderHistory &h = cache.folders.back();
    // What was appended since, after the bytes the last look ended on — a
    // file shorter than that, or with something else there, was rewritten.
    std::string    data;
    if (h.offset > 0 && (st.size != h.offset || st.mtimeMicros != h.mtime)) {
        const size_t seen = h.lastBytes.size();
        if (st.size < h.offset ||
            !file::readRange(
                historyPath, h.offset - int64_t(seen), size_t(st.size - h.offset) + seen, &data
            ) ||
            data.compare(0, seen, h.lastBytes) != 0) {
            h.offset = 0;
            h.entries.clear();
            h.lastBytes.clear();
            h.read = false;
            data.clear();
        } else {
            data.erase(0, seen);
        }
    }
    if (h.offset == 0) { // the whole file: a worker's job when loadedOnly
        if (loadedOnly && !h.read)
            return false;
        if (!file::readAll(historyPath, &data))
            return true;
    }
    h.read             = true;
    h.mtime            = st.mtimeMicros;
    // Whole lines are kept; a last one still being written is read for this
    // answer alone.
    const size_t whole = data.rfind('\n') + 1; // 0 when there's none
    historyOf(std::string_view(data).substr(0, whole), pasteDir, dir, &h.entries);
    h.offset += int64_t(whole);
    constexpr size_t kLastBytes = 64;
    const size_t     from       = whole > kLastBytes ? whole - kLastBytes : 0;
    h.lastBytes.append(data, from, whole - from);
    if (h.lastBytes.size() > kLastBytes)
        h.lastBytes.erase(0, h.lastBytes.size() - kLastBytes);
    std::vector<HistoryEntry> partial;
    if (whole < data.size())
        historyOf(std::string_view(data).substr(whole), pasteDir, dir, &partial);
    std::vector<std::string> own, others; // oldest first
    for (const std::vector<HistoryEntry> *part : {&h.entries, &partial})
        for (const HistoryEntry &e : *part)
            (!sessionId.empty() && e.sessionId == sessionId ? own : others).push_back(e.text);
    std::vector<std::string> &out = *answer;
    for (const std::vector<std::string> *part : {&own, &others}) {
        for (auto it = part->crbegin(); it != part->crend() && int(out.size()) < max; ++it)
            if (out.empty() || out.back() != *it) // the same prompt twice in a row: once
                out.push_back(*it);
    }
    return true;
}

} // namespace

std::vector<std::string> promptHistory(
    std::string_view historyPath,
    std::string_view pasteDir,
    std::string_view project,
    std::string_view sessionId,
    int              max
) {
    std::vector<std::string> out;
    historyFor(historyPath, pasteDir, project, sessionId, max, false, &out);
    return out;
}

bool loadedPromptHistory(
    std::string_view          historyPath,
    std::string_view          pasteDir,
    std::string_view          project,
    std::string_view          sessionId,
    std::vector<std::string> *out,
    int                       max
) {
    out->clear();
    return historyFor(historyPath, pasteDir, project, sessionId, max, true, out);
}

// ── Rewriting the transcript ────────────────────────────────────────────────

namespace {

// Thinking only: the reasoning half of an answer, written as a record of its own.
bool isThinkingRecord(const json::Value &o) {
    if (o["type"].str() != "assistant")
        return false;
    const json::Value blocks = o["message"]["content"];
    if (!blocks.isArray() || blocks.size() == 0)
        return false;
    for (const json::Value v : blocks) {
        const std::string_view bt = v["type"].str();
        if (bt != "thinking" && bt != "redacted_thinking")
            return false;
    }
    return true;
}

// Where the next turn starts: a turn's end, or a prompt someone typed.
bool endsTurn(const json::Value &o) {
    const std::string_view type = o["type"].str();
    if (type == "system")
        return o["subtype"].str() == "turn_duration";
    if (type != "user" || o["isMeta"].boolean())
        return false;
    const json::Value content = o["message"]["content"];
    if (content.isString())
        return true;
    for (const json::Value v : content)
        if (v["type"].str() == "tool_result")
            return false;
    return true;
}

} // namespace

bool hasTurnSince(std::string_view path, int64_t from, int64_t afterMs) {
    LineReader     lines(path, from);
    RecordSkim     skim;
    json::Document doc;
    for (std::string_view line; lines.next(&line);) {
        std::string_view type, timestamp;
        if (line.empty())
            continue;
        if (skimRecord(line, &skim)) {
            type      = skim.type;
            timestamp = skim.timestamp;
        } else if (doc.parse(line, nullptr)) {
            type      = doc.root()["type"].str();
            timestamp = doc.root()["timestamp"].str();
        } else {
            continue;
        }
        if (type != "user" && type != "assistant")
            continue;
        if (afterMs <= 0)
            return true;
        const int64_t at = base::parseIsoMicros(timestamp);
        if (at > 0 && at / 1000 > afterMs)
            return true;
    }
    return false;
}

namespace {

// What a line of a transcript says, read from the end: its record's uuid,
// and whether it's a prompt or an answer.
struct LineRecord {
    std::string uuid;
    bool        turn = false; // a user or assistant record
};

// Each line of the file at `path`, last to first (one still being written
// too), into `fn` until it returns false: 64 KB read at a time from the end,
// a line's bytes kept only until it's whole. False when it can't be read.
bool readBackwards(std::string_view path, const std::function<bool(const LineRecord &)> &fn) {
    constexpr int64_t kChunk = 64 * 1024;
    int64_t           pos    = file::size(path);
    if (pos < 0)
        return false;
    std::string    carry; // a line's end, its start still to be read
    RecordSkim     skim;
    json::Document doc;
    while (pos > 0) {
        const int64_t from = std::max<int64_t>(0, pos - kChunk);
        std::string   chunk;
        if (!file::readRange(path, from, size_t(pos - from), &chunk))
            return false;
        pos        = from;
        carry      = chunk + carry;
        // Every line in it, last first — but the first, whose start may be
        // in the bytes before (unless they're the file's start).
        size_t end = carry.size();
        for (;;) {
            const size_t nl = end ? carry.rfind('\n', end - 1) : std::string::npos;
            if (nl == std::string::npos && pos > 0)
                break;
            const size_t           start = nl == std::string::npos ? 0 : nl + 1;
            const std::string_view line  = std::string_view(carry).substr(start, end - start);
            std::string_view       type, uuid;
            bool                   read = false;
            if (end > start && skimRecord(line, &skim)) {
                type = skim.type;
                uuid = skim.uuid;
                read = true;
            } else if (end > start && doc.parse(line, nullptr) && doc.root().isObject()) {
                type = doc.root()["type"].str();
                uuid = doc.root()["uuid"].str();
                read = true;
            }
            if (read && !fn({std::string(uuid), type == "user" || type == "assistant"}))
                return true;
            if (nl == std::string::npos)
                break;
            end = nl;
        }
        carry.resize(end);
    }
    return true;
}

} // namespace

bool hasTurnAfter(std::string_view path, std::string_view uuid) {
    bool after = true; // the record not found: nothing says the session didn't go on
    if (uuid.empty())
        return after;
    readBackwards(path, [&](const LineRecord &r) {
        if (r.uuid == uuid) {
            after = false;
            return false;
        }
        // A turn before the record turns up: whether the record is further
        // back or not there at all, the answer is yes.
        return !r.turn;
    });
    return after;
}

std::vector<std::string> recordsBefore(std::string_view path, std::string_view uuid, size_t max) {
    std::vector<std::string> out;
    bool                     found = false;
    if (uuid.empty() || max == 0)
        return out;
    readBackwards(path, [&](const LineRecord &r) {
        if (!found) {
            found = r.uuid == uuid;
            return true;
        }
        if (!r.uuid.empty())
            out.push_back(r.uuid);
        return out.size() < max;
    });
    return out;
}

bool removeFromTranscript(std::string_view path, std::string_view uuid, std::string *error) {
    auto fail = [error](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    std::string data;
    if (uuid.empty() || !file::readAll(path, &data))
        return fail(str::concat({"can't read ", path}));
    std::vector<std::string_view> lines = str::split(data, '\n');
    if (lines.back().empty())
        lines.pop_back(); // after the last newline
    // Never every line's document at once (a long transcript's would take
    // several times its size): one is parsed at a time, and each line keeps
    // only what the rewrite asks of it. A line that isn't JSON is an empty
    // record.
    json::Document doc;
    RecordSkim     skim;
    const auto     parse = [&doc](std::string_view line) {
        if (!doc.parse(line, nullptr) || !doc.root().isObject())
            doc = json::Document();
        return doc.root();
    };
    int target = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if ((skimRecord(lines[i], &skim) ? skim.uuid : parse(lines[i])["uuid"].str()) == uuid)
            target = int(i);
    if (target < 0)
        return fail(str::concat({"no record ", uuid, " in ", path}));
    // The record taken out: its type, its message's id, a prompt's text.
    json::Document targetDoc;
    targetDoc.parse(lines[size_t(target)], nullptr);
    const json::Value t      = targetDoc.root();
    const bool        answer = t["type"].str() == "assistant";
    const std::string messageId(t["message"]["id"].str());
    const json::Value promptText = t["message"]["content"];
    const bool        bookkept   = t["type"].str() == "user" && promptText.isString();

    // What goes, each with the parent that records linked to it are moved to.
    constexpr std::string_view kLinks[] = {"parentUuid", "logicalParentUuid", "leafUuid"};
    struct Line {
        std::string uuid, links[3]; // links: as kLinks
    };
    std::vector<Line>                            info(lines.size());
    std::unordered_map<std::string, std::string> gone;
    std::vector<bool>                            dropped(lines.size(), false);
    auto                                         drop = [&](size_t i) {
        dropped[i] = true;
        if (!info[i].uuid.empty())
            gone[info[i].uuid] = info[i].links[0];
    };
    info[size_t(target)].uuid = std::string(t["uuid"].str());
    for (size_t k = 0; k < 3; ++k)
        info[size_t(target)].links[k] = std::string(t[kLinks[k]].str());
    drop(size_t(target));
    bool inTurn = true; // a prompt: the turn it started, up to its end
    for (size_t i = 0; i < lines.size(); ++i) {
        const json::Value r = parse(lines[i]);
        info[i].uuid        = std::string(r["uuid"].str());
        for (size_t k = 0; k < 3; ++k)
            info[i].links[k] = std::string(r[kLinks[k]].str());
        if (answer) {
            // An answer: the thinking written as part of the same message.
            if (!messageId.empty() && isThinkingRecord(r) && r["message"]["id"].isString() &&
                r["message"]["id"].str() == messageId)
                drop(i);
        } else if (i > size_t(target) && inTurn) {
            // A prompt: the thinking of the turn it started.
            if (endsTurn(r))
                inTurn = false;
            else if (isThinkingRecord(r))
                drop(i);
        }
        // Claude Code's own bookkeeping keeps a typed prompt's text as well
        // (its input queue, the last prompt for `claude --resume`): none of it
        // reaches Claude, but a deleted message shouldn't linger in the file.
        if (bookkept) {
            const std::string_view type = r["type"].str();
            if ((type == "queue-operation" && r["content"].isString() &&
                 r["content"].str() == promptText.str()) ||
                (type == "last-prompt" && r["lastPrompt"].isString() &&
                 r["lastPrompt"].str() == promptText.str()))
                dropped[i] = true;
        }
    }
    auto relink = [&gone](std::string id) {
        for (size_t hops = 0; hops < gone.size(); ++hops) {
            const auto it = gone.find(id);
            if (it == gone.end())
                break;
            id = it->second;
        }
        return id;
    };

    std::string out;
    out.reserve(data.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (dropped[i] || gone.count(info[i].uuid))
            continue;
        bool changed = false;
        for (const std::string &link : info[i].links)
            if (!link.empty() && gone.count(link))
                changed = true;
        if (!changed) {
            out.append(lines[i]); // untouched lines are kept byte for byte
            out += '\n';
            continue;
        }
        const json::Value o = parse(lines[i]);
        json::Writer      w;
        w.beginObject();
        for (const json::Value v : o) {
            const std::string_view key = v.key();
            const bool             link =
                std::find(std::begin(kLinks), std::end(kLinks), key) != std::end(kLinks);
            w.key(key);
            if (link && !v.str().empty() && gone.count(std::string(v.str()))) {
                const std::string to = relink(std::string(v.str()));
                if (to.empty())
                    w.null();
                else
                    w.value(to);
            } else {
                w.value(v);
            }
        }
        w.endObject();
        out += w.str();
        out += '\n';
    }
    // In place, not by replacing the file: a writer holding it open keeps
    // appending to the same file.
    if (!file::overwrite(path, out))
        return fail(str::concat({"can't write ", path}));
    return true;
}

} // namespace claude
