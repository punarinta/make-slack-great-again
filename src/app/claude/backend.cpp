// The reading half of the Claude Code backend: sessions and their state, the
// roster scans, transcripts into the Store, typing, history and threads, the
// timers (see backend.h).
#include "app/claude/backend.h"

#include "app/claude/backend_internal.h"
#include "app/claude/common.h"
#include "app/claude/outputs.h"
#include "app/claude/render.h"
#include "app/model/jobs.h"
#include "app/model/timers.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstring>
#include <utility>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace claude {

using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Message;
using model::Ts;
using model::UserRef;

// ── Shared helpers (backend_internal.h) ─────────────────────────────────────

int64_t nowMs() {
    return base::nowMicros() / 1000;
}

std::string loginName() {
    std::string n = base::env("USER");
    if (n.empty())
        n = base::env("USERNAME");
    return n.empty() ? std::string(tr("You")) : n;
}

bool awaitsApproval(const SessionInfo &s) {
    return s.kind == SessionInfo::Kind::Background && s.awaitsApproval && s.running;
}

bool needsLogin(const SessionInfo &s) {
    return str::startsWith(s.needs, "login required");
}

namespace {
uint64_t mix(uint64_t h, std::string_view s) {
    return (crypto::fnv1a(s, h) ^ 0xff) * 1099511628211ull; // a field's end: "ab"+"c" ≠ "a"+"bc"
}
uint64_t mix(uint64_t h, int64_t v) {
    return mix(h, std::string_view(reinterpret_cast<const char *>(&v), sizeof v));
}
} // namespace

uint64_t fingerprint(
    uint64_t                            content,
    Ts                                  ts,
    Ts                                  threadTs,
    uint32_t                            replyCount,
    Ts                                  latestReply,
    const std::vector<model::Reaction> &reactions
) {
    uint64_t h = content;
    h          = mix(h, ts);
    h          = mix(h, threadTs);
    h          = mix(h, int64_t(replyCount));
    h          = mix(h, latestReply);
    for (const auto &r : reactions) {
        h = mix(h, r.name);
        h = mix(h, int64_t(r.count));
    }
    return h;
}

uint64_t fingerprint(const Message &m) {
    return fingerprint(
        contentFingerprint(m), m.ts, m.threadTs, m.replyCount, m.latestReply, m.reactions
    );
}

uint64_t contentFingerprint(const Message &m) {
    uint64_t h = crypto::kFnvOffset;
    h          = mix(h, int64_t(m.user));
    h          = mix(h, m.text);
    h          = mix(h, int64_t(m.pending));
    if (m.extra) {
        h = mix(h, m.extra->subtype);
        for (const auto &f : m.extra->files)
            h = mix(h, f.path);
        for (const auto &a : m.extra->attachments) {
            h = mix(h, a.title);
            h = mix(h, a.text);
        }
        for (const auto &b : m.extra->buttons) {
            h = mix(h, b.id);
            h = mix(h, b.label);
        }
    }
    return h;
}

size_t sharedStart(const std::vector<TranscriptItem> &a, const std::vector<TranscriptItem> &b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n].ts == b[n].ts && a[n].kind == b[n].kind)
        ++n;
    return n;
}

std::string knownSessionsPath() {
    return dirs().data + "/known-sessions.json";
}

std::string profilePath() {
    return dirs().data + "/profile.json";
}

// ── Construction ────────────────────────────────────────────────────────────

Backend::Backend(model::Store &store, plat::App &app, Credentials creds)
    : model::Backend(store), _app(app), _creds(std::move(creds)), _paths(Paths::detect()),
      _team(dirs().data + "/team"), _alive(std::make_shared<bool>(true)) {
    _launcher = std::make_unique<Launcher>(_app, _creds.claudePath, _paths);
    loadKnown();
    loadProfile();
}

Backend::~Backend() {
    close();
}

void Backend::close() {
    if (!*_alive)
        return;
    saveKnown();
    *_alive = false;
    for (uint64_t *t : {&_debounce, &_watchTimer, &_safetyPoll, &_saveTimer, &_typingTimer}) {
        if (*t)
            _app.cancelTimer(*t);
        *t = 0;
    }
    _oneShots.cancelAll();
    if (_inotifyWatch)
        _app.unwatchFd(std::exchange(_inotifyWatch, 0));
#if defined(__linux__)
    if (_inotify >= 0)
        ::close(_inotify);
#endif
    _inotify = -2;
    for (auto &[id, t] : _sessions) {
        if (const auto typing = std::exchange(t->typing, {}).lock())
            typing->cancel(); // its done finds the backend gone
        t->answering.reset();
    }
}

void Backend::post(std::function<void()> fn) {
    model::postWhileAlive(_app, _alive, std::move(fn));
}

void Backend::after(int ms, std::function<void()> fn) {
    _oneShots.after(ms, std::move(fn)); // close() cancels the pending ones
}

Backend::Capabilities Backend::capabilities() const {
    Capabilities c;
    c.presence       = true;  // a session's dot: working vs not
    c.selfStatus     = false; // nothing to be "away" from
    c.profileContact = false; // your profile is a name and a picture, nothing more
    c.agentSessions  = true;
    c.zenMode        = true; // hides the tool-call cards
    c.slashCommands  = true; // Claude Code's own, typed to it (commands())
    c.customSections = true; // kept with the known sessions
    // fileUpload stays off: Forward sends a message's files as links.
    return c;
}

// ── Known sessions ──────────────────────────────────────────────────────────
// Ended interactive sessions are in no Claude Code registry — only their
// transcript remains — so msga remembers the sessions it has seen, and forgets
// each one once Claude Code has dropped it.

void Backend::loadKnown() {
    json::Document doc;
    if (!doc.parseFile(knownSessionsPath()))
        return;
    const json::Value root = doc.root();
    for (const json::Value o : root["sessions"]) {
        const std::string convId(o["id"].str());
        std::string       sid(o["sessionId"].str());
        if (sid.empty() && !str::startsWith(convId, kNewPrefix))
            sid = convId; // written before conversation ids could differ
        if (convId.empty() || sid.empty())
            continue;
        Tracked &t        = ensureTracked(convId);
        t.info.sessionId  = sid;
        _convOf[sid]      = convId;
        t.info.name       = o["name"].str();
        t.info.cwd        = o["cwd"].str();
        t.info.kind       = o["kind"].str() == "background" ? SessionInfo::Kind::Background
                                                            : SessionInfo::Kind::Interactive;
        t.info.entrypoint = o["entrypoint"].str();
        t.transcriptPath  = o["transcript"].str();
        t.lastRead        = model::parseTs(o["lastRead"].str());
        t.localName       = o["localName"].str();
        t.standalone      = o["standalone"].boolean();
        t.role            = o["role"].str();
        t.starred         = o["starred"].boolean();
        t.muted           = o["muted"].boolean();
        t.notify          = model::NotifyLevel(std::clamp<int64_t>(o["notify"].integer(), 0, 3));
        for (const json::Value v : o["deniedTools"])
            t.deniedTools.emplace_back(v.str());
        if (str::startsWith(convId, kNewPrefix))
            _launchedHere.insert(sid); // a "+" session, remembered before `started` was
    }
    for (const json::Value v : root["started"])
        if (!v.str().empty())
            _launchedHere.insert(std::string(v.str()));
    for (const json::Value o : root["hidden"]) {
        const std::string sid(o["sessionId"].str());
        if (sid.empty() || _convOf.count(sid))
            continue;
        Hidden h;
        h.atMs       = int64_t(o["at"].number());
        h.transcript = o["transcript"].str();
        h.seenSize   = int64_t(o["size"].number(-1));
        _hidden[sid] = std::move(h);
    }
    _lastConv = root["open"].str();
    for (const json::Value o : root["sections"]) {
        model::Store::Section sec;
        sec.id    = o["id"].str();
        sec.name  = o["name"].str();
        sec.emoji = o["emoji"].str();
        for (const json::Value v : o["convs"])
            sec.convs.emplace_back(v.str());
        if (!sec.id.empty())
            _sections.push_back(std::move(sec));
    }
}

void Backend::saveKnown() {
    // Removed and started sessions are forgotten once Claude Code has dropped
    // them: their transcript and, for a background session, its job both
    // gone. Hundreds of them, a few stats each: looked at once an hour, not
    // on every save.
    constexpr int64_t kPruneMs = 60 * 60'000;
    if (_prunedMs == 0 || nowMs() - _prunedMs >= kPruneMs) {
        _prunedMs        = nowMs();
        const auto jobOf = [this](const std::string &sid) {
            return file::exists(_paths.jobsDir() + "/" + sid.substr(0, 8));
        };
        _dropped.clear();
        for (const auto &[sid, h] : _hidden)
            if ((h.transcript.empty() || !file::exists(h.transcript)) && !jobOf(sid))
                _dropped.insert(sid);
        for (const std::string &sid : _launchedHere)
            if (!_convOf.count(sid) && (!_hidden.count(sid) || _dropped.count(sid)) && !jobOf(sid))
                _dropped.insert(sid);
    }
    json::Writer w;
    w.beginObject();
    w.key("sessions").beginArray();
    for (const auto &[convId, tp] : _sessions) {
        const Tracked &t = *tp;
        if (t.info.sessionId.empty())
            continue; // a "+" session nobody wrote to yet: nothing to come back to
        w.beginObject();
        w.key("id").value(convId);
        w.key("sessionId").value(t.info.sessionId);
        w.key("name").value(titleOf(t));
        w.key("cwd").value(t.info.cwd);
        w.key("kind").value(
            t.info.kind == SessionInfo::Kind::Background ? "background" : "interactive"
        );
        w.key("entrypoint").value(t.info.entrypoint);
        w.key("transcript").value(t.transcriptPath);
        w.key("lastRead").value(t.lastRead ? model::formatTs(t.lastRead) : std::string());
        if (!t.localName.empty())
            w.key("localName").value(t.localName);
        if (t.standalone)
            w.key("standalone").value(true); // a branch opened as a session
        if (const std::string role = roleOf(t); role != kGeneralist)
            w.key("role").value(role);
        if (t.starred)
            w.key("starred").value(true);
        if (t.muted)
            w.key("muted").value(true);
        if (t.notify != model::NotifyLevel::Default)
            w.key("notify").value(int64_t(t.notify));
        if (!t.deniedTools.empty()) { // an agent branch: every turn goes without them
            w.key("deniedTools").beginArray();
            for (const std::string &tool : t.deniedTools)
                w.value(tool);
            w.endArray();
        }
        w.endObject();
    }
    w.endArray();
    w.key("hidden").beginArray();
    for (const auto &[sid, h] : _hidden) {
        if (_dropped.count(sid))
            continue;
        w.beginObject();
        w.key("sessionId").value(sid);
        w.key("at").value(double(h.atMs));
        w.key("transcript").value(h.transcript);
        if (h.seenSize >= 0)
            w.key("size").value(double(h.seenSize));
        w.endObject();
    }
    w.endArray();
    w.key("started").beginArray();
    for (const std::string &sid : _launchedHere)
        if (_convOf.count(sid) || !_dropped.count(sid))
            w.value(sid);
    w.endArray();
    if (!_lastConv.empty())
        w.key("open").value(_lastConv);
    if (!_sections.empty()) { // the sidebar's own sections, forgotten sessions left out
        w.key("sections").beginArray();
        for (const model::Store::Section &sec : _sections) {
            w.beginObject();
            w.key("id").value(sec.id);
            w.key("name").value(sec.name);
            if (!sec.emoji.empty())
                w.key("emoji").value(sec.emoji);
            w.key("convs").beginArray();
            for (const std::string &conv : sec.convs)
                if (_sessions.count(conv))
                    w.value(conv);
            w.endArray();
            w.endObject();
        }
        w.endArray();
    }
    w.endObject();
    if (dirs().data.empty())
        return;
    // Saved after every refresh: written (and synced to disk) only when it
    // says something new.
    const uint64_t h = crypto::fnv1a(w.str());
    if (h == _savedHash)
        return;
    if (file::writeAtomic(knownSessionsPath(), w.str(), 0600))
        _savedHash = h;
}

void Backend::scheduleSaveKnown() {
    if (!_started) {
        saveKnown();
        return;
    }
    if (_saveTimer)
        return;
    _saveTimer = _app.addTimer(2000, false, [this, alive = _alive] {
        if (!*alive)
            return;
        _saveTimer = 0;
        saveKnown();
    });
}

Backend::Tracked &Backend::ensureTracked(const std::string &convId) {
    auto it = _sessions.find(convId);
    if (it == _sessions.end()) {
        it                 = _sessions.emplace(convId, std::make_unique<Tracked>()).first;
        it->second->convId = convId;
    }
    return *it->second;
}

Backend::Tracked *Backend::find(std::string_view convId) {
    const auto it = _sessions.find(convId);
    return it == _sessions.end() ? nullptr : it->second.get();
}

const Backend::Tracked *Backend::find(std::string_view convId) const {
    const auto it = _sessions.find(convId);
    return it == _sessions.end() ? nullptr : it->second.get();
}

Backend::Tracked *Backend::findRef(ConvRef conv) {
    if (conv >= _store.conversationCount())
        return nullptr;
    return find(_store.conversation(conv).id);
}

const Backend::Tracked *Backend::findRef(ConvRef conv) const {
    if (conv >= _store.conversationCount())
        return nullptr;
    return find(_store.conversation(conv).id);
}

