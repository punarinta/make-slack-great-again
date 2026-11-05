#include "app/cache/workspace_cache.h"

#include "app/identity.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/crypto.h"
#include "base/log.h"
#include "base/str.h"
#include "plat/plat.h"

#include <atomic>
#include <initializer_list>
#include <memory>
#include <mutex>

namespace cache {

using model::ConvRef;
using model::kNoUser;
using model::Ts;
using model::UserRef;

namespace {

constexpr int64_t kVersion = 1;

// clearAll() calls so far: a cache opened before the last one writes nothing
// (workers check it too).
std::atomic<uint32_t> g_clearGen{0};
int                   g_slowDelayMs = WorkspaceCache::kSlowDelayMs;

// Held around every cache file write and every wipe: a wipe never races a
// write under way on a worker (it would bring the directory back), and two
// writes of one file never cross. Never destroyed: a worker may still hold
// it as the process exits.
std::mutex &diskMutex() {
    static std::mutex *m = new std::mutex;
    return *m;
}

// ── Field lists ─────────────────────────────────────────────────────────────
//
// One list per record type, run in both directions by one IO: writing, it
// appends each field to a JSON array; reading, it takes them back in the
// same order (a missing trailing field reads as its default, so a record can
// only grow at the end). Change a list in the middle and bump kVersion. One
// IO with a mode, not one IO type per direction: every list exists once in
// the binary. The third mode only hashes the fields (has a record changed?).

class IO {
public:
    IO(json::Writer &w, const model::Store &s) : _w(&w), _s(const_cast<model::Store *>(&s)) {}
    // Reading: start() each record (one IO for a whole file: its stack is
    // allocated once).
    explicit IO(model::Store &s) : _s(&s) {}
    // Hashing into hash[0], or hash[1] between cursors(true) and
    // cursors(false).
    IO(const model::Store &s, bool) : _s(const_cast<model::Store *>(&s)), _hashing(true) {}

