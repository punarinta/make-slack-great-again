#include "app/claude/relay_protocol.h"

#include "base/crypto.h"
#include "base/str.h"

namespace claude::relay {

namespace {

// Table-driven: one loop per struct rather than a call per field.
template <class T>
struct IntField {
    const char *key;
    int64_t T::*field;
};

constexpr json::StrField<SessionInfo> kSessionStrings[] = {
    {"id", &SessionInfo::sessionId},
    {"name", &SessionInfo::name},
    {"cwd", &SessionInfo::cwd},
    {"status", &SessionInfo::status},
    {"entry", &SessionInfo::entrypoint},
    {"needs", &SessionInfo::needs},
    {"reply", &SessionInfo::suggestedReply},
    {"transcript", &SessionInfo::transcriptPath},
    {"worker", &SessionInfo::workerStatus},
    {"job", &SessionInfo::jobId},
    {"worktree", &SessionInfo::worktreePath},
};
constexpr json::BoolField<SessionInfo> kSessionBools[] = {
    {"running", &SessionInfo::running},
    {"approval", &SessionInfo::awaitsApproval},
    {"hook", &SessionInfo::permissionHook},
    {"commands", &SessionInfo::commandsRunning},
};
constexpr IntField<SessionInfo> kSessionInts[] = {
    {"pid", &SessionInfo::pid},
    {"since", &SessionInfo::statusSinceMs},
};

constexpr json::StrField<CatalogEntry> kCatalogStrings[] = {
    {"id", &CatalogEntry::sessionId},
    {"transcript", &CatalogEntry::transcriptPath},
    {"cwd", &CatalogEntry::cwd},
    {"title", &CatalogEntry::title},
    {"first", &CatalogEntry::firstPrompt},
    {"last", &CatalogEntry::lastPrompt},
    {"role", &CatalogEntry::role},
    {"roleName", &CatalogEntry::roleName},
};

constexpr json::StrField<SlashCommand> kCommandStrings[] = {
    {"name", &SlashCommand::name},
    {"desc", &SlashCommand::desc},
    {"usage", &SlashCommand::usage},
    {"source", &SlashCommand::source},
};

constexpr json::StrField<Account> kAccountStrings[] = {
    {"email", &Account::email},
    {"org", &Account::organization},
    {"sub", &Account::subscriptionType},
    {"api", &Account::apiProvider},
};

template <class T, size_t N>
void writeStrs(json::Writer &w, const T &obj, const json::StrField<T> (&fields)[N]) {
    for (const auto &f : fields)
        if (!(obj.*f.field).empty())
            w.key(f.key).value(obj.*f.field);
}

} // namespace

std::string helloLine(const Hello &h, std::string_view extraJson) {
    json::Writer w;
    w.beginObject().key("t").value("hello");
    w.key("proto").value(h.protocol).key("min").value(h.protocolMin);
    w.key("msga").value(h.msgaVersion);
    w.key("caps");
    writeStrings(w, h.caps);
    if (!h.home.empty())
        w.key("home").value(h.home);
    if (!h.os.empty())
        w.key("os").value(h.os);
    w.key("claude").value(h.claude);
    if (!h.error.empty())
        w.key("error").value(h.error);
    w.endObject();
    std::string line = w.take();
    // The caller's own members (msga's mirror state) go in before the end.
    if (!extraJson.empty()) {
        line.pop_back();
        line.append(",").append(extraJson).append("}");
    }
    return line;
}

bool readHello(const json::Value &v, Hello *out) {
    if (v["t"].str() != "hello")
        return false;
    out->protocol    = int(v["proto"].integer());
    out->protocolMin = int(v["min"].integer(out->protocol));
    out->msgaVersion = v["msga"].integer();
    out->caps        = readStrings(v["caps"]);
    out->home        = json::owned(v["home"]);
    out->os          = json::owned(v["os"]);
    out->claude      = v["claude"].boolean();
    out->error       = json::owned(v["error"]);
    return out->protocol > 0;
}

int agreedProtocol(const Hello &a, const Hello &b) {
    const int top = std::min(a.protocol, b.protocol);
    return top >= a.protocolMin && top >= b.protocolMin ? top : 0;
}

int64_t msgaVersion() {
#ifdef MSGA_VERSION
    return MSGA_VERSION;
#else
    return 0;
#endif
}

std::string osName() {
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#elif defined(_WIN32)
    return "windows";
#else
    return "unix";
#endif
}

void writeSession(json::Writer &w, const SessionInfo &s) {
    w.beginObject();
    writeStrs(w, s, kSessionStrings);
    if (s.kind == SessionInfo::Kind::Background)
        w.key("bg").value(true);
    for (const auto &f : kSessionBools)
        if (s.*f.field)
            w.key(f.key).value(true);
    for (const auto &f : kSessionInts)
        if (s.*f.field)
            w.key(f.key).value(s.*f.field);
    w.endObject();
}

void readSession(const json::Value &v, SessionInfo *s) {
    json::readStrings(v, *s, kSessionStrings);
    json::readBools(v, *s, kSessionBools);
    for (const auto &f : kSessionInts)
        (*s).*f.field = v[f.key].integer();
    s->kind = v["bg"].boolean() ? SessionInfo::Kind::Background : SessionInfo::Kind::Interactive;
}

void writeQuestion(json::Writer &w, const HookQuestion &q) {
    w.beginObject().key("id").value(q.id).key("session").value(q.sessionId);
    w.key("options").beginArray();
    for (const auto &o : q.options)
        w.beginObject().key("label").value(o.label).key("answer").value(o.answer).endObject();
    w.endArray().endObject();
}

void readQuestion(const json::Value &v, HookQuestion *q) {
    q->id        = json::owned(v["id"]);
    q->sessionId = json::owned(v["session"]);
    q->options.clear();
    for (const json::Value o : v["options"])
        q->options.push_back({json::owned(o["label"]), json::owned(o["answer"])});
}

void writePermissionQuestion(json::Writer &w, const PermissionQuestion &q) {
    w.beginObject().key("text").value(q.text).key("selected").value(q.selected);
    w.key("options").beginArray();
    for (const auto &o : q.options)
        w.beginObject().key("n").value(o.number).key("label").value(o.label).endObject();
    w.endArray().endObject();
}

void readPermissionQuestion(const json::Value &v, PermissionQuestion *q) {
    q->text     = json::owned(v["text"]);
    q->selected = int(v["selected"].integer());
    q->options.clear();
    for (const json::Value o : v["options"])
        q->options.push_back({int(o["n"].integer()), json::owned(o["label"])});
}

void writeCommand(json::Writer &w, const SlashCommand &c) {
    w.beginObject();
    writeStrs(w, c, kCommandStrings);
    if (c.local)
        w.key("local").value(true);
    w.endObject();
}

void readCommand(const json::Value &v, SlashCommand *c) {
    json::readStrings(v, *c, kCommandStrings);
    c->local = v["local"].boolean();
}

void writeAccount(json::Writer &w, const Account &a) {
    w.beginObject();
    writeStrs(w, a, kAccountStrings);
    w.endObject();
}

void readAccount(const json::Value &v, Account *a) {
    json::readStrings(v, *a, kAccountStrings);
}

void writeCatalogEntry(json::Writer &w, const CatalogEntry &e) {
    w.beginObject();
    writeStrs(w, e, kCatalogStrings);
    w.key("modified").value(e.modifiedMs);
    w.endObject();
}

void readCatalogEntry(const json::Value &v, CatalogEntry *e) {
    json::readStrings(v, *e, kCatalogStrings);
    e->modifiedMs = v["modified"].integer();
}

void writeStrings(json::Writer &w, const std::vector<std::string> &v) {
    w.beginArray();
    for (const auto &s : v)
        w.value(s);
    w.endArray();
}

std::vector<std::string> readStrings(const json::Value &v) {
    std::vector<std::string> out;
    for (const json::Value s : v)
        out.emplace_back(s.str());
    return out;
}

std::string_view loginWord(Login l) {
    switch (l) {
    case Login::In:
        return "in";
    case Login::Out:
        return "out";
    case Login::Unknown:
        break;
    }
    return "unknown";
}

Login parseLogin(std::string_view s) {
    return s == "in" ? Login::In : s == "out" ? Login::Out : Login::Unknown;
}

std::string_view inputOutcomeWord(AttachInput::Outcome o) {
    switch (o) {
    case AttachInput::Outcome::Sent:
        return "sent";
    case AttachInput::Outcome::Failed:
        return "failed";
    case AttachInput::Outcome::Unconfirmed:
        return "unconfirmed";
    case AttachInput::Outcome::NotReady:
        break;
    }
    return "notready";
}

AttachInput::Outcome parseInputOutcome(std::string_view s) {
    // Unknown words are "nothing typed": the safe one — the message waits.
    return s == "sent"          ? AttachInput::Outcome::Sent
           : s == "failed"      ? AttachInput::Outcome::Failed
           : s == "unconfirmed" ? AttachInput::Outcome::Unconfirmed
                                : AttachInput::Outcome::NotReady;
}

std::string_view answerOutcomeWord(AttachAnswer::Outcome o) {
    switch (o) {
    case AttachAnswer::Outcome::Done:
        return "done";
    case AttachAnswer::Outcome::Failed:
        return "failed";
    case AttachAnswer::Outcome::NotReady:
        break;
    }
    return "notready";
}

AttachAnswer::Outcome parseAnswerOutcome(std::string_view s) {
    return s == "done"     ? AttachAnswer::Outcome::Done
           : s == "failed" ? AttachAnswer::Outcome::Failed
                           : AttachAnswer::Outcome::NotReady;
}

bool mirrorPathOk(std::string_view rel) {
    if (rel.empty() || rel.size() > 1024 || rel.front() == '/' || rel.front() == '\\')
        return false;
    for (std::string_view part : str::split(rel, '/'))
        if (part.empty() || part == "." || part == ".." ||
            part.find_first_of(std::string_view("\\:\0", 3)) != std::string_view::npos)
            return false;
    return rel == "history.jsonl" || str::startsWith(rel, "projects/") ||
           str::startsWith(rel, "jobs/");
}

uint64_t tailHash(std::string_view bytes) {
    if (bytes.size() > kTailHashBytes)
        bytes = bytes.substr(bytes.size() - kTailHashBytes);
    return crypto::fnv1a(bytes);
}

} // namespace claude::relay