std::string Backend::convIdFor(const std::string &sessionId) const {
    const auto it = _convOf.find(sessionId);
    return it == _convOf.end() ? sessionId : it->second;
}

model::ConvRef Backend::lastConversation() const {
    return _lastConv.empty() ? kNoConv : _store.findConversation(_lastConv);
}

// ── Session state ───────────────────────────────────────────────────────────

bool Backend::busy(const Tracked &t) const {
    // The worker is on its way out — or will be as soon as the launch it was
    // stopped during reports back (stopRequested): no dot from the Stop on.
    if (t.stopping || t.stopRequested)
        return false;
    if (t.sending)
        return true; // msga's turn: from launching it until its end is written
    return working(t);
}

bool Backend::working(const Tracked &t) const {
    if (!t.info.running)
        return false;
    if (statusIsBusy(t.info.status))
        return true;
    // A turn that ends on a question leaves the job "blocked", and the reply's
    // turn doesn't flip it back to "working" (seen 2026-09-26: blocked for 20
    // minutes over two turns, nothing in between). A turn still open whose
    // records are newer than that status, its worker busy, is under way.
    return statusIsBusy(t.info.workerStatus) && t.parser.turnOpen() &&
           t.parser.lastActivity() / 1000 > t.info.statusSinceMs;
}

bool Backend::needsUser(const Tracked &t) const {
    return !t.sending && t.info.running && statusNeedsUser(t.info.status) && !needsLogin(t.info) &&
           !working(t);
}

const std::string &Backend::roleOf(const Tracked &t) const {
    static const std::string generalist(kGeneralist);
    if (!t.parser.role().empty())
        return t.parser.role();
    return t.role.empty() ? generalist : t.role;
}

const Role &Backend::roleFor(const Tracked &t) const {
    return _team.resolve(roleOf(t), t.parser.roleName());
}

model::UserRef Backend::userRef(const std::string &id) {
    return _store.internUser(id);
}

model::UserRef Backend::roleUser(const std::string &role) {
    if (role.empty() || role == kGeneralist)
        return userRef(kAgentUser);
    return userRef(kRoleUserPrefix + role);
}

model::UserRef Backend::sessionUser(const Tracked &t) {
    return userRef(kAssistantPrefix + t.convId);
}

model::UserRef Backend::subagentAuthor(const TranscriptItem &item, UserRef parent) {
    if (item.kind != TranscriptItem::Kind::Subagent)
        return parent;
    // A type that isn't a teammate ("claude", "Explore") is no teammate either
    // — not the parent's role — unless its prompt names one (a session with no
    // teammate types spawns them as "claude", see teammateNote).
    if (_team.find(item.agentType))
        return roleUser(item.agentType);
    if (_team.find(item.agentRole))
        return roleUser(item.agentRole);
    return userRef(kAgentUser);
}

std::vector<std::string> Backend::roleIds() const {
    std::vector<std::string> ids;
    for (const Role &r : _team.roles())
        ids.push_back(r.id);
    for (const auto &[id, name] : _team.formers())
        ids.push_back(id);
    return ids;
}

std::string Backend::titleOf(const Tracked &t) const {
    if (!t.info.name.empty())
        return teammateNames(withoutTeammateNote(t.info.name));
    if (!t.parser.aiTitle().empty())
        return teammateNames(t.parser.aiTitle());
    if (!t.info.cwd.empty())
        return std::string(file::baseName(t.info.cwd));
    return t.info.sessionId.substr(0, 8);
}

std::string Backend::shownTitle(const Tracked &t) const {
    return t.localName.empty() ? titleOf(t) : t.localName;
}

// A title is plain text, so a teammate mention it echoes from the prompt
// ("@claude:role:engineer") can't be a pill: name the teammate instead.
std::string Backend::teammateNames(std::string_view text) const {
    std::string out;
    size_t      last = 0;
    for (size_t i = text.find('@'); i != std::string_view::npos; i = text.find('@', i + 1)) {
        // <@claude:…>, or a bare @claude:… standing on its own.
        const Mention m = mentionAt(text, i);
        if (!m.len)
            continue;
        const bool   token = i > 0 && text[i - 1] == '<' && text.substr(i + m.len, 1) == ">";
        const size_t start = token ? i - 1 : i;
        out.append(text.substr(last, start - last));
        out += '@';
        out += _team.resolve(m.roleId).name;
        last = i + m.len + (token ? 1 : 0);
        i    = last - 1;
    }
    if (last == 0)
        return std::string(text);
    out.append(text.substr(last));
    return out;
}

bool Backend::readOnly(const Tracked &t) const {
    return (t.info.running && t.info.kind == SessionInfo::Kind::Interactive) ||
           _creds.claudePath.empty();
}

std::string Backend::readOnlyReason(const Tracked &t) const {
    if (t.info.running && t.info.kind == SessionInfo::Kind::Interactive) {
        if (t.info.entrypoint == "cli")
            return tr(
                "This session is running in a terminal \xE2\x80\x94 reply there. Once it "
                "ends, you can continue it here."
            );
        return tr(
            "Another program is driving this session. Once it ends, you can continue it "
            "here."
        );
    }
    if (_creds.claudePath.empty())
        return notInstalledMessage();
    return {};
}

// Not working, yet waiting on something besides a message: open in a
// terminal, or on a permission question (messages wait for its answer).
bool Backend::unavailable(const Tracked &t) const {
    return awaitsApproval(t.info) || readOnly(t);
}

bool Backend::roleBusy(const std::string &role) const {
    for (const auto &[id, t] : _sessions)
        if (!asThread(*t) && busy(*t) && roleOf(*t) == role)
            return true;
    return roleSubagentRunning(role);
}

// A subagent started as the teammate running in a live session — the same
// one pumpTyping has thinking in its thread. A plain one ("claude",
// "Explore") speaks as the Generalist, yet isn't one: it lights no dot. As
// of the last look (findSubagentRuns, every refresh and typing tick).
bool Backend::roleSubagentRunning(const std::string &role) const {
    if (role.empty() || role == kGeneralist)
        return false;
    return _subagentRoles.count(role) > 0;
}

// The one walk over every live session's subagents a tick makes: each one
// running thinks in its thread (pumpTyping), and lights its teammate's dot
// when started as one (roleSubagentRunning). Only a live worker runs one: a
// killed session never sends the notification that it stopped.
void Backend::findSubagentRuns() {
    _subagentRuns.clear();
    _subagentRoles.clear();
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (!t.info.running || t.stopping || asThread(t))
            continue;
        tail(t); // the notification that it stopped lands in the session's transcript
        const UserRef assistant = roleUser(roleOf(t));
        for (const size_t i : subagentItems(t)) {
            const TranscriptItem &item  = t.parser.items()[i];
            const int64_t         since = subagentRunSinceMs(t, item.agentId);
            if (!since)
                continue;
            if (_team.find(item.agentType))
                _subagentRoles.insert(item.agentType);
            else if (_team.find(item.agentRole))
                _subagentRoles.insert(item.agentRole);
            if (t.ref != kNoConv)
                _subagentRuns.push_back({t.ref, subagentAuthor(item, assistant), item.ts, since});
        }
    }
}

const std::vector<size_t> &Backend::subagentItems(Tracked &t) {
    const auto &items = t.parser.items();
    uint64_t    key   = mix(crypto::kFnvOffset, int64_t(t.parser.revision()));
    key               = mix(key, int64_t(items.size()));
    key               = mix(key, t.offset);
    if (key != t.subagentsKey) {
        t.subagentsKey = key;
        t.subagentItems.clear();
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].kind == TranscriptItem::Kind::Subagent && !items[i].agentId.empty())
                t.subagentItems.push_back(i);
    }
    return t.subagentItems;
}

uint64_t Backend::syncKey(Tracked &t) {
    const SessionInfo &s = t.info;
    uint64_t           k = mix(crypto::kFnvOffset, int64_t(t.parser.revision()));
    k                    = mix(k, t.offset);
    k                    = mix(k, s.status);
    k                    = mix(k, s.workerStatus);
    k                    = mix(k, s.needs);
    k                    = mix(k, s.entrypoint);
    k                    = mix(k, s.statusSinceMs);
    const bool thread    = asThread(t);
    const bool flags[]   = {
        s.running,
        s.awaitsApproval,
        s.kind == SessionInfo::Kind::Background,
        busy(t),
        t.sending,
        t.promptLanded,
        t.stopping,
        t.stopRequested,
        thread,
        t.standalone,
        t.awaitingRoot,
        _zen,
        t.approvalAnswered,
        t.answering.expired(),
    };
    int64_t bits = 0;
    for (const bool f : flags)
        bits = bits << 1 | int64_t(f);
    k = mix(k, bits);
    k = mix(k, int64_t(t.approvalReads));
    k = mix(k, t.approvalNeeds);
    k = mix(k, int64_t(t.approvalOptions.size()));
    k = mix(k, t.forkOf);
    k = mix(k, int64_t(t.forkAt));
    k = mix(k, t.forkRoot);
    k = mix(k, int64_t(roleUser(roleOf(t))));
    if (t.flying) {
        k = mix(k, t.flying->ts);
        k = mix(k, t.flying->threadRoot);
    }
    for (const auto &o : t.outbox) {
        k = mix(k, o.ts);
        k = mix(k, o.threadRoot);
    }
    // Where it shows, and from which message on.
    const Tracked *parent = thread ? find(t.forkOf) : nullptr;
    const ConvRef  target = thread ? (parent ? parent->ref : kNoConv) : t.ref;
    const auto     shown =
        target == kNoConv ? _shown.end() : _shown.find({target, thread ? t.forkRoot : 0});
    k = mix(k, int64_t(target));
    k = mix(k, shown == _shown.end() ? -1 : shown->second.from);
    // A loaded list counts its subagents' replies: their transcripts — how
    // long each is, and what was read of it (only the sync reads them).
    if (shown != _shown.end() && !thread)
        for (const size_t i : subagentItems(t)) {
            const std::string path =
                Paths::subagentTranscript(t.transcriptPath, t.parser.items()[i].agentId);
            k = mix(k, file::size(path));
            if (const auto f = _subagents.find(path); f != _subagents.end()) {
                k = mix(k, int64_t(f->second->parser.revision()));
                k = mix(k, f->second->offset);
            }
        }
    return k;
}

// A teammate is yellow while none of its sessions works and one of them is
// yellow itself (see assistantUser) — green, as any working one makes it, wins.
bool Backend::roleUnavailable(const std::string &role) const {
    if (roleBusy(role))
        return false;
    for (const auto &[id, t] : _sessions)
        if (!asThread(*t) && roleOf(*t) == role && unavailable(*t))
            return true;
    return false;
}

model::User Backend::meUser() const {
    model::User u;
    u.id          = kMeId;
    u.name        = _myName.empty() ? loginName() : _myName;
    u.displayName = u.name;
    if (!_myAvatarPath.empty() && file::exists(_myAvatarPath))
        u.avatar = _myAvatarPath;
    u.active = true;
    return u;
}

model::User Backend::assistantUser(const Tracked &t) const {
    model::User u;
    u.id          = kAssistantPrefix + t.convId;
    u.name        = shownTitle(t);
    u.displayName = u.name;
    u.avatar      = roleFor(t).avatar;
    u.title       = homeRelative(t.info.cwd); // shown on the profile card
    u.active      = busy(t);
    u.unavailable = !u.active && unavailable(t);
    if (needsUser(t))
        u.statusText = tr("Waiting for you");
    else if (!t.sending && t.info.running && needsLogin(t.info))
        u.statusText = tr("Not logged in");
    else if (busy(t))
        u.statusText = tr("Working");
    else if (t.info.running && statusHasShell(t.info.status))
        u.statusText = tr("Running a background command");
    return u;
}

model::User Backend::teammateUser(const Role &r) const {
    model::User u;
    u.id          = r.id.empty() || r.id == kGeneralist ? std::string(kAgentUser)
                                                        : std::string(kRoleUserPrefix) + r.id;
    u.name        = r.name;
    u.displayName = r.name;
    u.avatar      = r.avatar;
    u.title       = r.description; // shown on the profile card
    u.active      = roleBusy(r.id);
    u.unavailable = roleUnavailable(r.id);
    return u;
}

// ── Transcripts ─────────────────────────────────────────────────────────────

namespace {

// Whole transcripts are read a megabyte at a time: never all of a long one
// (tens of MB) held at once besides what the parser makes of it.
constexpr size_t  kReadChunk       = 1 << 20;
// A transcript read from its start beyond this is read on a worker.
constexpr int64_t kWorkerParseFrom = 1 << 20;
// What a removed session's transcript grew by, looked through for a turn
// (hasTurnSince), beyond this is looked through on a worker.
constexpr int64_t kWorkerScanFrom  = 256 << 10;
// The transcript of a session nothing runs, not found, is looked for again
// after this long (each look lists every project folder): a background job
// whose transcript Claude Code deleted lingers for days.
constexpr int64_t kMissRetryMs     = 10'000;

} // namespace

int64_t feedTranscript(const std::string &path, TranscriptParser &parser) {
    int64_t     offset = 0;
    std::string chunk;
    for (;;) {
        if (!file::readRange(path, offset, kReadChunk, &chunk))
            return offset ? offset : -1;
        if (chunk.empty())
            return offset;
        parser.feed(chunk);
        offset += int64_t(chunk.size());
        if (chunk.size() < kReadChunk)
            return offset;
    }
}