    void start(json::Value record) {
        _stack.clear();
        _stack.push_back({record.begin(), record.end()});
    }
    bool     reading() const { return !_w && !_hashing; }
    void     cursors(bool on) { _part = on; }
    uint64_t hash[2] = {crypto::kFnvOffset, crypto::kFnvOffset};
    void     str(std::string &v);
    void     num(int64_t &v);
    void     num(int32_t &v);
    void     num(uint32_t &v);
    void     byte(uint8_t &v);
    void     flags(std::initializer_list<bool *> f);
    void     user(UserRef &r);
    // A nested array: its element count when reading, `n` when writing.
    size_t   open(size_t n);
    void     close();
    // A part that may be absent (null): true if present.
    bool     optional(bool present);

private:
    struct Level {
        json::Value::Iterator it, end;
    };
    json::Value next();
    int64_t     integer(int64_t v); // writes v, or reads the next one
    void        fold(int64_t v) {
        hash[_part] =
            crypto::fnv1a(std::string_view(reinterpret_cast<const char *>(&v), 8), hash[_part]);
    }
    json::Writer      *_w = nullptr;
    model::Store      *_s;
    bool               _hashing = false, _part = false;
    std::vector<Level> _stack;
};

json::Value IO::next() {
    Level &l = _stack.back();
    if (!(l.it != l.end))
        return {};
    const json::Value v = *l.it;
    ++l.it;
    return v;
}

int64_t IO::integer(int64_t v) {
    if (_w) {
        _w->value(v);
        return v;
    }
    if (_hashing) {
        fold(v);
        return v;
    }
    return next().integer();
}

void IO::str(std::string &v) {
    if (_w) {
        _w->value(v);
    } else if (_hashing) {
        fold(int64_t(v.size()));
        hash[_part] = crypto::fnv1a(v, hash[_part]);
    } else {
        v = std::string(next().str());
    }
}

void IO::num(int64_t &v) {
    v = integer(v);
}
void IO::num(int32_t &v) {
    v = int32_t(integer(v));
}
void IO::num(uint32_t &v) {
    v = uint32_t(integer(v));
}
void IO::byte(uint8_t &v) {
    v = uint8_t(integer(v));
}

void IO::flags(std::initializer_list<bool *> f) {
    int64_t bits = 0, bit = 1;
    for (bool *b : f) {
        bits |= *b ? bit : 0;
        bit <<= 1;
    }
    bits = integer(bits);
    bit  = 1;
    for (bool *b : f) {
        *b = (bits & bit) != 0;
        bit <<= 1;
    }
}

void IO::user(UserRef &r) {
    if (_w) {
        _w->value(_s->user(r).id);
        return;
    }
    if (_hashing) { // a ref names the same user for the Store's life
        fold(r);
        return;
    }
    const std::string_view id = next().str();
    r                         = id.empty() ? kNoUser : _s->internUser(id);
}

size_t IO::open(size_t n) {
    if (_w || _hashing) {
        if (_w)
            _w->beginArray();
        else
            fold(int64_t(n));
        return n;
    }
    const json::Value v = next();
    _stack.push_back({v.begin(), v.end()});
    return v.isArray() ? v.size() : 0;
}

void IO::close() {
    if (_w)
        _w->endArray();
    else if (!_hashing)
        _stack.pop_back();
}

bool IO::optional(bool present) {
    if (_w || _hashing) {
        if (_hashing)
            fold(present);
        else if (!present)
            _w->null();
        return present;
    }
    Level &l = _stack.back();
    if (l.it != l.end && (*l.it).isArray())
        return true;
    next(); // null: absent
    return false;
}

void fields(IO &io, model::User &u);
void fields(IO &io, model::Reaction &r);
void fields(IO &io, model::File &f);
void fields(IO &io, model::AttachmentField &f);
void fields(IO &io, model::Block &b);
void fields(IO &io, model::Button &b);
void fields(IO &io, model::Attachment &a);
void fields(IO &io, model::Message &m);
void fields(IO &io, model::Conversation &c);

// A list of records, each its own array.
template <class T>
void records(IO &io, std::vector<T> &v) {
    const size_t n = io.open(v.size());
    if (io.reading())
        v.resize(n);
    for (T &x : v) {
        io.open(0);
        fields(io, x);
        io.close();
    }
    io.close();
}

void users(IO &io, std::vector<UserRef> &v) {
    const size_t n = io.open(v.size());
    if (io.reading())
        v.assign(n, kNoUser);
    for (UserRef &u : v)
        io.user(u);
    io.close();
}

void fields(IO &io, model::User &u) {
    io.str(u.id);
    io.str(u.name);
    io.str(u.displayName);
    io.str(u.title);
    io.str(u.email);
    io.str(u.avatar);
    io.str(u.statusEmoji);
    io.str(u.statusText);
    io.num(u.tzOffset);
    // Presence too: the dots show until the first poll.
    io.flags({&u.hasTz, &u.active, &u.bot, &u.admin, &u.owner, &u.deleted, &u.stranger});
    io.str(u.realName);
    io.str(u.profileName);
}

void fields(IO &io, model::Reaction &r) {
    io.str(r.name);
    io.num(r.count);
    users(io, r.users);
}

void fields(IO &io, model::File &f) {
    io.str(f.id);
    io.str(f.name);
    io.str(f.mime);
    io.str(f.prettyType);
    io.str(f.path);
    io.str(f.subtype);
    io.str(f.transcript);
    io.num(f.size);
    io.num(f.width);
    io.num(f.height);
    io.num(f.durationMs);
    io.str(f.original);
    io.str(f.permalink);
    io.str(f.thumb);
    io.str(f.title);
    io.str(f.transcriptVtt);
}

void fields(IO &io, model::AttachmentField &f) {
    io.str(f.title);
    io.str(f.value);
}

void fields(IO &io, model::Block &b) {
    io.byte(reinterpret_cast<uint8_t &>(b.kind));
    io.str(b.text);
    io.str(b.image);
    io.str(b.alt);
    io.num(b.width);
    io.num(b.height);
    const size_t n = io.open(b.rows.size());
    if (io.reading())
        b.rows.resize(n);
    for (std::vector<std::string> &row : b.rows) {
        const size_t k = io.open(row.size());
        if (io.reading())
            row.resize(k);
        for (std::string &cell : row)
            io.str(cell);
        io.close();
    }
    io.close();
}

void fields(IO &io, model::Button &b) {
    io.str(b.id);
    io.str(b.label);
    io.byte(reinterpret_cast<uint8_t &>(b.style));
    io.str(b.url);
    io.str(b.blockId);
    io.str(b.value);
    io.num(b.attachment);
}

void fields(IO &io, model::Attachment &a) {
    io.str(a.color);
    io.str(a.pretext);
    io.str(a.author);
    io.str(a.title);
    io.str(a.link);
    io.str(a.text);
    io.str(a.service);
    io.str(a.favicon);
    io.str(a.footer);
    io.str(a.image);
    io.num(a.imageWidth);
    io.num(a.imageHeight);
    records(io, a.fields);
    io.flags({&a.linkPreview, &a.msgUnfurl, &a.app});
    io.num(a.id);
    io.str(a.authorIcon);
    io.str(a.channel);
    io.num(a.ts);
    records(io, a.blocks);
    records(io, a.files);
}

void fields(IO &io, model::Message &m) {
    io.num(m.ts);
    io.num(m.threadTs);
    io.num(m.latestReply);
    io.user(m.user);
    io.user(m.pinnedBy);
    io.num(m.replyCount);
    io.str(m.text);
    io.flags({&m.edited, &m.pinned, &m.saved});
    users(io, m.replyUsers);
    records(io, m.reactions);
    if (io.optional(m.extra != nullptr)) {
        model::MessageExtras &x = m.extras();
        io.open(0);
        io.str(x.subtype);
        io.str(x.botName);
        io.str(x.botAvatar);
        records(io, x.files);
        records(io, x.attachments);
        records(io, x.blocks);
        io.str(x.botId);
        records(io, x.buttons);
        // A huddle_thread's summary ("hu"); the name line follows
        // the locale when it is drawn.
        users(io, x.huddle.attendees);
        io.num(x.huddle.startSec);
        io.num(x.huddle.endSec);
        io.flags({&x.huddle.ended});
        io.close();
    }
}

void fields(IO &io, model::Conversation &c) {
    io.str(c.id);
    io.str(c.name);
    io.str(c.topic);
    io.byte(reinterpret_cast<uint8_t &>(c.kind));
    io.user(c.dmUser);
    users(io, c.members);
    io.num(c.memberCount);
    io.cursors(true); // what every new message moves (WorkspaceCache::noteRecord)
    io.num(c.unread);
    io.num(c.mentions);
    io.num(c.lastRead);
    io.num(c.latest);
    io.cursors(false);
    io.flags({&c.starred, &c.muted, &c.member, &c.extShared});
    io.byte(reinterpret_cast<uint8_t &>(c.notify));
    io.str(c.canvasTitle);
    io.str(c.localName);
    io.str(c.canvasId);
}

// ── Files ───────────────────────────────────────────────────────────────────

// A file of ours, parsed, if its version is ours.
bool readDoc(const std::string &path, json::Document *doc, uint64_t *hash) {
    std::string text;
    if (!file::readAll(path, &text) || text.empty())
        return false;
    if (hash)
        *hash = crypto::fnv1a(text);
    if (!doc->parse(std::move(text)) || doc->root()["v"].integer() != kVersion) {
        LOG_INFO("cache", "ignoring %s (unreadable or another version)", path.c_str());
        return false;
    }
    return true;
}

// Opens {"v":kVersion,"<key>":[ — the caller closes with endArray/endObject.
void begin(json::Writer &w, const char *key) {
    w.beginObject().key("v").value(kVersion).key(key).beginArray();
}

std::string safeName(std::string_view s) {
    std::string out(s);
    for (char &ch : out)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.'))
            ch = '_';
    return out == "." || out == ".." ? std::string("_") : out;
}

// One conversation's roster record hashed: [0] all but the read cursors
// and badges, [1] those.
struct RecordHash {
    uint64_t stable, cursors;
};
RecordHash hashRecord(const model::Store &s, ConvRef c) {
    IO io(s, true);
    fields(io, const_cast<model::Conversation &>(s.conversation(c)));
    return {io.hash[0], io.hash[1]};
}

// usergroups.json's records: [id, handle, name, [], mine]. Before, [id,
// handle, name, [user ids]] (meta.json's "x"."ug" held those): the members
// say whether I am in it (setUsergroups), a record can only grow at the end.
void readUsergroups(const json::Value &arr, std::vector<model::Store::Usergroup> *out) {
    for (const json::Value v : arr) {
        model::Store::Usergroup g{
            std::string(v[0].str()),
            std::string(v[1].str()),
            std::string(v[2].str()),
            {},
            v[4].boolean()
        };
        for (const json::Value u : v[3])
            g.users.emplace_back(u.str());
        if (!g.id.empty())
            out->push_back(std::move(g));
    }
}

} // namespace

// ── The write queue ─────────────────────────────────────────────────────────
//
// flush() queues each file's bytes here and has a worker drain the queue;
// close() drains what is left itself, so nothing of a closed cache is written
// later (a sign-out removes the directory right after). Shared with the
// workers: the cache may go while one runs.

struct WorkspaceCache::Disk {
    struct File {
        std::string name, data;
    };
    std::string              dir;
    uint32_t                 clearGen = 0;
    std::mutex               mutex; // the members below (never held while writing)
    std::vector<File>        queue; // oldest first; one entry per file
    std::vector<std::string> failed;
    bool                     draining = false; // a worker is asked to drain

    // Writes the queue out, oldest first.
    void drain() {
        std::lock_guard<std::mutex> io(diskMutex());
        for (;;) {
            File f;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (queue.empty()) {
                    draining = false;
                    return;
                }
                f = std::move(queue.front());
                queue.erase(queue.begin());
            }
            if (clearGen != g_clearGen.load())
                continue; // Clear cache ran since: nothing more
            if (!file::writeAtomic(file::join(dir, f.name), f.data, 0600, false)) {
                LOG_WARN("cache", "can't write %s/%s", dir.c_str(), f.name.c_str());
                std::lock_guard<std::mutex> lock(mutex);
                failed.push_back(std::move(f.name));
            }
        }
    }
};

// ── WorkspaceCache ──────────────────────────────────────────────────────────

WorkspaceCache::WorkspaceCache(plat::App &app, model::Store &store, std::string dir)
    : _app(app), _store(store), _dir(std::move(dir)), _clearGen(g_clearGen),
      _disk(std::make_shared<Disk>()) {
    _disk->dir      = _dir;
    _disk->clearGen = _clearGen;
}

void WorkspaceCache::setSlowDelayForTest(int ms) {
    g_slowDelayMs = ms > 0 ? ms : kSlowDelayMs;
}

WorkspaceCache::~WorkspaceCache() {
    close(true);
}