std::string Backend::lookForTranscript(const Tracked &t) {
    const std::string &sessionId = t.info.sessionId;
    if (const auto pre = _preFound.find(sessionId); pre != _preFound.end())
        return pre->second; // the first scan found it on its worker
    // Not written yet (a session nobody typed to): looked for at most once per
    // refresh, not once per tail — and while nothing runs it, only now and then.
    const auto miss = _transcriptMiss.find(sessionId);
    if (miss != _transcriptMiss.end() &&
        (miss->second.first == _scanGen ||
         (!t.info.running && !t.sending && nowMs() - miss->second.second < kMissRetryMs)))
        return {};
    std::string found = _paths.findTranscript(sessionId);
    if (found.empty())
        _transcriptMiss[sessionId] = {_scanGen, nowMs()};
    else
        _transcriptMiss.erase(sessionId);
    return found;
}

void Backend::parseOnWorker(Tracked &t) {
    t.parsing   = true;
    auto parsed = std::make_shared<Preparsed>();
    model::runInBackground(
        _app,
        [parsed, path = t.transcriptPath] {
            parsed->offset = feedTranscript(path, parsed->parser);
        },
        [this, alive = _alive, parsed, convId = t.convId, path = t.transcriptPath] {
            if (!*alive)
                return;
            Tracked *t = find(convId);
            if (!t)
                return;
            t->parsing = false;
            if (t->transcriptPath == path && t->offset == 0 && parsed->offset > 0) {
                t->restart(std::move(parsed->parser), parsed->offset);
            }
            for (auto &then : std::exchange(t->afterParse, {}))
                then();
            if (parsed->offset > 0) // unreadable: tried again on a later refresh, not now
                refresh();
        }
    );
}

void Backend::Tracked::restart(TranscriptParser &&p, int64_t from) {
    parser = std::move(p);
    offset = from;
    rendered.clear();
}

void Backend::whenParsed(Tracked &t, std::function<void()> then) {
    if (t.parsing)
        t.afterParse.push_back(std::move(then));
    else
        then();
}

void Backend::tail(Tracked &t) {
    if (t.info.sessionId.empty() || t.parsing)
        return;
    int64_t size = t.transcriptPath.empty() ? -1 : file::size(t.transcriptPath);
    if (size < 0) {
        const std::string found = lookForTranscript(t);
        if (found.empty())
            return;
        t.transcriptPath = found;
        size             = file::size(t.transcriptPath);
        if (size < 0)
            return;
    }
    if (size < t.offset) {
        // Rewritten or truncated: start over.
        t.restart();
        _born.erase(t.transcriptPath);
    }
    if (t.offset == 0 && !_preparsed.empty()) {
        // Parsed on the first scan's worker: only what came since is read.
        if (const auto pre = _preparsed.find(t.transcriptPath); pre != _preparsed.end()) {
            if (pre->second->offset <= size) {
                t.restart(std::move(pre->second->parser), pre->second->offset);
            }
            _preparsed.erase(pre);
        }
    }
    if (t.offset == 0 && size > kWorkerParseFrom) {
        // Found after the first scan (resumed, added, written anew) and long:
        // read on a worker, never here.
        parseOnWorker(t);
        return;
    }
    if (size > t.offset) {
        // Only the bytes appended since the last look: a long session's
        // transcript runs to tens of MB, and a streamed answer grows it
        // several times a second.
        std::string chunk;
        if (file::readRange(t.transcriptPath, t.offset, size_t(size - t.offset), &chunk) &&
            !chunk.empty()) {
            t.parser.feed(chunk);
            t.offset += int64_t(chunk.size());
            _counters.bytesRead += chunk.size();
        }
    }
    // A role this machine doesn't know (another machine's teammate, a deleted
    // file) keeps the name its sessions give it.
    if (_team.noteFormer(t.parser.role(), t.parser.roleName()))
        putUser(teammateUser(_team.resolve(t.parser.role())));
}

Backend::SubagentFeed &Backend::subagentFeed(const Tracked &t, const std::string &agentId) {
    const std::string path = Paths::subagentTranscript(t.transcriptPath, agentId);
    auto             &slot = _subagents[path];
    if (!slot)
        slot = std::make_unique<SubagentFeed>();
    SubagentFeed &f = *slot;
    if (f.parsing)
        return f; // read on a worker: as it is until then
    const int64_t size = file::size(path);
    if (size < f.offset)
        f = SubagentFeed{};  // gone, rewritten or truncated: start over
    f.parser.keepActivity(); // when its runs began (subagentRunSinceMs)
    if (f.offset == 0 && size > kWorkerParseFrom) {
        // A long subagent (they run to a hundred MB): read on a worker, as
        // tail() does a session's.
        parseSubagentOnWorker(path, t, f);
        return f;
    }
    if (size > f.offset) {
        // Only what was appended since the last look, as tail().
        std::string chunk;
        if (file::readRange(path, f.offset, size_t(size - f.offset), &chunk) && !chunk.empty()) {
            f.parser.feed(chunk);
            f.offset += int64_t(chunk.size());
            _counters.bytesRead += chunk.size();
        }
    }
    if (f.counted != f.parser.revision()) {
        f.counted = f.parser.revision();
        f.zenCount =
            int(std::count_if(f.parser.items().begin(), f.parser.items().end(), [](const auto &i) {
                return i.kind != TranscriptItem::Kind::ToolGroup;
            }));
    }
    return f;
}

void Backend::parseSubagentOnWorker(const std::string &path, const Tracked &t, SubagentFeed &f) {
    f.parsing    = true;
    f.parseToken = ++_subagentParses;
    auto parsed  = std::make_shared<Preparsed>();
    parsed->parser.keepActivity();
    model::runInBackground(
        _app,
        [parsed, path] { parsed->offset = feedTranscript(path, parsed->parser); },
        [this, alive = _alive, parsed, path, convId = t.convId, token = f.parseToken] {
            if (!*alive)
                return;
            // The feed may have been dropped meanwhile (the session no longer
            // looks at its subagents), or started over: then this is no one's.
            const auto it = _subagents.find(path);
            if (it == _subagents.end() || !it->second->parsing || it->second->parseToken != token)
                return;
            SubagentFeed &f = *it->second;
            f.parsing       = false;
            if (parsed->offset > 0) { // unreadable: tried again on a later look
                f.parser = std::move(parsed->parser);
                f.offset = parsed->offset;
                f.rendered.clear();
                f.counted = 0;
            }
            if (std::find(_subagentsReadFor.begin(), _subagentsReadFor.end(), convId) ==
                _subagentsReadFor.end())
                _subagentsReadFor.push_back(convId);
            if (_subagentsReadFor.size() == 1)
                post([this] { subagentsRead(); }); // all that come in meanwhile at once
        }
    );
}

void Backend::subagentsRead() {
    for (const std::string &convId : std::exchange(_subagentsReadFor, {})) {
        Tracked *t = find(convId);
        if (!t || t->parsing || !t->announcedInit)
            continue;
        // Their reply counts in its list, their threads, their runs.
        sync(*t);
        t->syncedKey = syncKey(*t);
        syncThreads(*t);
    }
    pumpTyping();
}

void Backend::forgetCaches(Tracked &t) {
    // Waiting for its transcript, which no one reads now: they find it gone.
    for (auto &then : std::exchange(t.afterParse, {}))
        post(std::move(then));
    if (!t.transcriptPath.empty())
        _born.erase(t.transcriptPath);
    if (!t.transcriptPath.empty()) {
        const std::string dir = Paths::subagentsDir(t.transcriptPath) + "/";
        std::erase_if(_subagents, [&](const auto &e) { return str::startsWith(e.first, dir); });
    }
    const std::string outputs = outputsDir(t.convId) + "/";
    std::erase_if(_noOutputs, [&](const std::string &k) { return str::startsWith(k, outputs); });
    if (!t.info.sessionId.empty())
        _transcriptMiss.erase(t.info.sessionId);
}

int Backend::subagentReplyCount(const Tracked &t, const std::string &agentId, Ts *latest) {
    const SubagentFeed &f     = subagentFeed(t, agentId);
    const auto         &items = f.parser.items();
    *latest                   = items.empty() ? 0 : items.back().ts;
    return _zen ? f.zenCount : int(items.size());
}

// A background subagent runs from its first record after it last stopped:
// the session is notified each time it stops, a few ms after its last record,
// and it may start again (a reply relayed to it, or on its own when work of
// its own ends) — then its file grows past that notification.
//
// One that stopped and hasn't been read here isn't read to tell — the look
// every tick makes would otherwise read every finished subagent of a live
// session, a hundred MB in all — while its file was last written no later
// than a moment after the notification (its own last writes trail that by
// ~0.1 s, measured 2026-10-06; a run starting again writes on).
int64_t Backend::subagentRunSinceMs(const Tracked &t, const std::string &agentId) {
    constexpr int64_t kStopSlackMicros = 1'000'000;
    const int64_t     stopped          = t.parser.taskStoppedAt(agentId);
    if (stopped > 0) {
        const std::string path = Paths::subagentTranscript(t.transcriptPath, agentId);
        const auto        feed = _subagents.find(path);
        file::Stat        st;
        if ((feed == _subagents.end() || feed->second->offset == 0) &&
            (!file::stat(path, &st) || st.mtimeMicros <= stopped + kStopSlackMicros))
            return 0;
    }
    const auto &activity = subagentFeed(t, agentId).parser.activity();
    if (activity.empty() || activity.back() <= stopped)
        return 0;
    auto it = activity.end();
    while (it != activity.begin() && *(it - 1) > stopped)
        --it;
    return *it / 1000;
}

std::string outputKey(const TranscriptItem &item, const std::string &agentId) {
    return (agentId.empty() ? std::string() : agentId + "-") +
           (item.uuid.empty() ? model::formatTs(item.ts) : item.uuid);
}

void Backend::attachOutputs(
    Message                           &m,
    const std::vector<TranscriptItem> &items,
    size_t                             i,
    const std::string                 &convId,
    const std::string                 &cwd,
    const std::string                 &agentId
) {
    const TranscriptItem &item = items[i];
    // Most answers name no file: nothing to look for, nothing to read.
    if (item.kind != TranscriptItem::Kind::AssistantText || !mayNameFiles(item.text))
        return;
    OutputContext ctx;
    ctx.convId     = convId;
    ctx.messageKey = outputKey(item, agentId);
    ctx.cwd        = cwd;
    ctx.date       = item.ts;
    ctx.turnStart  = items.front().ts;
    for (size_t j = i; j-- > 0;)
        if (items[j].kind == TranscriptItem::Kind::UserPrompt) {
            ctx.turnStart = items[j].ts;
            break;
        }
    std::string folder = outputsFolder(ctx);
    if (_noOutputs.count(folder))
        return; // looked for before: it made none
    std::vector<model::File> files;
    if (cachedOutputs(ctx, &files)) {
        if (files.empty())
            _noOutputs.insert(std::move(folder));
        for (model::File &f : files)
            m.extras().files.push_back(std::move(f));
        return;
    }
    // Not looked for yet: found and copied on a worker (up to 10 files of
    // 50 MB each), then the answer is rendered again with them — every answer
    // a list shows in one job.
    if (!_outputsPending.insert(folder).second)
        return;
    _outputsQueue.push_back({item.text, agentId, std::move(folder), std::move(ctx)});
    if (!std::exchange(_outputsFlushPosted, true))
        post([this] { flushOutputs(); });
}

void Backend::flushOutputs() {
    _outputsFlushPosted = false;
    auto wanted         = std::make_shared<std::vector<OutputsWanted>>(std::move(_outputsQueue));
    _outputsQueue.clear();
    if (wanted->empty())
        return;
    model::runInBackground(
        _app,
        [wanted] {
            for (const OutputsWanted &w : *wanted)
                makeOutputs(w.text, w.ctx);
        },
        [this, alive = _alive, wanted] {
            if (!*alive)
                return;
            // The answers to render again: those that got files.
            std::vector<const OutputsWanted *> made;
            for (const OutputsWanted &w : *wanted) {
                _outputsPending.erase(w.folder);
                if (!find(w.ctx.convId)) {
                    clearOutputs(w.ctx.convId); // removed from msga meanwhile: its copies go too
                    continue;
                }
                // None made (most answers): nothing to show, nothing to render again.
                std::vector<model::File> files;
                if (!cachedOutputs(w.ctx, &files) || files.empty()) {
                    _noOutputs.insert(w.folder);
                    continue;
                }
                made.push_back(&w);
            }
            // Once per session and subagent, all of its answers at once.
            while (!made.empty()) {
                const OutputsWanted     *first = made.front();
                std::vector<std::string> keys;
                std::erase_if(made, [&](const OutputsWanted *w) {
                    if (w->ctx.convId != first->ctx.convId || w->agentId != first->agentId)
                        return false;
                    keys.push_back(w->ctx.messageKey);
                    return true;
                });
                outputsMade(first->ctx.convId, first->agentId, keys);
            }
        }
    );
}