std::string WorkspaceCache::root(plat::App &app) {
    const std::string base = identity::cacheDir(app);
    return base.empty() ? std::string() : file::join(base, "workspaces");
}

std::string WorkspaceCache::dirFor(plat::App &app, std::string_view key) {
    const std::string r = root(app);
    // "slack:T0123" → "slack_T0123" (':' can't be in a Windows path).
    return r.empty() || key.empty() ? std::string() : file::join(r, safeName(key));
}

int64_t WorkspaceCache::diskBytes(plat::App &app) {
    const std::string r = root(app);
    return r.empty() ? 0 : file::treeBytes(r);
}

void WorkspaceCache::clearAll(plat::App &app) {
    ++g_clearGen;
    std::lock_guard<std::mutex> io(diskMutex());
    if (const std::string r = root(app); !r.empty())
        file::removeTree(r);
}

void WorkspaceCache::diskBytesAsync(plat::App &app, std::function<void(int64_t)> done) {
    auto bytes = std::make_shared<int64_t>(0);
    model::runInBackground(
        app,
        [r = root(app), bytes] { *bytes = r.empty() ? 0 : file::treeBytes(r); },
        [bytes, done = std::move(done)] {
            if (done)
                done(*bytes);
        }
    );
}

void WorkspaceCache::clearAllAsync(plat::App &app, std::function<void()> done) {
    ++g_clearGen; // at once: caches still open write nothing more
    model::runInBackground(
        app,
        [r = root(app)] {
            std::lock_guard<std::mutex> io(diskMutex());
            if (!r.empty())
                file::removeTree(r);
        },
        std::move(done)
    );
}

void WorkspaceCache::remove(plat::App &app, std::string_view key) {
    std::lock_guard<std::mutex> io(diskMutex());
    if (const std::string d = dirFor(app, key); !d.empty())
        file::removeTree(d);
}

bool WorkspaceCache::writable() const {
    return !_dir.empty() && _observer && _clearGen == g_clearGen;
}

bool WorkspaceCache::load(json::Document *meta) {
    if (_observer || _dir.empty())
        return false;
    // Observe from here on, whatever was found: the backend's first answers
    // write the cache of a cold start.
    const auto observe = [this] {
        _observer = _store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
            onChange(ch);
        });
    };
    json::Document roster, users, emoji, groups;
    uint64_t       hRoster = 0, hUsers = 0, hEmoji = 0, hMeta = 0, hGroups = 0;
    if (!readDoc(file::join(_dir, "roster.json"), &roster, &hRoster) ||
        roster.root()["c"].size() == 0) {
        observe();
        return false;
    }
    _written["roster.json"] = hRoster;
    // Users before conversations: their refs resolve to the real records.
    if (readDoc(file::join(_dir, "users.json"), &users, &hUsers)) {
        _written["users.json"] = hUsers;
        IO io(_store);
        for (const json::Value rec : users.root()["u"]) {
            model::User u;
            io.start(rec);
            fields(io, u);
            if (!u.id.empty())
                _store.addUser(std::move(u));
        }
    }
    std::vector<model::Conversation> convs;
    convs.reserve(roster.root()["c"].size());
    IO io(_store);
    for (const json::Value rec : roster.root()["c"]) {
        model::Conversation c;
        io.start(rec);
        fields(io, c);
        if (c.id.empty())
            continue;
        if (uint8_t(c.kind) > uint8_t(model::ConvKind::Group))
            c.kind = model::ConvKind::Channel;
        if (uint8_t(c.notify) > uint8_t(model::NotifyLevel::All))
            c.notify = model::NotifyLevel::Default;
        // Nothing newer than the read cursor: a badge kept from a merge
        // would never clear (opening the chat has nothing left to mark).
        if (c.latest && c.lastRead >= c.latest)
            c.unread = c.mentions = 0;
        convs.push_back(std::move(c));
    }
    _store.addConversations(std::move(convs)); // one Roster emit
    if (readDoc(file::join(_dir, "emoji.json"), &emoji, &hEmoji)) {
        _written["emoji.json"]                           = hEmoji;
        std::unordered_map<std::string, std::string> all = _store.customEmoji();
        for (const json::Value e : emoji.root()["e"])
            if (!e.key().empty())
                all[std::string(e.key())] = std::string(e.str());
        _store.replaceCustomEmoji(std::move(all)); // one change, not one per emoji
    }
    json::Document m;
    const bool     haveMeta = readDoc(file::join(_dir, "meta.json"), &m, &hMeta);
    if (haveMeta) {
        _written["meta.json"] = hMeta;
        const json::Value r   = m.root();
        if (const std::string_view me = r["me"].str(); !me.empty())
            _store.me = _store.internUser(me);
        _last = std::string(r["last"].str());
        _store.myGroups.clear();
        for (const json::Value g : r["groups"])
            _store.myGroups.emplace_back(g.str());
        for (const json::Value t : r["muted"])
            if (const ConvRef c = _store.findConversation(t[0].str()); c != model::kNoConv)
                _store.setThreadMuted(c, t[1].integer(), true);
        for (const json::Value t : r["rem"])
            if (const ConvRef c = _store.findConversation(t[0].str()); c != model::kNoConv)
                _store.setReminderAt(c, t[1].integer(), t[2].integer());
        for (const json::Value t : r["ai"])
            _store.setAiTranscript(
                std::string(t.key()), std::string(t[0].str()), std::string(t[1].str())
            );
        for (const json::Value d : r["drafts"])
            if (const ConvRef c = _store.findConversation(d[0].str()); c != model::kNoConv) {
                std::vector<std::string> files;
                for (const json::Value f : d[3])
                    files.emplace_back(f.str());
                _store.setDraft(c, d[1].integer(), std::string(d[2].str()), std::move(files));
            }
    }
    // The user groups: their own file, or where meta.json kept them before.
    std::vector<model::Store::Usergroup> ug;
    bool                                 migrate = false;
    if (readDoc(file::join(_dir, "usergroups.json"), &groups, &hGroups)) {
        _written["usergroups.json"] = hGroups;
        readUsergroups(groups.root()["g"], &ug);
    } else if (haveMeta) {
        readUsergroups(m.root()["x"]["ug"], &ug);
        migrate = !ug.empty();
    }
    if (!ug.empty())
        _store.setUsergroups(std::move(ug));
    if (meta && haveMeta)
        *meta = std::move(m);
    _store.usersChanged();                      // one repaint for the whole roster
    _usersNamedRev    = _store.namedRevision(); // what users.json holds
    _usersPresenceRev = _store.presenceRevision();
    _localRev         = _store.localRevision(); // what meta.json holds
    _groupsRev        = _store.usergroupRevision();
    // What roster.json holds: the first Meta of each conversation compares
    // with it, not with nothing.
    _convHash.resize(_store.conversationCount());
    _cursorHash.resize(_store.conversationCount());
    for (ConvRef c = 0; c < _store.conversationCount(); ++c) {
        const RecordHash h = hashRecord(_store, c);
        _convHash[c]       = h.stable;
        _cursorHash[c]     = h.cursors;
    }
    observe();
    if (migrate) // the new file now; meta.json drops them with its next write
        mark(kGroups | kMeta);
    LOG_INFO(
        "cache",
        "%s: %zu conversations, %zu users from the cache",
        _dir.c_str(),
        _store.conversationCount(),
        _store.userCount()
    );
    return true;
}