// Answers' output files are copied: they're rendered again, with them, and
// whatever list shows them takes the change.
void Backend::outputsMade(
    const std::string &convId, const std::string &agentId, const std::vector<std::string> &keys
) {
    Tracked *t = find(convId);
    if (!t)
        return;
    const auto stale = [&](const std::vector<TranscriptItem> &items,
                           std::vector<Rendered>             &rendered) {
        for (size_t i = 0; i < rendered.size() && i < items.size(); ++i)
            if (items[i].kind == TranscriptItem::Kind::AssistantText &&
                std::find(keys.begin(), keys.end(), outputKey(items[i], agentId)) != keys.end())
                rendered[i].rev = 0;
    };
    if (agentId.empty()) {
        stale(t->parser.items(), t->rendered);
        sync(*t);
    } else {
        SubagentFeed &f = subagentFeed(*t, agentId);
        stale(f.parser.items(), f.rendered);
        syncThreads(*t);
    }
}

const Backend::Rendered &Backend::renderedAt(Tracked &t, size_t i) {
    const auto   &items  = t.parser.items();
    const UserRef author = roleUser(roleOf(t));
    if (author != t.renderedAuthor) {
        t.rendered.clear(); // the role became known: Claude's messages change hands
        t.renderedAuthor = author;
    }
    // A slot per item; one is made when it's asked for (rev 0: not yet).
    if (t.rendered.size() != items.size())
        t.rendered.resize(items.size());
    // A subagent started as a teammate is that teammate's thread; an answer
    // carries the files it made.
    const UserRef me     = _store.me;
    const auto    render = [&](size_t j) {
        ++_counters.renders;
        Rendered r;
        r.msg = toMessage(items[j], me, subagentAuthor(items[j], author));
        if (int(j) == t.forkAt && !t.branchLabel.empty()) // an agent branch's root
            r.msg.text = escapeMrkdwn(t.branchLabel);
        attachOutputs(r.msg, items, j, t.convId, t.info.cwd);
        r.rev       = t.parser.revision(j);
        r.contentFp = contentFingerprint(r.msg);
        return r;
    };
    // Only an item that changed since (its revision) is rendered again.
    if (t.rendered[i].rev != t.parser.revision(i))
        t.rendered[i] = render(i);
    return t.rendered[i];
}

Backend::Visible Backend::ownVisible(Message m) {
    Visible v;
    v.ts          = m.ts;
    v.threadTs    = m.threadTs;
    v.latestReply = m.latestReply;
    v.replyCount  = m.replyCount;
    v.user        = m.user;
    v.progress    = m.subtype() == kProgressSubtype;
    v.contentFp   = contentFingerprint(m);
    v.own         = std::make_shared<const Message>(std::move(m));
    return v;
}

Backend::Visible Backend::renderedVisible(const std::vector<Rendered> &src, size_t index) {
    const Rendered &r = src[index];
    Visible         v;
    v.ts          = r.msg.ts;
    v.threadTs    = r.msg.threadTs;
    v.latestReply = r.msg.latestReply;
    v.replyCount  = r.msg.replyCount;
    v.user        = r.msg.user;
    v.progress    = r.msg.subtype() == kProgressSubtype;
    v.contentFp   = r.contentFp;
    v.src         = &src;
    v.index       = index;
    return v;
}

Message Backend::make(const Visible &v) const {
    ++_counters.copies;
    Message m     = v.base().clone();
    m.ts          = v.ts;
    m.threadTs    = v.threadTs;
    m.replyCount  = v.replyCount;
    m.latestReply = v.latestReply;
    if (v.reactions)
        m.reactions = *v.reactions;
    return m;
}

// A subagent's report, handed back to the session: its thread already ends
// with it (the subagent's own transcript), so here it's a line from the
// subagent pointing there instead of the report again.
Message
Backend::handbackPointer(const Tracked &t, const TranscriptItem &subagent, Ts ts, UserRef parent) {
    Message m;
    m.ts                   = ts;
    m.user                 = subagentAuthor(subagent, parent);
    m.extras().subtype     = kProgressSubtype; // what the session says next notifies
    const std::string what = escapeMrkdwn(
        subagent.text.empty() && !subagent.tools.empty() ? subagent.tools.front().name
                                                         : subagent.text
    );
    const std::string link = str::concat(
        {"<",
         mrkdwn::threadLink(t.convId, model::formatTs(subagent.ts)),
         "|",
         tr("its thread"),
         ">"}
    );
    m.text =
        str::concat({"_", i18n::arg(tr("Subagent “%1” reported back in %2."), what, link), "_"});
    return m;
}

void Backend::realize(Tracked &t, Visible &v) {
    if (v.rendered())
        return;
    const Rendered &r = renderedAt(t, v.index);
    v.src             = &t.rendered;
    v.contentFp       = r.contentFp;
}

std::vector<Backend::Visible> Backend::visibleList(Tracked &t, Ts renderFrom) {
    tail(t);
    const auto                             &items     = t.parser.items();
    const UserRef                           assistant = roleUser(roleOf(t));
    const bool                              isBusy    = busy(t);
    // The user's replies in subagent threads, relayed through this session:
    // replies in the thread, each counted on its root with msga's copies.
    std::unordered_map<std::string, Ts>     subagentRoot; // by agentId
    std::unordered_map<std::string, size_t> subagentItem; // by agentId
    std::map<Ts, int>                       relayed;      // by root
    std::map<Ts, Ts>                        relayedLatest;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == TranscriptItem::Kind::Subagent && !items[i].agentId.empty()) {
            subagentRoot[items[i].agentId] = items[i].ts;
            subagentItem[items[i].agentId] = i;
        }
    const auto noteRelay = [&](Ts root, Ts ts) {
        ++relayed[root];
        relayedLatest[root] = std::max(relayedLatest[root], ts);
    };
    for (const auto &item : items)
        if (!item.relayTo.empty())
            if (const auto r = subagentRoot.find(item.relayTo); r != subagentRoot.end())
                noteRelay(r->second, item.ts);
    if (t.flying && t.flying->threadRoot)
        noteRelay(t.flying->threadRoot, t.flying->ts);
    for (const auto &o : t.outbox)
        if (o.threadRoot)
            noteRelay(o.threadRoot, o.ts);
    std::vector<Visible> out;
    out.reserve(items.size() + 1);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isVisible(item, isBusy) || (_zen && item.kind == TranscriptItem::Kind::ToolGroup))
            continue;
        if (item.kind == TranscriptItem::Kind::PeerMessage)
            if (const auto r = subagentItem.find(item.agentId); r != subagentItem.end()) {
                out.push_back(ownVisible(handbackPointer(t, items[r->second], item.ts, assistant)));
                continue;
            }
        Visible v;
        if (item.ts >= renderFrom || !item.relayTo.empty()) {
            renderedAt(t, i);
            v = renderedVisible(t.rendered, i);
        } else {
            // What toMessage would make of it, without making it.
            v.ts       = item.ts;
            v.index    = i;
            v.user     = item.kind == TranscriptItem::Kind::UserPrompt ? _store.me : assistant;
            v.progress = item.kind == TranscriptItem::Kind::ToolGroup ||
                         item.kind == TranscriptItem::Kind::Subagent ||
                         item.kind == TranscriptItem::Kind::PeerMessage ||
                         (item.kind == TranscriptItem::Kind::AssistantText &&
                          item.state == TranscriptItem::State::Progress);
        }
        if (item.kind == TranscriptItem::Kind::Subagent && !item.agentId.empty()) {
            Ts latest = 0;
            v.replyCount =
                uint32_t(subagentReplyCount(t, item.agentId, &latest) + relayed[item.ts]);
            latest = std::max(latest, relayedLatest[item.ts]);
            if (latest)
                v.latestReply = latest;
        }
        if (!item.relayTo.empty())
            if (const auto r = subagentRoot.find(item.relayTo); r != subagentRoot.end())
                v.threadTs = r->second;
        out.push_back(std::move(v));
    }

    // A session stopped for the user where the transcript doesn't show it — a
    // terminal session waiting at a prompt, a background one on a permission
    // prompt — gets a message of its own, so it notifies and badges like an
    // answer. (A background session that asked a question already shows the
    // question as its last answer.) Its ts is pinned to when the status
    // changed, so it stays the same message while the session waits.
    if (const Ts waitTs = waitingTs(t, out.empty() ? 0 : out.back().ts))
        out.push_back(ownVisible(waitingMessage(t, waitTs)));

    appendOutgoing(t, out, 0);

    // Side conversations branched off this session (/btw): each one's first
    // prompt is a message here, rooting a thread with the rest of it.
    bool branched = false;
    for (auto &[id, fp] : _sessions) {
        Tracked &f = *fp;
        if (f.forkOf != t.convId || !asThread(f))
            continue;
        const auto replies = threadList(f);
        if (f.forkAt < 0)
            continue;
        renderedAt(f, size_t(f.forkAt));
        Visible root    = renderedVisible(f.rendered, size_t(f.forkAt));
        root.replyCount = uint32_t(replies.size());
        if (!replies.empty())
            root.latestReply = replies.back().ts;
        out.push_back(std::move(root));
        branched = true;
    }
    if (branched)
        std::stable_sort(out.begin(), out.end(), [](const Visible &a, const Visible &b) {
            return a.ts < b.ts;
        });
    applyReactions(t.convId, out);
    return out;
}

// "Waiting for you in the terminal." / "Waiting for your approval: …" at ts
// (waitingTs), with the approval's options as buttons once read.
Message Backend::waitingMessage(const Tracked &t, Ts ts) {
    const bool    inTerminal = terminalWaits(t);
    const UserRef assistant  = roleUser(roleOf(t));
    Message       m;
    m.ts                         = ts;
    m.user                       = assistant;
    const std::string_view needs = str::startsWith(t.info.needs, "approve ")
                                       ? std::string_view(t.info.needs).substr(8)
                                       : std::string_view(t.info.needs);
    m.text = inTerminal ? std::string(tr("Waiting for you in the terminal."))
                        : i18n::arg(tr("Waiting for your approval: %1"), escapeMrkdwn(needs));
    if (!inTerminal) {
        // Its options, once read off the screen (readApproval), as
        // buttons; until then a note that msga is still at it — never a
        // terminal.
        const bool read = t.approvalNeeds == t.info.needs && !t.approvalOptions.empty();
        if (read) {
            for (const auto &o : t.approvalOptions) {
                model::Button b;
                b.id    = "option:" + str::number(int64_t(o.number));
                b.label = o.label;
                if (str::startsWith(o.label, "Yes"))
                    b.style = model::Button::Style::Primary;
                else if (str::startsWith(o.label, "No"))
                    b.style = model::Button::Style::Danger;
                m.extras().buttons.push_back(std::move(b));
            }
        } else if (
            t.answering.expired() && t.approvalReads >= kApprovalReads && !t.approvalAnswered
        ) {
            m.text += "\n";
            m.text += tr("Its options couldn't be read yet; trying again.");
        }
    }
    return m;
}

bool Backend::newsAt(Tracked &t, bool thread, Ts ts, Message *out) {
    const auto   &items = t.parser.items();
    const UserRef owner = roleUser(roleOf(t));
    const size_t  from  = thread ? size_t(std::max(t.forkAt, 0)) + 1 : 0;
    for (size_t i = from; i < items.size(); ++i) {
        if (items[i].ts != ts)
            continue;
        *out = toMessage(items[i], _store.me, subagentAuthor(items[i], owner));
        if (thread)
            out->threadTs = t.forkRoot;
        return true;
    }
    // Not an item: the session's "Waiting for…" (seenOf's only other news).
    if (thread || (!terminalWaits(t) && !awaitsApproval(t.info)))
        return false;
    *out = waitingMessage(t, ts);
    return true;
}

bool Backend::terminalWaits(const Tracked &t) const {
    return t.info.running && t.info.kind == SessionInfo::Kind::Interactive && needsUser(t);
}

// The ts of "Waiting for you…" (visibleList), 0 when it doesn't wait:
// pinned to when the status changed, after everything before it (lastTs).
Ts Backend::waitingTs(const Tracked &t, Ts lastTs) const {
    if (!terminalWaits(t) && !awaitsApproval(t.info))
        return 0;
    const Ts micros = t.info.statusSinceMs * 1000;
    return micros <= lastTs ? lastTs + 1 : micros;
}

// The ts visibleList (a /btw branch: threadList) ends on, 0 for an empty
// list — read off the items, nothing rendered.
Ts Backend::lastShownTs(Tracked &t) {
    tail(t);
    const bool thread = asThread(t);
    if (thread && t.forkAt < 0)
        return 0;
    const auto  &items  = t.parser.items();
    const bool   isBusy = busy(t);
    const size_t from   = thread ? size_t(t.forkAt) + 1 : 0;
    Ts           last   = 0; // the newest item shown (their ts only grow)
    for (size_t i = items.size(); i-- > from;)
        if (isVisible(items[i], isBusy) &&
            !(_zen && items[i].kind == TranscriptItem::Kind::ToolGroup)) {
            last = items[i].ts;
            break;
        }
    Ts out = last;
    if (!thread) {
        out = std::max(out, waitingTs(t, last));
        for (auto &[id, fp] : _sessions) { // the /btw threads' roots
            Tracked &f = *fp;
            if (f.forkOf != t.convId || !asThread(f))
                continue;
            tail(f);
            if (f.forkAt >= 0)
                out = std::max(out, f.parser.items()[size_t(f.forkAt)].ts);
        }
    }
    if (t.flying)
        out = std::max(out, t.flying->ts);
    for (const auto &o : t.outbox)
        out = std::max(out, o.ts);
    return out;
}