std::string WorkspaceCache::messagesFile(ConvRef c) const {
    return str::concat({"messages/", safeName(_store.conversation(c).id), ".json"});
}

Ts WorkspaceCache::loadMessages(ConvRef c) {
    if (_dir.empty() || c >= _store.conversationCount())
        return 0;
    if (_checked.size() <= c)
        _checked.resize(c + 1, 0);
    if (_checked[c])
        return 0;
    _checked[c] = 1;
    track(c);
    const std::string name = messagesFile(c);
    json::Document    doc;
    uint64_t          hash = 0;
    if (!readDoc(file::join(_dir, name), &doc, &hash))
        return 0;
    _written[name] = hash;
    std::vector<model::Message> page;
    IO                          io(_store);
    for (const json::Value rec : doc.root()["m"]) {
        model::Message m;
        io.start(rec);
        fields(io, m);
        if (m.ts <= 0)
            continue;
        if (m.isReply())
            m.threadTs = 0; // a top-level list: a page must not mix in replies
        page.push_back(std::move(m));
    }
    if (page.empty())
        return 0;
    Ts newest = 0;
    for (const model::Message &m : page)
        newest = std::max(newest, m.ts);
    const bool wasEmpty = _store.conversation(c).messages.empty();
    _store.addPage(c, std::move(page));
    // The cache held the whole history: no "load older" from the top of it.
    if (wasEmpty && !doc.root()["more"].boolean(true))
        _store.updateConversation(c, [](model::Conversation &x) { x.hasMoreBefore = false; });
    return newest;
}

void WorkspaceCache::track(ConvRef c) {
    if (c >= _store.conversationCount() || tracked(c))
        return;
    if (_tracked.size() <= c)
        _tracked.resize(c + 1, 0);
    _tracked[c] = 1;
}

void WorkspaceCache::setLastConversation(ConvRef c) {
    if (c >= _store.conversationCount() || _store.conversation(c).id == _last)
        return;
    _last = _store.conversation(c).id;
    mark(kMeta);
}

void WorkspaceCache::extrasChanged() {
    mark(kMeta);
}

void WorkspaceCache::emojiChanged() {
    mark(kEmoji);
}

// After a Meta of conversation c: its record changed (written within a
// second), or only its read cursors and badges did (every new message moves
// them: the slow cadence, else each message would cost the UI thread a
// pass over every conversation).
void WorkspaceCache::noteRecord(ConvRef c) {
    if (c >= _store.conversationCount() || (_dirty & kConvs))
        return;
    if (_convHash.size() <= c) {
        _convHash.resize(c + 1, 0);
        _cursorHash.resize(c + 1, 0);
    }
    const RecordHash h = hashRecord(_store, c);
    if (h.stable != _convHash[c])
        mark(kConvs);
    else if (h.cursors != _cursorHash[c] && !(_slow & kConvs))
        slow(kConvs);
}

void WorkspaceCache::onChange(const model::Change &ch) {
    using K = model::ChangeKind;
    // The app-local marks (muted threads, reminders, AI transcripts) ride
    // Meta and Update emits; meta.json only when one of them moved.
    if (_store.localRevision() != _localRev && ch.kind != K::Typing) {
        _localRev = _store.localRevision();
        mark(kMeta);
    }
    switch (ch.kind) {
    case K::Roster:
        mark(kConvs);
        return;
    case K::Meta:
        // Most Meta emits (a huddle, paging) leave what roster.json holds
        // alone.
        if (ch.conv != model::kNoConv)
            noteRecord(ch.conv);
        return;
    case K::Users:
        // Only a profile change: presence/DND flips, placeholders, emoji and
        // channel names don't change what the next start needs at once. The
        // first users.json is written at once, later ones slowly.
        if (_store.namedRevision() != _usersNamedRev) {
            if (_written.count("users.json"))
                slow(kUsers);
            else
                mark(kUsers);
        }
        if (_store.usergroupRevision() != _groupsRev) {
            _groupsRev = _store.usergroupRevision();
            mark(kGroups | kMeta); // meta.json: my groups
        }
        return;
    case K::Typing:
        return;
    default:
        break;
    }
    // A message list changed.
    if (ch.thread == 0 && tracked(ch.conv)) {
        if (_msgDirty.size() <= ch.conv)
            _msgDirty.resize(ch.conv + 1, 0);
        _msgDirty[ch.conv] = 1;
        schedule();
    }
}

void WorkspaceCache::mark(uint8_t what) {
    _dirty |= what;
    schedule();
}

void WorkspaceCache::slow(uint8_t what) {
    _slow |= what;
    if (_slowTimer || !writable())
        return;
    _slowTimer = _app.addTimer(g_slowDelayMs, false, [this] {
        _slowTimer       = 0;
        const uint8_t sl = _slow;
        _slow            = 0;
        mark(sl);
    });
}

// At most one write per kWriteDelayMs: a throttle, not a debounce, so a
// workspace that changes every second still gets written.
void WorkspaceCache::schedule() {
    if (_timer || !writable())
        return;
    _timer = _app.addTimer(kWriteDelayMs, false, [this] {
        _timer = 0;
        writePending();
    });
}

// Queues a file's new bytes for a worker, unless they are what it holds.
void WorkspaceCache::write(const char *name, std::string data) {
    const uint64_t h  = crypto::fnv1a(data);
    const auto     it = _written.find(name);
    if (it != _written.end() && it->second == h)
        return; // unchanged since we read or wrote it
    _written[name] = h;
    Disk &d        = *_disk;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        bool                        queued = false;
        for (Disk::File &f : d.queue)
            if (f.name == name) { // still waiting: the newer bytes instead
                f.data = std::move(data);
                queued = true;
                break;
            }
        if (!queued)
            d.queue.push_back({name, std::move(data)});
        if (d.draining)
            return;
        d.draining = true;
    }
    model::runInBackground(_app, [disk = _disk] { disk->drain(); }, nullptr);
}

void WorkspaceCache::flush() {
    if (_slowTimer) {
        _app.cancelTimer(_slowTimer);
        _slowTimer = 0;
    }
    _dirty |= _slow;
    _slow = 0;
    writePending();
}