// What visibleList (thread: threadList) shows, as sync() takes it —
// each message's ts, whose and what kind — read off the parser's items alone.
// It must keep in step with those two, item for item.
std::map<Ts, Backend::Seen> Backend::seenOf(Tracked &t, bool thread) {
    std::map<Ts, Tracked::Seen> out;
    tail(t);
    const auto &items  = t.parser.items();
    const bool  isBusy = busy(t);
    const auto  shown  = [&](const TranscriptItem &item) {
        return isVisible(item, isBusy) && !(_zen && item.kind == TranscriptItem::Kind::ToolGroup);
    };
    // toMessage: prompts are mine; tool cards, subagents, remarks and what
    // other sessions or subagents said to it (hand-backs) progress.
    const auto seen = [](const TranscriptItem &item, Ts threadTs) {
        Tracked::Seen s;
        s.mine     = item.kind == TranscriptItem::Kind::UserPrompt;
        s.progress = item.kind == TranscriptItem::Kind::ToolGroup ||
                     item.kind == TranscriptItem::Kind::Subagent ||
                     item.kind == TranscriptItem::Kind::PeerMessage ||
                     (item.kind == TranscriptItem::Kind::AssistantText &&
                      item.state == TranscriptItem::State::Progress);
        s.thread   = threadTs;
        return s;
    };
    const auto outgoing = [&](Ts threadRoot) {
        const auto add = [&](const Tracked::Outgoing &o) {
            out[o.ts] = {true, false, o.threadRoot ? o.threadRoot : threadRoot};
        };
        if (t.flying)
            add(*t.flying);
        for (const auto &o : t.outbox)
            add(o);
    };
    if (thread) {
        if (t.forkAt < 0)
            return out;
        for (size_t i = size_t(t.forkAt) + 1; i < items.size(); ++i)
            if (shown(items[i]))
                out[items[i].ts] = seen(items[i], t.forkRoot);
        outgoing(t.forkRoot);
        return out;
    }
    std::unordered_map<std::string, Ts> subagentRoot; // by agentId
    for (const auto &item : items)
        if (item.kind == TranscriptItem::Kind::Subagent && !item.agentId.empty())
            subagentRoot[item.agentId] = item.ts;
    Ts lastTs = 0;
    for (const auto &item : items) {
        if (!shown(item))
            continue;
        Ts threadTs = 0;
        if (!item.relayTo.empty())
            if (const auto r = subagentRoot.find(item.relayTo); r != subagentRoot.end())
                threadTs = r->second;
        out[item.ts] = seen(item, threadTs);
        lastTs       = item.ts;
    }
    if (const Ts waitTs = waitingTs(t, lastTs))
        out[waitTs] = {};
    outgoing(0);
    for (auto &[id, fp] : _sessions) {
        Tracked &f = *fp;
        if (f.forkOf != t.convId || !asThread(f))
            continue;
        tail(f);
        if (f.forkAt >= 0)
            out[f.parser.items()[size_t(f.forkAt)].ts] = {true, false, 0}; // its first prompt
    }
    return out;
}

std::vector<Backend::Visible> Backend::threadList(Tracked &f) {
    tail(f);
    std::vector<Visible> out;
    if (f.forkAt < 0)
        return out;
    const auto &items  = f.parser.items();
    const bool  isBusy = busy(f);
    for (size_t i = size_t(f.forkAt) + 1; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isVisible(item, isBusy) || (_zen && item.kind == TranscriptItem::Kind::ToolGroup))
            continue;
        renderedAt(f, i);
        Visible v  = renderedVisible(f.rendered, i);
        v.threadTs = f.forkRoot;
        out.push_back(std::move(v));
    }
    appendOutgoing(f, out, f.forkRoot);
    applyReactions(f.forkOf, out);
    return out;
}

// The root is the Subagent message; the replies are its own transcript: the
// prompt written by the session's assistant, the rest said by the subagent
// (the teammate it was started as, else the assistant too), and the user's
// replies relayed to it through the session (in `shown`, visibleList(t)).
std::vector<Backend::Visible>
Backend::subagentList(Tracked &t, Ts root, const std::vector<Visible> &shown) {
    std::vector<Visible> out;
    const UserRef        assistant = roleUser(roleOf(t));
    std::string          agentId;
    UserRef              subagent = assistant;
    for (const auto &item : t.parser.items())
        if (item.ts == root) {
            agentId  = item.agentId;
            subagent = subagentAuthor(item, assistant);
        }
    for (const auto &v : shown)
        if (v.threadTs == root)
            out.push_back(v);
    if (!agentId.empty()) {
        // Read as it grows, rendered as it changes (like the session's own).
        SubagentFeed &f     = subagentFeed(t, agentId);
        const auto   &items = f.parser.items();
        if (f.renderedMe != assistant || f.renderedAuthor != subagent) {
            f.rendered.clear();
            f.renderedMe     = assistant;
            f.renderedAuthor = subagent;
        }
        f.rendered.resize(items.size()); // new ones are made below (rev 0)
        for (size_t i = 0; i < items.size(); ++i) {
            const auto &item = items[i];
            if (_zen && item.kind == TranscriptItem::Kind::ToolGroup)
                continue;
            Rendered &r = f.rendered[i];
            if (r.rev != f.parser.revision(i)) {
                ++_counters.renders;
                r.msg = toMessage(item, assistant, subagent);
                attachOutputs(r.msg, items, i, t.convId, t.info.cwd, agentId);
                r.rev       = f.parser.revision(i);
                r.contentFp = contentFingerprint(r.msg);
            }
            Visible v  = renderedVisible(f.rendered, i);
            v.threadTs = root;
            out.push_back(std::move(v));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Visible &a, const Visible &b) {
        return a.ts < b.ts;
    });
    // Two transcripts, one list: a relayed reply and a subagent record in the
    // same microsecond would share a ts.
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i].ts <= out[i - 1].ts)
            out[i].ts = out[i - 1].ts + 1;
    applyReactions(t.convId, out);
    return out;
}

void Backend::appendOutgoing(const Tracked &t, std::vector<Visible> &out, Ts threadRoot) const {
    const auto add = [&](const Tracked::Outgoing &o) {
        TranscriptItem item;
        item.kind  = TranscriptItem::Kind::UserPrompt;
        item.ts    = o.ts;
        item.text  = takeAttachments(o.shown.empty() ? o.text : o.shown, &item.images);
        Message m  = toMessage(item, _store.me, _store.me);
        m.threadTs = o.threadRoot ? o.threadRoot : threadRoot;
        out.push_back(ownVisible(std::move(m)));
    };
    if (t.flying)
        add(*t.flying);
    for (const auto &o : t.outbox)
        add(o);
}

bool Backend::isOutgoingCopy(const Tracked &t, Ts ts) const {
    return (t.flying && t.flying->ts == ts) ||
           std::any_of(t.outbox.begin(), t.outbox.end(), [&](const Tracked::Outgoing &o) {
               return o.ts == ts;
           });
}

void Backend::applyReactions(const std::string &convId, std::vector<Visible> &list) const {
    const auto byTs = _reactions.find(convId);
    for (auto &v : list) {
        if (!v.rendered())
            continue; // realize() first
        v.reactions = nullptr;
        if (byTs != _reactions.end())
            if (const auto r = byTs->second.find(v.ts); r != byTs->second.end())
                v.reactions = &r->second;
        // What make() returns, hashed without making it.
        v.fp = fingerprint(
            v.contentFp,
            v.ts,
            v.threadTs,
            v.replyCount,
            v.latestReply,
            v.reactions ? *v.reactions : v.base().reactions
        );
    }
}

// ── Branched sessions (/btw threads) ────────────────────────────────────────
// A fork starts with a copy of its parent's records — same timestamps, so the
// same leading items — and goes on with its own. Nothing in it names the
// parent, so the family is read off the transcripts: sessions whose first
// item is the same are one family, and each younger member is a fork of the
// older one it shares the longest start with. The thread's root is the fork's
// first prompt after that shared start.

bool Backend::asThread(const Tracked &t) const {
    return !t.forkOf.empty() && !t.standalone && t.forkOf != t.convId && find(t.forkOf);
}

Backend::Tracked *Backend::forkFor(const std::string &parentConv, Ts root) {
    for (auto &[id, fp] : _sessions) {
        Tracked &f = *fp;
        if (f.forkOf == parentConv && f.forkAt >= 0 && f.forkRoot == root && asThread(f))
            return &f;
    }
    return nullptr;
}

const Backend::Tracked *Backend::forkFor(const std::string &parentConv, Ts root) const {
    return const_cast<Backend *>(this)->forkFor(parentConv, root);
}

int64_t Backend::bornMicros(const std::string &path) {
    // A transcript's birth doesn't change (one written anew is read anew:
    // tail forgets it).
    const auto it = _born.find(path);
    if (it != _born.end())
        return it->second;
    file::Stat    st;
    const int64_t born = file::stat(path, &st) ? st.birthMicros : 0;
    _born.emplace(path, born);
    return born;
}

namespace {
// What a session's family is known by: its first item (its time and text).
int64_t familyKey(const TranscriptItem &first) {
    return int64_t(mix(crypto::fnv1a(first.text), first.ts));
}
} // namespace

std::unordered_set<std::string> Backend::detectForks() {
    // Families are made of the sessions' first items alone, and a family's
    // forks of its members' items: when none of those changed (nor which
    // sessions there are) since the last look, neither did they. A session of
    // no family streaming its answer changes nothing.
    uint64_t sig = crypto::kFnvOffset;
    for (const auto &[id, tp] : _sessions) {
        const auto &items = tp->parser.items();
        sig               = mix(sig, id);
        sig               = mix(sig, tp->transcriptPath);
        sig               = mix(sig, int64_t(tp->info.sessionId.empty()));
        sig               = mix(sig, items.empty() ? 0 : familyKey(items.front()));
        if (tp->inFamily) {
            sig = mix(sig, tp->offset);
            sig = mix(sig, int64_t(tp->parser.revision()));
            sig = mix(sig, int64_t(items.size()));
        }
    }
    if (sig == _forkSig)
        return {};
    _forkSig           = sig;
    // By first item: sorted by its key, then each run of the same first item.
    const auto byFirst = [](const std::pair<int64_t, Tracked *> &a,
                            const std::pair<int64_t, Tracked *> &b) { return a.first < b.first; };
    std::vector<std::pair<int64_t, Tracked *>> keyed;
    for (auto &[id, tp] : _sessions) {
        Tracked    &t     = *tp;
        const auto &items = t.parser.items();
        t.inFamily        = false;
        if (t.info.sessionId.empty() || items.empty() || t.transcriptPath.empty())
            continue;
        keyed.emplace_back(familyKey(items.front()), &t);
    }
    std::stable_sort(keyed.begin(), keyed.end(), byFirst);
    std::unordered_set<std::string> changed; // parents whose threads changed
    for (size_t from = 0, to = 0; from < keyed.size(); from = to) {
        const TranscriptItem &first = keyed[from].second->parser.items().front();
        for (to = from + 1; to < keyed.size() && keyed[to].first == keyed[from].first; ++to) {
            const TranscriptItem &other = keyed[to].second->parser.items().front();
            if (other.ts != first.ts || other.text != first.text)
                break; // the same key, another first item: never one family
        }
        if (to - from < 2)
            continue;
        std::vector<std::pair<int64_t, Tracked *>> byAge;
        for (size_t k = from; k < to; ++k) {
            keyed[k].second->inFamily = true;
            byAge.emplace_back(bornMicros(keyed[k].second->transcriptPath), keyed[k].second);
        }
        std::stable_sort(byAge.begin(), byAge.end(), byFirst);
        for (size_t i = 1; i < byAge.size(); ++i) {
            Tracked &f      = *byAge[i].second;
            Tracked *parent = nullptr;
            size_t   best   = 0;
            for (size_t j = 0; j < i; ++j) {
                const size_t n = sharedStart(byAge[j].second->parser.items(), f.parser.items());
                if (!parent || n > best) {
                    parent = byAge[j].second;
                    best   = n;
                }
            }
            const auto &items = f.parser.items();
            int         at    = -1;
            for (size_t k = best; k < items.size(); ++k)
                if (items[k].kind == TranscriptItem::Kind::UserPrompt) {
                    at = int(k);
                    break;
                }
            const Ts root = at >= 0 ? items[size_t(at)].ts : 0;
            if (f.forkOf != parent->convId || f.forkAt != at || f.forkRoot != root) {
                if (!f.forkOf.empty())
                    changed.insert(f.forkOf);
                changed.insert(parent->convId);
            }
            f.forkOf   = parent->convId;
            f.forkAt   = at;
            f.forkRoot = root;
        }
    }
    return changed;
}

std::string Backend::subagentOf(const Tracked &t, Ts root) const {
    for (const auto &item : t.parser.items())
        if (item.ts == root && item.kind == TranscriptItem::Kind::Subagent)
            return item.agentId;
    return {};
}

bool Backend::threadAcceptsReplies(ConvRef conv, Ts root) const {
    const Tracked *t = findRef(conv);
    if (!t)
        return false;
    if (forkFor(t->convId, root))
        return true;
    return !subagentOf(*t, root).empty();
}

// A /btw thread is the user's: its answers notify as replies to them.
bool Backend::threadFollowed(ConvRef conv, Ts root) const {
    const Tracked *t = findRef(conv);
    return t && forkFor(t->convId, root) != nullptr;
}

bool Backend::threadOpensAsSession(ConvRef conv, Ts root) const {
    const Tracked *t = findRef(conv);
    return t && forkFor(t->convId, root) != nullptr;
}

model::ConvRef Backend::openThreadAsSession(ConvRef conv, Ts root) {
    Tracked *p = findRef(conv);
    Tracked *f = p ? forkFor(p->convId, root) : nullptr;
    if (!f)
        return kNoConv;
    f->standalone    = true;
    f->announcedInit = false; // re-taken as a conversation of its own, silently
    sync(*f);
    syncMeta(*f); // the session joins the list
    sync(*p);     // the thread's root leaves the parent
    syncMeta(*p);
    saveKnown();
    return f->ref;
}

// ── Into the Store ──────────────────────────────────────────────────────────

void Backend::sync(Tracked &t) {
    if (t.parsing)
        return; // nothing to take yet: it would all be news once read
    ++_counters.syncs;
    // A branched session's news is its thread's, in its parent's conversation.
    const bool thread = asThread(t);
    if (thread != t.announcedAsThread) {
        t.announcedAsThread = thread;
        t.announcedInit     = false; // what was announced belongs to the other place
    }
    Tracked      *parent = thread ? find(t.forkOf) : nullptr;
    // Where it shows: the conversation's own list, or its parent's thread.
    const ConvRef target = thread ? (parent ? parent->ref : kNoConv) : t.ref;
    // A list the Store doesn't hold needs no messages, only what they are
    // (seenOf): no markdown rendered, no answer's files copied — the first
    // scan looks at every session, the UI opens a few.
    const auto    shown =
        target == kNoConv ? _shown.end() : _shown.find({target, thread ? t.forkRoot : 0});
    const bool                  loaded = shown != _shown.end();
    std::vector<Visible>        list;
    std::map<Ts, Tracked::Seen> now;
    const auto                  take = [&] {
        if (!loaded) {
            now = seenOf(t, thread);
            return;
        }
        // Only what the Store holds is rendered (syncList takes no more).
        list = thread ? threadList(t) : visibleList(t, shown->second.from);
        now.clear();
        for (const auto &v : list)
            now[v.ts] = {v.user == _store.me, v.progress, v.threadTs};
    };
    take();
    // The message on its way is delivered once its prompt is in the
    // transcript: msga's copy of it goes in the same breath.
    if (t.flying && t.announcedInit && std::any_of(now.begin(), now.end(), [&](const auto &n) {
            return n.second.mine && !t.announced.count(n.first) && !isOutgoingCopy(t, n.first);
        })) {
        t.flying.reset();
        take();
    }
    bool            sawOwnPrompt = false;
    std::vector<Ts> fresh; // news in a list the Store doesn't hold
    if (!t.announcedInit) {
        // First look at this session: its history is served by loadHistory,
        // nothing here is news.
        t.announced     = std::move(now);
        t.announcedInit = true;
        ++t.announcedGen;
        if (!thread && t.lastRead == 0)
            for (auto it = t.announced.rbegin(); it != t.announced.rend(); ++it)
                if (!it->second.thread) {
                    t.lastRead = it->first; // don't greet a new session with old unreads
                    break;
                }
    } else {
        for (const auto &[ts, seen] : now) {
            if (t.announced.count(ts))
                continue;
            if (seen.mine && !isOutgoingCopy(t, ts))
                sawOwnPrompt = true;
            // News in a list the Store doesn't hold (any session's): an answer, a "Waiting for…" —
            // announced on its own, so it notifies. Opening the list later serves it as history
            // (its fingerprint taken then), never again as news.
            else if (!loaded && !seen.mine && !seen.progress && target != kNoConv)
                fresh.push_back(ts);
        }
        t.announced = std::move(now);
        ++t.announcedGen;
    }
    // A /btw is delivered once the branch's first prompt — the thread's root,
    // shown in the parent — is in its transcript.
    if (thread && t.awaitingRoot && t.forkAt >= 0) {
        t.awaitingRoot = false;
        sawOwnPrompt   = true;
    }
    // The turn msga started is under way once its prompt is in the transcript.
    if (sawOwnPrompt && t.sending && !t.promptLanded) {
        t.promptLanded   = true;
        t.promptLandedMs = nowMs();
        t.inFlightBack   = {};
        if (auto done = std::exchange(t.inFlight, {}))
            done(true, {});
    }

    // The Store's lists: the conversation's (once its history was loaded) and
    // its loaded threads.
    if (!loaded) {
        for (const Ts ts : fresh) {
            Message m;
            if (newsAt(t, thread, ts, &m))
                _store.announceReply(target, m);
        }
        return;
    }
    if (thread) {
        syncList(target, t.forkRoot, list);
        return;
    }
    // Replies relayed to a subagent belong to its thread (subagentList).
    std::erase_if(list, [](const Visible &v) { return v.threadTs != 0; });
    syncList(target, 0, list);
}

// What the Store holds of a loaded list (conversation `conv`, thread `root`;
// 0 = the conversation's own) made what `want` says: what went is removed,
// what's new added, what changed (its fingerprint) updated — the rest is
// left alone, never copied.
void Backend::syncList(ConvRef conv, Ts root, const std::vector<Visible> &want) {
    const auto it = _shown.find({conv, root});
    if (it == _shown.end())
        return; // not loaded: the Store doesn't hold it
    Shown                        &s = it->second;
    std::map<Ts, const Visible *> byTs;
    for (const auto &v : want)
        if (root || v.ts >= s.from)
            byTs[v.ts] = &v;
    for (auto f = s.fp.begin(); f != s.fp.end();) {
        if (byTs.count(f->first)) {
            ++f;
            continue;
        }
        _store.removeMessage(conv, f->first);
        f = s.fp.erase(f);
    }
    for (const auto &[ts, v] : byTs) {
        const auto old = s.fp.find(ts);
        if (old == s.fp.end()) {
            s.fp[ts] = v->fp;
            _store.addMessage(conv, make(*v));
        } else if (old->second != v->fp) {
            old->second  = v->fp;
            Message copy = make(*v);
            _store.updateMessage(conv, ts, [&](Message &x) { x = std::move(copy); });
        }
    }
}

// A loaded subagent thread follows its transcript: rebuilt whenever its root
// (reply count, newest reply) changed.
void Backend::syncThreads(Tracked &t) {
    if (t.ref == kNoConv || asThread(t))
        return;
    std::optional<std::vector<Visible>> shown; // what the session shows, taken once
    for (const auto &[key, s] : _shown) {
        if (key.first != t.ref || key.second == 0 || forkFor(t.convId, key.second))
            continue;
        if (!shown)
            shown = visibleList(t, kRenderNone); // its replies relayed to subagents alone
        syncList(t.ref, key.second, subagentList(t, key.second, *shown));
    }
}

// The conversation and the session's user, as the list shows them; pushed
// when they changed. Counts are the session's own (unread = what Claude said
// since the last read; only its answers count toward the red counter).
void Backend::syncMeta(Tracked &t) {
    if (asThread(t)) {
        // A thread, not a conversation — one listed before it was known for a
        // branch leaves the list.
        if (t.ref != kNoConv && _store.conversation(t.ref).member)
            _store.updateConversation(t.ref, [](model::Conversation &c) { c.member = false; });
        return;
    }
    const model::User u    = assistantUser(t);
    std::string       usig = str::concat(
        {u.name,
         "\x1f",
         u.statusText,
         "\x1f",
         u.title,
         "\x1f",
         u.avatar,
         "\x1f",
         u.active ? "1" : "0",
         u.unavailable ? "1" : "0"}
    );
    if (usig != t.userSig) {
        t.userSig = std::move(usig);
        putUser(u);
    }
    // Counted again only when what's shown, or the read mark, moved.
    if (t.countedGen != t.announcedGen || t.countedRead != t.lastRead) {
        t.countedGen  = t.announcedGen;
        t.countedRead = t.lastRead;
        t.unread = t.mentions = 0;
        t.latest              = 0;
        for (const auto &[ts, seen] : t.announced) {
            if (seen.thread)
                continue;
            t.latest = ts;
            if (seen.mine || (t.lastRead && ts <= t.lastRead))
                continue;
            ++t.unread;
            if (!seen.progress)
                ++t.mentions;
        }
    }
    const uint32_t unread = t.unread, mentions = t.mentions;
    const Ts       latest = t.latest;
    std::string    topic  = homeRelative(t.info.cwd);
    if (t.skipPermissionChecks)
        topic += tr(" \xC2\xB7 no permission checks");
    const std::string readOnly = readOnlyReason(t);
    const bool        isNew    = t.ref == kNoConv;
    if (isNew) {
        model::Conversation c;
        c.id   = t.convId;
        c.kind = model::ConvKind::Dm;
        t.ref  = _store.addConversation(std::move(c));
    }
    const UserRef     dm   = sessionUser(t);
    const auto       &cur  = _store.conversation(t.ref);
    const std::string name = titleOf(t);
    if (isNew || !cur.member || cur.name != name || cur.localName != t.localName ||
        cur.topic != topic || cur.readOnly != readOnly || cur.unread != unread ||
        cur.mentions != mentions || cur.latest != latest || cur.lastRead != t.lastRead ||
        cur.dmUser != dm || cur.starred != t.starred || cur.muted != t.muted ||
        cur.notify != t.notify)
        _store.updateConversation(t.ref, [&](model::Conversation &c) {
            c.name      = name;
            c.kind      = model::ConvKind::Dm;
            c.dmUser    = dm;
            c.localName = t.localName;
            c.topic     = topic;
            c.readOnly  = readOnly;
            c.member    = true;
            c.unread    = unread;
            c.mentions  = mentions;
            c.latest    = latest;
            c.lastRead  = t.lastRead;
            c.starred   = t.starred;
            c.muted     = t.muted;
            c.notify    = t.notify;
        });
}

void Backend::syncUsers() {
    ++_batchUsers;
    _store.me   = _store.addUser(meUser());
    _usersDirty = true;
    for (const std::string &role : roleIds())
        putUser(teammateUser(_team.resolve(role)));
    if (--_batchUsers == 0 && std::exchange(_usersDirty, false))
        _store.usersChanged();
}

void Backend::putUser(model::User u) {
    _store.addUser(std::move(u));
    if (_batchUsers)
        _usersDirty = true;
    else
        _store.usersChanged();
}

// ── Roster refresh ──────────────────────────────────────────────────────────

void Backend::scheduleRefresh() {
    if (!_started || _debounce)
        return;
    // A write burst (a streamed reply) → one refresh.
    _debounce = _app.addTimer(150, false, [this, alive = _alive] {
        if (!*alive)
            return;
        _debounce = 0;
        refresh();
    });
}

void Backend::refresh() {
    // Users changed during the scan are told once, at its end.
    ++_batchUsers;
    refreshScan();
    if (--_batchUsers == 0 && std::exchange(_usersDirty, false))
        _store.usersChanged();
}