void WorkspaceCache::writePending() {
    if (_timer) {
        _app.cancelTimer(_timer);
        _timer = 0;
    }
    const uint8_t dirty = _dirty;
    _dirty              = 0;
    std::vector<uint8_t> msgs;
    msgs.swap(_msgDirty);
    if (!writable())
        return;
    {
        // A write that failed is tried again with the next bytes, same or not.
        Disk                       &d = *_disk;
        std::lock_guard<std::mutex> lock(d.mutex);
        for (const std::string &name : d.failed)
            _written.erase(name);
        d.failed.clear();
    }
    const model::Store &s = _store;
    if (dirty & kConvs) {
        // {"v":1,"c":[record,…]}, each record hashed as it goes: a later
        // Meta that leaves a record as it is writes nothing.
        json::Writer w;
        IO           io(w, s);
        begin(w, "c");
        _convHash.assign(s.conversationCount(), 0);
        _cursorHash.assign(s.conversationCount(), 0);
        for (ConvRef c = 0; c < s.conversationCount(); ++c) {
            io.open(0);
            fields(io, const_cast<model::Conversation &>(s.conversation(c)));
            io.close();
            const RecordHash h = hashRecord(s, c);
            _convHash[c]       = h.stable;
            _cursorHash[c]     = h.cursors;
        }
        w.endArray().endObject();
        write("roster.json", w.take());
        _slow &= uint8_t(~kConvs); // the cursors went with it
    }
    if (dirty & kGroups) {
        json::Writer w;
        begin(w, "g");
        for (const model::Store::Usergroup &g : s.usergroups())
            w.beginArray()
                .value(g.id)
                .value(g.handle)
                .value(g.name)
                .beginArray()
                .endArray()
                .value(g.mine)
                .endArray();
        w.endArray().endObject();
        write("usergroups.json", w.take());
    }
    if (dirty & kUsers) {
        _usersNamedRev    = s.namedRevision();
        _usersPresenceRev = s.presenceRevision();
        _slow &= uint8_t(~kUsers);
        json::Writer w;
        IO           io(w, s);
        begin(w, "u");
        for (size_t i = 0; i < s.userCount(); ++i) {
            model::User &u = const_cast<model::User &>(s.user(UserRef(i)));
            if (u.placeholder || u.id.empty())
                continue; // known by id only: the next start interns it again
            io.open(0);
            fields(io, u);
            io.close();
        }
        w.endArray().endObject();
        write("users.json", w.take());
    }
    if (dirty & kEmoji) {
        json::Writer w;
        w.beginObject().key("v").value(kVersion).key("e").beginObject();
        for (const auto &[name, value] : s.customEmoji())
            w.key(name).value(value);
        w.endObject().endObject();
        write("emoji.json", w.take());
    }
    if (dirty & kMeta) {
        json::Writer w;
        w.beginObject().key("v").value(kVersion);
        w.key("me").value(s.user(s.me).id).key("last").value(_last);
        w.key("groups").beginArray();
        for (const std::string &g : s.myGroups)
            w.value(g);
        w.endArray();
        for (int k = 0; k < 2; ++k) { // muted threads, reminders
            w.key(k ? "rem" : "muted").beginArray();
            for (const model::Store::Mark &m : k ? s.reminders() : s.mutedThreads()) {
                if (m.conv >= s.conversationCount())
                    continue;
                w.beginArray().value(s.conversation(m.conv).id).value(int64_t(m.ts));
                if (k)
                    w.value(m.value);
                w.endArray();
            }
            w.endArray();
        }
        w.key("ai").beginObject(); // "Transcribe with AI" results
        for (const auto &[fileId, t] : s.aiTranscripts())
            w.key(fileId).beginArray().value(t.text).value(t.by).endArray();
        w.endObject();
        w.key("drafts").beginArray();
        for (const model::Store::Draft &d : s.drafts()) {
            if (d.conv >= s.conversationCount())
                continue;
            w.beginArray().value(s.conversation(d.conv).id).value(int64_t(d.thread)).value(d.html);
            w.beginArray();
            for (const std::string &f : d.files)
                w.value(f);
            w.endArray().endArray();
        }
        w.endArray();
        w.key("x").beginObject();
        if (saveExtras)
            saveExtras(w);
        w.endObject().endObject();
        write("meta.json", w.take());
    }
    for (ConvRef c = 0; c < msgs.size(); ++c) {
        if (!msgs[c] || c >= s.conversationCount())
            continue;
        // The newest kMaxMessages confirmed ones (a pending copy must not
        // come back as a ghost after a restart).
        const model::Conversation          &cv = s.conversation(c);
        std::vector<const model::Message *> keep;
        bool                                older = cv.hasMoreBefore;
        for (size_t i = cv.messages.size(); i-- > 0;) {
            if (cv.messages[i].pending)
                continue;
            if (keep.size() == size_t(kMaxMessages)) {
                older = true;
                break;
            }
            keep.push_back(&cv.messages[i]);
        }
        json::Writer w;
        IO           io(w, s);
        w.beginObject().key("v").value(kVersion).key("more").value(older).key("m").beginArray();
        for (size_t i = keep.size(); i-- > 0;) {
            io.open(0);
            fields(io, const_cast<model::Message &>(*keep[i]));
            io.close();
        }
        w.endArray().endObject();
        write(messagesFile(c).c_str(), w.take());
    }
    if (!_slow && _slowTimer) {
        _app.cancelTimer(_slowTimer);
        _slowTimer = 0;
    }
}

void WorkspaceCache::close(bool keep) {
    if (!_observer)
        return;
    // The presence dots the next start shows until its first poll.
    if (_store.namedRevision() != _usersNamedRev || _store.presenceRevision() != _usersPresenceRev)
        _dirty |= kUsers;
    if (keep)
        flush();
    for (uint64_t *t : {&_timer, &_slowTimer})
        if (*t) {
            _app.cancelTimer(*t);
            *t = 0;
        }
    _store.unobserve(_observer);
    _observer = 0;
    _dirty = _slow = 0;
    _msgDirty.clear();
    // What is still queued is written now, here: nothing of this cache is
    // written after close() (a sign-out removes the directory next).
    _disk->drain();
}

} // namespace cache