void Backend::refreshScan() {
    ++_scanGen; // a transcript not found is looked for again from here on
    // Only the jobs whose state.json changed are read again.
    auto                            scanned = scanSessions(_paths, nullptr, &_jobStates);
    // A "+" session being started shows up in the roster a moment before the
    // launcher reports its id — don't list it twice meanwhile.
    std::unordered_set<std::string> startingIn;
    for (const auto &[id, t] : _sessions)
        if (t->sending && t->info.sessionId.empty())
            startingIn.insert(file::absolute(t->info.cwd));
    for (const auto &f : _forkingIn)
        startingIn.insert(f);                  // likewise a /btw branch being launched
    std::unordered_set<std::string> listedNow; // conversation ids
    for (auto &s : scanned) {
        const std::string convId = convIdFor(s.sessionId);
        if (!find(convId) && startingIn.count(file::absolute(s.cwd)))
            continue;
        if (const auto h = _hidden.find(s.sessionId); h != _hidden.end()) {
            // Removed from msga: stays away until its transcript gets a new
            // turn. No transcript (none yet, or Claude Code already deleted it
            // while the job lingers) means no new activity either.
            const std::string path =
                s.transcriptPath.empty() ? h->second.transcript : s.transcriptPath;
            // While msga stops it, what it still writes isn't new activity either.
            file::Stat st;
            if (h->second.stopping || !file::stat(path, &st) ||
                st.mtimeMicros / 1000 <= h->second.atMs)
                continue;
            const int64_t size = st.size;
            // Written since, but maybe only Claude Code's bookkeeping (its
            // daemon retiring the idle worker an hour on appends some). An
            // entry saved before sizes were kept has no size: look for a turn
            // timestamped after the removal anywhere in the file instead.
            if (h->second.checking)
                continue; // being looked through on a worker
            if (h->second.seenSize < 0 || size >= h->second.seenSize) {
                const bool    sized = h->second.seenSize >= 0;
                const int64_t from  = sized ? h->second.seenSize : 0;
                if (size != h->second.seenSize && size - from > kWorkerScanFrom) {
                    checkHiddenOnWorker(s.sessionId, path, from, size);
                    continue;
                }
                if (size == h->second.seenSize ||
                    !hasTurnSince(path, from, sized ? 0 : h->second.atMs)) {
                    h->second.seenSize = size;
                    scheduleSaveKnown();
                    continue;
                }
            }
            _hidden.erase(h);
        }
        // A session msga went on from in a copy (adoptCopy), should it be
        // listed again, never takes over the chat that is the copy's now.
        if (const Tracked *owner = find(convId);
            owner && !owner->info.sessionId.empty() && owner->info.sessionId != s.sessionId)
            continue;
        listedNow.insert(convId);
        const bool isNew = !find(convId);
        Tracked   &t     = ensureTracked(convId);
        // Keep a known name when the fresh entry has none. A background worker
        // is named after the prompt it was resumed with, and a slash command
        // ("/compact") is no name for a session.
        if (!s.transcriptPath.empty())
            t.transcriptPath = s.transcriptPath;
        std::string oldName = std::move(t.info.name);
        t.info              = std::move(s); // the scan's own copy: taken, not copied
        if (str::startsWith(t.info.name, "/"))
            t.info.name.clear();
        if (t.info.name.empty())
            t.info.name = std::move(oldName);
        t.listed = true;
        if (isNew)
            t.announcedInit = false; // a session started elsewhere: its history isn't news
    }

    // Sessions that left the roster.
    std::vector<std::string> dropped;
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (listedNow.count(id))
            continue;
        t.listed       = false;
        t.info.running = false;
        t.info.status.clear();
        t.info.needs.clear();
        t.info.awaitsApproval = false;
        t.info.suggestedReply.clear();
        if (t.info.sessionId.empty() || t.sending)
            continue; // a "+" session not started yet, or one being started right now
        // Kept exactly as long as Claude Code keeps the session: a background
        // job whose state dir is gone was removed (`claude rm`); any other
        // session lives on in its transcript until Claude Code cleans that up.
        const bool jobGone = t.info.kind == SessionInfo::Kind::Background;
        if (jobGone || (!t.transcriptPath.empty() && !file::exists(t.transcriptPath)) ||
            (t.transcriptPath.empty() && lookForTranscript(t).empty()))
            dropped.push_back(id);
    }
    for (const auto &id : dropped) {
        Tracked &t = *_sessions[id];
        _convOf.erase(t.info.sessionId);
        if (t.ref != kNoConv)
            _store.updateConversation(t.ref, [](model::Conversation &c) { c.member = false; });
        for (auto it = _shown.begin(); it != _shown.end();)
            it = it->first.first == t.ref ? _shown.erase(it) : std::next(it);
        forgetCaches(t);
        endWatch(t, tr("The session was removed from msga."));
        _sessions.erase(id);
        clearOutputs(id);
    }

    // Read what's new (only a live session can have changed, or one that just
    // ended: its last answer stops being "pending"; an ended one was read
    // already), then sort out which sessions are branches of which.
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (!t.announcedInit || t.listed || t.sending || t.wasLive)
            tail(t);
    }
    std::unordered_set<std::string> parentsToSync = detectForks();

    // What changed, per session — the /btw threads' replies first: a reply
    // arriving bumps its root's count, so the root's own count (synced with
    // its parent) must already include it, never run ahead.
    std::vector<Tracked *> order;
    order.reserve(_sessions.size());
    for (auto &[id, tp] : _sessions)
        if (asThread(*tp))
            order.push_back(tp.get());
    for (auto &[id, tp] : _sessions)
        if (!asThread(*tp))
            order.push_back(tp.get());
    const bool syncAll = std::exchange(_syncAll, false);
    for (Tracked *session : order) {
        Tracked   &t    = *session;
        const bool live = t.listed || t.sending;
        if (t.parsing) { // read on a worker: synced once it's read
            t.wasLive = live;
            syncMeta(t);
            continue;
        }
        // A live session whose transcript, state and sending are all as the
        // last sync left them has nothing new to sync.
        if ((!t.announcedInit || live || t.wasLive) &&
            (!t.announcedInit || syncAll || syncKey(t) != t.syncedKey)) {
            sync(t);
            t.syncedKey = syncKey(t);
            if (asThread(t))
                parentsToSync.insert(t.forkOf); // its root's reply count
        }
        t.wasLive = live;

        // A turn that failed for want of a login: the login is checked again
        // before the next send, which is then told why.
        if (t.parser.loginFailedAt() > _loginFailedSeen) {
            _loginFailedSeen = t.parser.loginFailedAt();
            _loginCheckedMs  = 0;
        }
        settleTurn(t);
        if (t.watch)
            watchTurn(t); // someone follows its turn: how it goes, how it ended
        dispatch(t);      // the next queued message, if the turn is over
        readApproval(t);
        syncMeta(t);
        if (!asThread(t))
            t.lastBusy = busy(t);
    }
    for (const auto &id : parentsToSync)
        if (Tracked *p = find(id)) {
            sync(*p);
            p->syncedKey = syncKey(*p);
            syncMeta(*p);
        }
    for (auto &[id, tp] : _sessions)
        if (tp->listed || tp->wasLive)
            syncThreads(*tp);
    findSubagentRuns(); // this refresh's one look, for the roles and the typing alike
    announceRoles();
    if (!_firstScanDone) {
        // Copies of sessions gone while msga wasn't looking.
        std::vector<std::string> keep;
        for (const auto &[id, t] : _sessions)
            keep.push_back(id);
        pruneOutputs(keep);
    }
    _firstScanDone = true;
    // What the first scan's worker parsed was taken by now (tail).
    _preparsed.clear();
    _preFound.clear();
    scheduleSaveKnown();
    if (_watchTimer)
        syncWatches(); // sessions and transcripts come and go
    pumpTyping(true);
    // Subagent transcripts are kept read only for the sessions that look at
    // them: a live one (its subagents' runs) and one whose list is loaded
    // (their threads and reply counts).
    std::unordered_set<std::string> keep;
    for (const auto &[id, tp] : _sessions)
        if (!tp->transcriptPath.empty() &&
            (tp->info.running || (tp->ref != kNoConv && _shown.count({tp->ref, 0}))))
            keep.insert(Paths::subagentsDir(tp->transcriptPath));
    std::erase_if(_subagents, [&](const auto &e) {
        return !keep.count(std::string(file::dirName(e.first)));
    });
}

// A removed session's transcript grew by more than a glance reads: whether
// that was a turn (it comes back) is looked for on a worker.
void Backend::checkHiddenOnWorker(
    const std::string &sessionId, const std::string &path, int64_t from, int64_t size
) {
    Hidden &h  = _hidden[sessionId];
    h.checking = true;
    auto found = std::make_shared<bool>(false);
    model::runInBackground(
        _app,
        [found, path, from, afterMs = h.seenSize >= 0 ? int64_t(0) : h.atMs] {
            *found = hasTurnSince(path, from, afterMs);
        },
        [this, alive = _alive, found, sessionId, size, atMs = h.atMs] {
            if (!*alive)
                return;
            const auto it = _hidden.find(sessionId);
            if (it == _hidden.end())
                return;
            it->second.checking = false;
            if (it->second.atMs != atMs)
                return; // removed again meanwhile: looked at anew
            if (*found) {
                _hidden.erase(it);
                scheduleRefresh(); // listed again
            } else {
                it->second.seenSize = size;
                scheduleSaveKnown();
            }
        }
    );
}

// A teammate shows as working while any of its sessions does, or a subagent
// started as it does — which no roster scan need notice: pumpTyping asks too.
void Backend::announceRoles() {
    // Every teammate's dot from one walk over the sessions (as roleBusy and
    // roleUnavailable tell one teammate's): those working, and those of the
    // rest waiting on something besides a message.
    std::unordered_set<std::string> working, waiting;
    for (const auto &[id, tp] : _sessions) {
        const Tracked &t = *tp;
        if (asThread(t))
            continue;
        if (busy(t))
            working.insert(roleOf(t));
        else if (unavailable(t))
            waiting.insert(roleOf(t));
    }
    for (const std::string &role : roleIds()) {
        const bool b = working.count(role) || roleSubagentRunning(role);
        const bool y = !b && waiting.count(role);
        if (_roleBusy[role] == b && _roleUnavailable[role] == y && _firstScanDone)
            continue;
        _roleBusy[role]        = b;
        _roleUnavailable[role] = y;
        putUser(teammateUser(_team.resolve(role)));
    }
}

// Claude working on a turn shows as the session "thinking (8m 58s)" — the
// terminal's spinner. Re-evaluated every 3 s while any session is busy (a
// background subagent's run shows in no roster file).
void Backend::pumpTyping(bool looked) {
    using Key = std::tuple<ConvRef, UserRef, Ts>;
    std::map<Key, int64_t> want;
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (!busy(t)) {
            t.busySinceMs = 0;
            continue;
        }
        // When the turn began, fixed for as long as it runs: msga's own turn
        // from its send, else from when the session's status last changed (it
        // turned busy, possibly before msga was looking) — or, busy under a
        // stale "blocked" (working), from the turn's first record.
        if (t.busySinceMs == 0) {
            const int64_t statusSince = statusIsBusy(t.info.status)
                                            ? t.info.statusSinceMs
                                            : t.parser.turnStartedAt() / 1000;
            t.busySinceMs             = t.sending && t.sendStartedMs > 0 ? t.sendStartedMs
                                        : statusSince > 0 ? std::min(statusSince, nowMs())
                                                          : nowMs();
        }
        // A /btw thread working shows nowhere: "typing" is the session's own.
        if (asThread(t) || t.ref == kNoConv)
            continue;
        want[{t.ref, roleUser(roleOf(t)), 0}] = t.busySinceMs;
    }
    // A background subagent working "thinks" in its thread (findSubagentRuns).
    if (!looked)
        findSubagentRuns();
    for (const SubagentRun &r : _subagentRuns)
        want[{r.ref, r.author, r.root}] = r.since;
    for (const auto &[key, since] : _typing)
        if (!want.count(key))
            _store.setTyping(std::get<0>(key), std::get<1>(key), std::get<2>(key), false);
    // Always set again: a message of the typer's own clears its indicator.
    for (const auto &[key, since] : want)
        _store.setTyping(std::get<0>(key), std::get<1>(key), std::get<2>(key), true, since);
    _typing = std::move(want);
    if (!_typing.empty() && !_typingTimer)
        _typingTimer = _app.addTimer(3000, true, [this, alive = _alive] {
            if (*alive)
                pumpTyping();
        });
    else if (_typing.empty() && _typingTimer) {
        _app.cancelTimer(_typingTimer);
        _typingTimer = 0;
    }
    // A subagent starting or stopping turns its teammate's dot — the stop
    // too: this is the last tick when nothing else works.
    announceRoles();
}

// File watching: the roster dirs (sessions come and go), every live
// session's state file (status changes) and live transcripts (new messages).
// What changed schedules a refresh. On Linux the folders holding them are
// watched (inotify): a change wakes the loop, and the look below runs only
// as a slow safety sweep. Elsewhere — and while a folder can't be watched
// (one not there yet) — it is all by looking, faster while something works,
// so a streamed answer shows as it comes.

namespace {
// The sweep while every folder is watched.
constexpr int kWatchSweepMs = 10'000;
} // namespace

// What watchCheck looks at besides the sessions folder's own files: live
// background sessions' state files and live transcripts.
std::vector<std::string> Backend::watchedFiles(bool *anyBusy) const {
    std::vector<std::string> want;
    for (const auto &[id, tp] : _sessions) {
        const Tracked &t = *tp;
        if (anyBusy)
            *anyBusy = *anyBusy || t.sending || busy(t);
        if ((!t.listed && !t.sending) || t.info.sessionId.empty())
            continue;
        if (t.info.kind == SessionInfo::Kind::Background)
            want.push_back(_paths.jobsDir() + "/" + t.info.sessionId.substr(0, 8) + "/state.json");
        if (!t.transcriptPath.empty())
            want.push_back(t.transcriptPath);
    }
    return want;
}

void Backend::watchCheck() {
    const std::string sessionsDir = _paths.sessionsDir(), jobsDir = _paths.jobsDir();
    // A directory's entries' names say what came and went; the sessions
    // folder's one listing serves for that and for its files alike.
    const auto        names = [](const std::vector<file::DirEntry> &list) {
        uint64_t h = crypto::kFnvOffset;
        for (const auto &e : list)
            h ^= crypto::fnv1a(e.name); // whatever order the OS lists them in
        return int64_t(h);
    };
    syncWatches(); // first: what changes from here on wakes the loop
    std::vector<file::DirEntry> entries;
    file::listDir(sessionsDir, &entries);
    std::vector<std::string> want = watchedFiles(nullptr);
    for (const auto &e : entries)
        if (!e.isDir && str::endsWith(e.name, ".json"))
            want.push_back(sessionsDir + "/" + e.name);
    // Each path's size (a folder: its listing's names) and modification
    // time; -1 and 0 when it's missing.
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> seen;
    file::Stat                                                   s;
    if (file::stat(sessionsDir, &s))
        seen[sessionsDir] = {names(entries), s.mtimeMicros};
    else
        seen[sessionsDir] = {-1, 0};
    if (file::stat(jobsDir, &s)) {
        std::vector<file::DirEntry> jobs;
        file::listDir(jobsDir, &jobs);
        seen[jobsDir] = {names(jobs), s.mtimeMicros};
    } else {
        seen[jobsDir] = {-1, 0};
    }
    for (const auto &p : want)
        seen[p] = file::stat(p, &s) ? std::pair{s.size, s.mtimeMicros}
                                    : std::pair<int64_t, int64_t>{-1, 0};
    const bool changed = seen != _watched;
    _watched           = std::move(seen);
    if (changed)
        scheduleRefresh();
}

void Backend::watchTick() {
    bool anyBusy = false;
    watchedFiles(&anyBusy);
    // Watched, with no loop to wake (a headless app): what it says is read here.
    if (_watchAll && !_inotifyWatch)
        readWatchEvents();
    if (!_watchAll || nowMs() - _watchCheckedMs >= kWatchSweepMs) {
        _watchCheckedMs = nowMs();
        watchCheck();
    }
    const int next = _watchAll && _inotifyWatch ? kWatchSweepMs : anyBusy ? 400 : 1200;
    _watchTimer    = _app.addTimer(next, false, [this, alive = _alive] {
        if (!*alive)
            return;
        _watchTimer = 0;
        watchTick();
    });
}

// The folders of what watchCheck looks at, watched: those no longer of
// interest dropped, new ones added (_watchAll: all of them are).
void Backend::syncWatches() {
#if defined(__linux__)
    if (_inotify == -1) {
        _inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (_inotify < 0) {
            _inotify = -2; // never: looked at instead
            return;
        }
        _inotifyWatch = _app.watchFd(_inotify, plat::FdRead, [this, alive = _alive](uint32_t) {
            if (*alive)
                readWatchEvents();
        });
    }
    if (_inotify < 0)
        return;
    std::vector<std::string> files = watchedFiles(nullptr);
    std::vector<std::string> dirs{_paths.sessionsDir(), _paths.jobsDir()};
    for (const std::string &f : files)
        if (const std::string d(file::dirName(f));
            std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(d);
    _watchFiles = {files.begin(), files.end()};
    for (auto it = _watchDirs.begin(); it != _watchDirs.end();)
        if (std::find(dirs.begin(), dirs.end(), it->first) == dirs.end()) {
            inotify_rm_watch(_inotify, int(it->second));
            it = _watchDirs.erase(it);
        } else {
            ++it;
        }
    constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
                               IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
    bool               all = true, added = false;
    for (const std::string &d : dirs) {
        if (_watchDirs.count(d))
            continue;
        const int wd = inotify_add_watch(_inotify, d.c_str(), kMask);
        if (wd < 0) {
            all = false; // not there (yet): looked at instead
            continue;
        }
        _watchDirs[d] = wd;
        added         = true;
    }
    // What a new folder had written before it was watched isn't missed.
    if (added && _watchAll)
        scheduleRefresh();
    _watchAll = all;
#endif
}

// What inotify says: a change to anything watchCheck looks at schedules a
// refresh, as a look that saw it would.
void Backend::readWatchEvents() {
#if defined(__linux__)
    if (_inotify < 0)
        return;
    alignas(inotify_event) char buf[4096];
    bool                        changed = false;
    for (;;) {
        const ssize_t n = ::read(_inotify, buf, sizeof buf);
        if (n <= 0)
            break;
        for (ssize_t at = 0; at < n;) {
            const auto *e = reinterpret_cast<const inotify_event *>(buf + at);
            at += ssize_t(sizeof(inotify_event) + e->len);
            if (changed)
                continue;
            if (e->mask & (IN_Q_OVERFLOW | IN_IGNORED)) {
                // Events lost, or a folder gone (watched again once it's
                // back): whatever changed, a refresh finds.
                if (e->mask & IN_IGNORED)
                    std::erase_if(_watchDirs, [&](const auto &w) { return w.second == e->wd; });
                changed = true;
                continue;
            }
            const auto dir = std::find_if(_watchDirs.begin(), _watchDirs.end(), [&](const auto &w) {
                return w.second == e->wd;
            });
            if (dir == _watchDirs.end())
                continue;
            const std::string_view name = e->len ? std::string_view(e->name) : std::string_view();
            constexpr uint32_t     kCameOrWent =
                IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
            if (dir->first == _paths.sessionsDir() ||
                (dir->first == _paths.jobsDir() && (e->mask & kCameOrWent)) ||
                (!name.empty() && _watchFiles.count(str::concat({dir->first, "/", name}))))
                changed = true;
        }
    }
    if (changed)
        scheduleRefresh();
#endif
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void Backend::connect(Done done) {
    _store.workspaceId        = kWorkspaceId;
    _store.workspaceName      = "Claude Code";
    _store.workspaceIcon      = agentAvatarPath();
    _store.answersAreMentions = true;
    _store.setSections(_sections);
    syncUsers();
    if (!_started) {
        firstScan(std::move(done));
        return;
    }
    postDone(std::move(done), true);
}

// The first scan reads every session msga remembers or the roster lists —
// whole transcripts, tens of MB in all — so it reads and parses them on a
// worker, and the refresh that lists them (on the UI thread) takes what the
// worker made (tail: _preparsed) and reads only what came since.
void Backend::firstScan(Done done) {
    _connectDone.push_back(std::move(done));
    if (_connecting)
        return; // under way: done with it
    _connecting = true;
    struct Want {
        std::string sessionId, transcript;
    };
    struct Job {
        std::vector<Want>                                               want;
        std::vector<std::pair<std::string, std::string>>                found;  // id → transcript
        std::vector<std::pair<std::string, std::shared_ptr<Preparsed>>> parsed; // by transcript
    };
    auto job = std::make_shared<Job>();
    for (const auto &[id, tp] : _sessions)
        if (!tp->info.sessionId.empty())
            job->want.push_back({tp->info.sessionId, tp->transcriptPath});
    model::runInBackground(
        _app,
        [job, paths = _paths] {
            for (const SessionInfo &s : scanSessions(paths))
                job->want.push_back({s.sessionId, s.transcriptPath});
            std::unordered_set<std::string> read;
            for (const Want &w : job->want) {
                std::string path = w.transcript;
                if (path.empty() || !file::exists(path)) {
                    path = paths.findTranscript(w.sessionId);
                    if (path.empty())
                        continue;
                    job->found.emplace_back(w.sessionId, path);
                }
                if (!read.insert(path).second)
                    continue;
                auto p    = std::make_shared<Preparsed>();
                p->offset = feedTranscript(path, p->parser);
                if (p->offset >= 0)
                    job->parsed.emplace_back(path, std::move(p));
            }
        },
        [this, alive = _alive, job] {
            if (!*alive)
                return;
            for (auto &[id, path] : job->found)
                _preFound[id] = path;
            for (auto &[path, p] : job->parsed)
                _preparsed[path] = std::move(p);
            _connecting = false;
            _started    = true;
            refresh();
#if defined(__GLIBC__)
            // Parsing the whole transcripts peaks at well over 100 MB on the
            // worker, nearly all of it freed by now but kept by that thread's
            // malloc arena; hand the pages back to the system.
            model::runInBackground(_app, [] { malloc_trim(0); }, [] {});
#endif
            // File watching can miss what a look in between saw already; a
            // slow sweep catches whatever it missed.
            _safetyPoll = _app.addTimer(10'000, true, [this, alive = _alive] {
                if (!*alive)
                    return;
                _syncAll = true; // whatever a skipped sync would have missed
                refresh();
            });
            watchTick();
            for (Done &d : std::exchange(_connectDone, {}))
                if (d)
                    d(true, {});
        }
    );
}

void Backend::loadHistory(ConvRef conv, Ts before, Done done) {
    Tracked *t = findRef(conv);
    if (!t) {
        postDone(std::move(done), false, "not found");
        return;
    }
    if (t->parsing) { // its transcript is being read on a worker: served once it is
        whenParsed(*t, [this, conv, before, done = std::move(done)]() mutable {
            loadHistory(conv, before, std::move(done));
        });
        return;
    }
    if (!t->announcedInit)
        sync(*t);
    // Listed whole, rendered only as far as the page goes.
    auto list = visibleList(*t, kRenderNone);
    // Replies in subagent threads are in the threads (loadThread).
    std::erase_if(list, [](const Visible &v) { return v.threadTs != 0; });
    // Newest page first, then the 200 before `before`.
    constexpr size_t kPage = 200;
    size_t           end   = list.size();
    if (before)
        end = size_t(
            std::lower_bound(
                list.begin(), list.end(), before, [](const Visible &v, Ts b) { return v.ts < b; }
            ) -
            list.begin()
        );
    const size_t begin = end > kPage ? end - kPage : 0;
    for (size_t i = begin; i < end; ++i)
        realize(*t, list[i]);
    applyReactions(t->convId, list);
    Shown &s = _shown[{conv, 0}];
    if (!before)
        s = {};
    std::vector<Message> page;
    page.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        s.fp[list[i].ts] = list[i].fp;
        page.push_back(make(list[i]));
    }
    s.from =
        begin < end ? list[begin].ts : (before ? before : (list.empty() ? 0 : list.back().ts + 1));
    if (!before && begin == end)
        s.from = 0;
    _store.addPage(conv, std::move(page));
    _store.updateConversation(conv, [&](model::Conversation &c) { c.hasMoreBefore = begin > 0; });
    postDone(std::move(done), true);
}

void Backend::loadThread(ConvRef conv, Ts root, Done done) {
    Tracked *t = findRef(conv);
    if (t && t->parsing) { // as loadHistory
        whenParsed(*t, [this, conv, root, done = std::move(done)]() mutable {
            loadThread(conv, root, std::move(done));
        });
        return;
    }
    std::vector<Visible> list;
    if (t) {
        if (Tracked *f = forkFor(t->convId, root))
            list = threadList(*f); // a /btw thread: the rest of the branch
        else
            list = subagentList(*t, root, visibleList(*t, kRenderNone));
    }
    Shown &s = _shown[{conv, root}];
    s        = {};
    std::vector<Message> replies;
    replies.reserve(list.size());
    for (const auto &v : list) {
        s.fp[v.ts] = v.fp;
        replies.push_back(make(v));
    }
    _store.addPage(conv, std::move(replies));
    postDone(std::move(done), t != nullptr, t ? std::string() : std::string("not found"));
}

void Backend::setActiveConversation(ConvRef conv, Ts) {
    const Tracked    *t  = findRef(conv);
    const std::string id = t ? t->convId : std::string();
    if (id != _lastConv && !id.empty()) {
        _lastConv = id;
        scheduleSaveKnown();
    }
    // Its prompt history (↑), read ahead.
    if (t && !t->info.cwd.empty())
        readAheadHistory(t->info.cwd);
}

// On a worker: the first look at a folder's reads all of history.jsonl, later
// ones what was added since. One read per folder at a time.
void Backend::readAheadHistory(const std::string &dir) {
    if (std::find(_historyReads.begin(), _historyReads.end(), dir) != _historyReads.end())
        return;
    _historyReads.push_back(dir);
    model::runInBackground(
        _app,
        [home = _paths.home, dir] {
            claude::promptHistory(home + "/history.jsonl", home + "/paste-cache", dir, {}, 0);
        },
        [this, alive = _alive, dir] {
            if (*alive)
                std::erase(_historyReads, dir);
        }
    );
}

bool Backend::isAgentSession(ConvRef conv) const {
    return findRef(conv) != nullptr;
}

std::string Backend::promptSuggestion(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    // Only while it still waits for that answer: once msga sends one, or the
    // reply typed in a terminal is under way, it answers nothing any more.
    if (!t || !needsUser(*t) || readOnly(*t) || awaitsApproval(t->info))
        return {};
    return t->info.suggestedReply;
}

std::string Backend::agentSessionFolder(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    return t ? t->info.cwd : std::string();
}

std::string Backend::agentSessionRole(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    return t ? roleOf(*t) : std::string();
}

void Backend::markRead(ConvRef conv, Ts ts) {
    Tracked *t = findRef(conv);
    if (!t || (t->lastRead && ts <= t->lastRead))
        return;
    t->lastRead = ts;
    syncMeta(*t);
    scheduleSaveKnown();
}

void Backend::markUnread(ConvRef conv, Ts ts) {
    Tracked *t = findRef(conv);
    if (!t || ts <= 0)
        return;
    t->lastRead = ts - 1;
    syncMeta(*t);
    scheduleSaveKnown();
}

void Backend::setStarred(ConvRef conv, bool starred) {
    if (starred)
        leaveSection(conv);
    if (Tracked *t = findRef(conv)) {
        t->starred = starred;
        syncMeta(*t);
        scheduleSaveKnown();
    }
}

void Backend::sectionsChanged() {
    _sections = _store.sections();
    scheduleSaveKnown();
}

void Backend::setMuted(ConvRef conv, bool muted) {
    if (Tracked *t = findRef(conv)) {
        t->muted = muted;
        syncMeta(*t);
        scheduleSaveKnown();
    }
}

void Backend::setNotifyLevel(ConvRef conv, model::NotifyLevel level) {
    if (Tracked *t = findRef(conv)) {
        t->notify = level;
        syncMeta(*t);
        scheduleSaveKnown();
    }
}

// "Rename session…": a name only msga shows (Claude Code keeps its own). It
// goes on the session's user too, which titles the DM everywhere.
void Backend::setLocalName(ConvRef conv, std::string name) {
    Tracked *t = findRef(conv);
    if (!t)
        return;
    name = std::string(str::trim(name));
    if (t->localName == name)
        return;
    t->localName = std::move(name);
    syncMeta(*t);
    saveKnown();
}

// Zen mode hides the tool-call cards: what each loaded list shows is taken
// again, and what the switch hides or reveals isn't news.
void Backend::setZenMode(bool on) {
    if (_zen == on)
        return;
    _zen = on;
    for (auto &[id, tp] : _sessions)
        if (tp->announcedInit)
            sync(*tp);
    for (auto &[id, tp] : _sessions)
        syncThreads(*tp);
}

} // namespace claude
