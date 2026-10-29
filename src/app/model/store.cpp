#include "app/model/store.h"

#include "app/mrkdwn/emoji.h"
#include "base/crypto.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <tuple>
#include <charconv>
#include <cstdio>

namespace model {

// ── Ts ──────────────────────────────────────────────────────────────────────

Ts parseTs(std::string_view s) {
    // "seconds.micros"; the fraction may be shorter than 6 digits.
    const size_t dot  = s.find('.');
    int64_t      secs = 0;
    const auto   head = s.substr(0, dot);
    if (head.empty() || std::from_chars(head.data(), head.data() + head.size(), secs).ptr !=
                            head.data() + head.size())
        return 0;
    int64_t micros = 0;
    if (dot != std::string_view::npos) {
        const auto frac = s.substr(dot + 1);
        if (frac.size() > 6)
            return 0;
        for (char c : frac) {
            if (c < '0' || c > '9')
                return 0;
            micros = micros * 10 + (c - '0');
        }
        for (size_t k = frac.size(); k < 6; ++k)
            micros *= 10;
    }
    return secs * 1000000 + micros;
}

std::string formatTs(Ts ts) {
    char buf[32];
    std::snprintf(
        buf, sizeof buf, "%lld.%06lld", (long long)(ts / 1000000), (long long)(ts % 1000000)
    );
    return buf;
}

// ── Records ─────────────────────────────────────────────────────────────────

std::string_view User::label() const {
    if (!displayName.empty())
        return displayName;
    return name.empty() ? std::string_view(id) : std::string_view(name);
}

std::string_view User::mentionLabel() const {
    return profileName.empty() ? label() : std::string_view(profileName);
}

void User::resolveName(bool realNames) {
    if (realName.empty() && profileName.empty())
        return;
    const std::string &first = realNames ? realName : profileName;
    const std::string &other = realNames ? profileName : realName;
    displayName              = !first.empty() ? first : !other.empty() ? other : name;
}

MessageExtras::MessageExtras()                      = default;
MessageExtras::MessageExtras(const MessageExtras &) = default;
MessageExtras::~MessageExtras()                     = default;
Message::Message()                                  = default;
Message::Message(Message &&) noexcept               = default;
Message &Message::operator=(Message &&) noexcept    = default;
Message::~Message()                                 = default;

bool File::isHtml() const {
    // An HTML file by its type, else by its extension.
    if (str::iequals(mime, "text/html"))
        return true;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string_view ext = std::string_view(name).substr(dot + 1);
    return str::iequals(ext, "html") || str::iequals(ext, "htm");
}

MessageExtras &Message::extras() {
    if (!extra)
        extra = std::make_unique<MessageExtras>();
    return *extra;
}

namespace {
const MessageExtras       kNoExtras;
const User                kNoUserRecord;
const std::vector<Typing> kNoTyping;
} // namespace

const std::string &Message::subtype() const {
    return (extra ? *extra : kNoExtras).subtype;
}
const std::vector<File> &Message::files() const {
    return (extra ? *extra : kNoExtras).files;
}
const std::vector<Attachment> &Message::attachments() const {
    return (extra ? *extra : kNoExtras).attachments;
}
bool Message::isHuddle() const {
    return extra && extra->subtype == "huddle_thread";
}

Message Message::clone() const {
    Message m;
    m.ts          = ts;
    m.threadTs    = threadTs;
    m.latestReply = latestReply;
    m.user        = user;
    m.replyCount  = replyCount;
    m.text        = text;
    m.edited      = edited;
    m.pinned      = pinned;
    m.pending     = pending;
    m.replyUsers  = replyUsers;
    m.reactions   = reactions;
    if (extra)
        m.extra = std::make_unique<MessageExtras>(*extra);
    return m;
}

bool Reaction::operator==(const Reaction &) const               = default;
bool File::operator==(const File &) const                       = default;
bool AttachmentField::operator==(const AttachmentField &) const = default;
bool Block::operator==(const Block &) const                     = default;
bool Attachment::operator==(const Attachment &) const           = default;
bool Button::operator==(const Button &) const                   = default;
bool Huddle::operator==(const Huddle &) const                   = default;
bool MessageExtras::operator==(const MessageExtras &) const     = default;

bool Message::operator==(const Message &o) const {
    if (ts != o.ts || threadTs != o.threadTs || latestReply != o.latestReply || user != o.user ||
        pinnedBy != o.pinnedBy || replyCount != o.replyCount || edited != o.edited ||
        pinned != o.pinned || saved != o.saved || pending != o.pending || text != o.text ||
        replyUsers != o.replyUsers || reactions != o.reactions)
        return false;
    // No extras and empty extras read the same (files() etc. are empty).
    if (!extra || !o.extra)
        return (extra ? *extra : kNoExtras) == (o.extra ? *o.extra : kNoExtras);
    return *extra == *o.extra;
}

// ── Store ───────────────────────────────────────────────────────────────────

Store::Store()  = default;
Store::~Store() = default;

void Store::clear() {
    _users.clear();
    _userIndex.clear();
    _convs.clear();
    _convIndex.clear();
    _typing.clear();
    _customEmoji.clear();
    customEmojiChanged();
    myGroups.clear();
    workspaceId.clear();
    workspaceName.clear();
    workspaceIcon.clear();
    workspaceUrl.clear();
    _mutedThreads.clear();
    _aiTranscripts.clear();
    _reminders.clear();
    _saved.clear();
    _scheduled.clear();
    _sections.clear();
    ++_sectionsRev;
    _usergroups.clear();
    _channelNames.clear();
    _linkedAuthors.clear();
    ++_localRev;
    ++_groupRev;
    _unreadThreads     = 0;
    _unreadActivity    = 0;
    workspaceMuted     = false;
    answersAreMentions = false;
    me                 = kNoUser;
    ++_textRev;
    textUnlisted();
    _allUsersDirty = true;
    emit({ChangeKind::Roster});
    emit({ChangeKind::Users});
}

UserRef Store::addUser(User u) {
    u.resolveName(_realNames);
    if (const auto it = _userIndex.find(u.id); it != _userIndex.end()) {
        _users[it->second]             = std::move(u); // same ref: messages keep pointing at it
        _users[it->second].placeholder = false;
        _touchedUsers.push_back(it->second);
        return it->second;
    }
    const UserRef ref = UserRef(_users.size());
    _userIndex.emplace(u.id, ref);
    _users.push_back(std::move(u));
    _touchedUsers.push_back(ref);
    return ref;
}

void Store::setRealNames(bool on) {
    if (on == _realNames)
        return;
    _realNames = on;
    for (User &u : _users)
        u.resolveName(on);
    usersChanged();
}

void Store::usersChanged() {
    _allUsersDirty = true;
    emit({ChangeKind::Users});
}

void Store::usersChanged(UserRef u) {
    if (u < _users.size())
        _touchedUsers.push_back(u);
    emit({ChangeKind::Users});
}

namespace {

uint64_t mixHash(uint64_t h, std::string_view v) {
    // A separator after each field: "ab","c" ≠ "a","bc".
    return crypto::fnv1a(std::string_view("\x1f", 1), crypto::fnv1a(v, h));
}

} // namespace

// Hashing the users touched since the last Users emit (all of them after an
// in-place change that didn't say whose): far cheaper than what an observer
// would redo without it (re-binding every message row, re-serialising
// users.json).
void Store::noteUserRevisions() {
    const size_t n       = _users.size();
    bool         profile = n < _profileHash.size(), named = profile, presence = false;
    // Many touched (a users.list load): one pass over all is as cheap.
    const bool   all = _allUsersDirty || profile || _touchedUsers.size() > n / 2;
    _profileHash.resize(n, 0);
    _presenceHash.resize(n, 0);
    _userRev.resize(n, 0);
    const uint64_t next = _profileRev + 1;
    const size_t   todo = all ? n : _touchedUsers.size();
    for (size_t k = 0; k < todo; ++k) {
        const size_t i = all ? k : _touchedUsers[k];
        if (i >= n)
            continue;
        const User &u = _users[i];
        uint64_t    h = crypto::kFnvOffset;
        for (const std::string *f :
             {&u.id,
              &u.name,
              &u.displayName,
              &u.realName,
              &u.profileName,
              &u.title,
              &u.email,
              &u.avatar,
              &u.statusEmoji,
              &u.statusText})
            h = mixHash(h, *f);
        const char flags[] = {
            char(u.hasTz),
            char(u.bot),
            char(u.admin),
            char(u.owner),
            char(u.placeholder),
            char(u.unavailable),
            char(u.deleted),
            char(u.stranger)
        };
        h = mixHash(h, std::string_view(flags, sizeof flags));
        h = mixHash(h, std::string_view(reinterpret_cast<const char *>(&u.tzOffset), 4));
        h |= 1; // never 0: a new slot always differs
        if (h != _profileHash[i]) {
            _profileHash[i] = h;
            _userRev[i]     = next;
            profile         = true;
            named           = named || !u.placeholder;
        }
        const uint64_t p = 2 | uint64_t(u.active) | uint64_t(u.dnd) << 2;
        if (p != _presenceHash[i]) {
            _presenceHash[i] = p;
            presence         = true;
        }
    }
    _touchedUsers.clear();
    _allUsersDirty = false;
    if (profile)
        _profileRev = next;
    if (named)
        ++_namedRev;
    if (presence)
        ++_presenceRev;
}

UserRef Store::internUser(std::string_view id) {
    if (id.empty())
        return kNoUser;
    if (const UserRef r = findUser(id); r != kNoUser)
        return r;
    User u;
    u.id          = std::string(id);
    u.placeholder = true; // kept: only the merge path of addUser clears it
    return addUser(std::move(u));
}

UserRef Store::findUser(std::string_view id) const {
    // unordered_map<std::string> has no heterogeneous lookup before C++20's
    // is_transparent support for unordered containers; one short copy is fine.
    const auto it = _userIndex.find(std::string(id));
    return it == _userIndex.end() ? kNoUser : it->second;
}

const User &Store::user(UserRef u) const {
    return u < _users.size() ? _users[u] : kNoUserRecord;
}

User &Store::user(UserRef u) {
    static User scratch; // writes to kNoUser land here and are harmless
    return u < _users.size() ? _users[u] : scratch;
}

namespace {

// Every Conversation field but the loaded messages and threads. A field added
// to Conversation belongs here too, or changing only it emits no Meta.
bool sameMeta(const Conversation &a, const Conversation &b) {
    const auto meta = [](const Conversation &c) {
        return std::tie(
            c.id,
            c.name,
            c.topic,
            c.kind,
            c.dmUser,
            c.members,
            c.memberCount,
            c.unread,
            c.mentions,
            c.lastRead,
            c.latest,
            c.starred,
            c.muted,
            c.member,
            c.notify,
            c.canvasTitle,
            c.canvasId,
            c.localName,
            c.readOnly,
            c.huddleActive,
            c.huddleLink,
            c.huddleParticipants,
            c.hasMoreBefore
        );
    };
    return meta(a) == meta(b);
}

} // namespace

ConvRef Store::addConversation(Conversation c) {
    bool          added = false;
    const ConvRef ref   = mergeConversation(std::move(c), &added);
    if (added)
        emit({ChangeKind::Roster});
    return ref;
}

void Store::addConversations(std::vector<Conversation> convs) {
    bool added = false;
    for (Conversation &c : convs) {
        bool one = false;
        mergeConversation(std::move(c), &one);
        added = added || one;
    }
    if (added)
        emit({ChangeKind::Roster});
}

// addConversation without the Roster emit for a new one (*added says).
ConvRef Store::mergeConversation(Conversation c, bool *added) {
    *added = false;
    if (const auto it = _convIndex.find(c.id); it != _convIndex.end()) {
        Conversation &old      = _convs[it->second];
        // Keep the loaded messages: a roster refresh only brings metadata.
        const bool    metaOnly = c.messages.empty() && c.threads.empty();
        if (metaOnly) {
            c.messages      = std::move(old.messages);
            c.threads       = std::move(old.threads);
            c.hasMoreBefore = old.hasMoreBefore;
        }
        // A roster reload re-adds every conversation: only a real change is
        // news (N no-op Meta emits cost the sidebar and the shell N passes).
        const bool changed = !metaOnly || !sameMeta(old, c);
        old                = std::move(c);
        if (changed)
            emit({ChangeKind::Meta, it->second});
        return it->second;
    }
    const ConvRef ref = ConvRef(_convs.size());
    _convIndex.emplace(c.id, ref);
    _convs.push_back(std::move(c));
    _typing.emplace_back();
    *added = true;
    return ref;
}

ConvRef Store::findConversation(std::string_view id) const {
    const auto it = _convIndex.find(std::string(id));
    return it == _convIndex.end() ? kNoConv : it->second;
}

// A ref this Store doesn't have (kNoConv, or one a view kept from the Store
// the slot just left) reads as an empty conversation, not past the end: the
// exit crash was a message list rebuilding on the blank Store.
const Conversation &Store::conversation(ConvRef c) const {
    static const Conversation none;
    return c < _convs.size() ? _convs[c] : none;
}

Conversation &Store::conversation(ConvRef c) {
    static Conversation scratch; // writes to a missing ref land here and are harmless
    if (c < _convs.size())
        return _convs[c];
    scratch = {};
    return scratch;
}

std::string Store::displayName(ConvRef ref) const {
    if (ref >= _convs.size())
        return {};
    const Conversation &c = _convs[ref];
    switch (c.kind) {
    case ConvKind::Dm:
        if (!c.localName.empty()) // a renamed agent session
            return c.localName;
        if (c.dmUser != kNoUser)
            return std::string(user(c.dmUser).label());
        return c.name;
    case ConvKind::Group: {
        if (!c.localName.empty())
            return c.localName;
        // "Mira Okafor, Jonas Weber": the participants other than me.
        std::string out;
        for (UserRef u : c.members) {
            if (u == me)
                continue;
            if (!out.empty())
                out += ", ";
            out += user(u).label();
        }
        return out.empty() ? c.name : out;
    }
    default:
        return c.name;
    }
}

void Store::updateConversation(ConvRef c, const std::function<void(Conversation &)> &fn) {
    if (c >= _convs.size())
        return;
    fn(_convs[c]);
    emit({ChangeKind::Meta, c});
}

Thread *Store::findThread(Conversation &c, Ts root) {
    for (auto &t : c.threads)
        if (t.root == root)
            return &t;
    return nullptr;
}

const Thread *Store::findThread(const Conversation &c, Ts root) const {
    for (const auto &t : c.threads)
        if (t.root == root)
            return &t;
    return nullptr;
}

std::vector<Message> *Store::listFor(Conversation &c, Ts thread, bool create) {
    if (!thread)
        return &c.messages;
    if (Thread *t = findThread(c, thread))
        return &t->replies;
    if (!create)
        return nullptr;
    c.threads.push_back(Thread{thread, {}});
    return &c.threads.back().replies;
}

// Pages come oldest-first (fixture, conversations.replies) or newest-first
// (conversations.history): reverse the latter, insertion-sort anything else.
// Pages are ≤ 200 messages, and this avoids std::sort's ~1.5 KB introsort
// instantiation for a move-only type.
void sortByTs(std::vector<Message> &v) {
    if (v.size() > 1 && v.front().ts > v.back().ts)
        std::reverse(v.begin(), v.end());
    for (size_t i = 1; i < v.size(); ++i)
        for (size_t j = i; j > 0 && v[j].ts < v[j - 1].ts; --j)
            std::swap(v[j], v[j - 1]);
}

static std::vector<Message>::iterator lowerByTs(std::vector<Message> &v, Ts ts) {
    return std::lower_bound(v.begin(), v.end(), ts, [](const Message &m, Ts t) {
        return m.ts < t;
    });
}

Message *Store::findMessage(ConvRef ref, Ts ts) {
    if (ref >= _convs.size())
        return nullptr;
    Conversation &c = _convs[ref];
    if (auto it = lowerByTs(c.messages, ts); it != c.messages.end() && it->ts == ts)
        return &*it;
    for (auto &t : c.threads)
        if (auto it = lowerByTs(t.replies, ts); it != t.replies.end() && it->ts == ts)
            return &*it;
    return nullptr;
}

const Message *Store::findMessage(ConvRef ref, Ts ts) const {
    return const_cast<Store *>(this)->findMessage(ref, ts);
}

const std::vector<Message> *Store::replies(ConvRef ref, Ts root) const {
    if (ref >= _convs.size())
        return nullptr;
    const Thread *t = findThread(_convs[ref], root);
    return t ? &t->replies : nullptr;
}

size_t Store::indexOf(ConvRef ref, Ts ts, Ts thread) const {
    if (ref >= _convs.size())
        return SIZE_MAX;
    Store *self = const_cast<Store *>(this); // listFor(create = false) never mutates
    auto  *list = self->listFor(self->_convs[ref], thread, false);
    if (!list)
        return SIZE_MAX;
    const auto it = lowerByTs(*list, ts);
    return (it != list->end() && it->ts == ts) ? size_t(it - list->begin()) : SIZE_MAX;
}

void Store::addPage(ConvRef ref, std::vector<Message> page) {
    if (ref >= _convs.size() || page.empty())
        return;
    Conversation &c      = _convs[ref];
    const Ts      thread = page.front().isReply() ? page.front().threadTs : 0;
    auto         &list   = *listFor(c, thread, true);
    sortByTs(page);

    std::vector<Ts>      updated, inserted;
    std::vector<Message> before, after;
    const bool           empty = list.empty();
    const Ts             front = empty ? 0 : list.front().ts;
    const Ts             back  = empty ? 0 : list.back().ts;
    for (auto &m : page) {
        if (!empty) {
            if (auto it = lowerByTs(list, m.ts); it != list.end() && it->ts == m.ts) {
                // A refetched page is mostly what we hold: only a real change
                // is an Update (each one re-lays the list out and is saved).
                if (*it == m)
                    continue;
                *it = std::move(m);
                updated.push_back(it->ts);
                continue;
            }
        }
        if (empty || m.ts > back)
            after.push_back(std::move(m));
        else if (m.ts < front)
            before.push_back(std::move(m));
        else {
            inserted.push_back(m.ts);
            list.insert(lowerByTs(list, m.ts), std::move(m));
        }
    }
    // Dedupe within the page itself (a sorted page may repeat a ts).
    auto dedupe = [](std::vector<Message> &v) {
        v.erase(
            std::unique(
                v.begin(), v.end(), [](const Message &a, const Message &b) { return a.ts == b.ts; }
            ),
            v.end()
        );
    };
    dedupe(before);
    dedupe(after);
    const uint32_t nBefore = uint32_t(before.size()), nAfter = uint32_t(after.size());
    if (nBefore)
        list.insert(
            list.begin(),
            std::make_move_iterator(before.begin()),
            std::make_move_iterator(before.end())
        );
    if (nAfter)
        list.insert(
            list.end(), std::make_move_iterator(after.begin()), std::make_move_iterator(after.end())
        );
    if (!thread && !list.empty())
        c.latest = std::max(c.latest, list.back().ts);

    for (Ts ts : updated)
        emit({ChangeKind::Update, ref, thread, ts});
    if (nBefore)
        emit({ChangeKind::Prepend, ref, thread, 0, nBefore});
    for (Ts ts : inserted)
        emit({ChangeKind::Insert, ref, thread, ts});
    if (nAfter)
        emit({ChangeKind::Append, ref, thread, 0, nAfter});
}

void Store::addMessage(ConvRef ref, Message m) {
    if (ref >= _convs.size())
        return;
    Conversation &c      = _convs[ref];
    const Ts      thread = m.isReply() ? m.threadTs : 0;
    const Ts      ts     = m.ts;
    const UserRef author = m.user;

    // The author's typing indicator ends with their message, as in Slack —
    // except an agent's mid-task update (subtype "progress"): it is still
    // working.
    auto        &typers       = _typing[ref];
    const size_t typersBefore = typers.size();
    if (m.subtype() != "progress")
        std::erase_if(typers, [&](const Typing &t) {
            return t.user == author && t.thread == thread;
        });
    const bool typingChanged = typers.size() != typersBefore;

    if (Message *existing = findMessage(ref, ts)) {
        // A push for something we have (our own send echoed back): replace.
        *existing = std::move(m);
        emit({ChangeKind::Update, ref, thread, ts});
        if (typingChanged)
            emit({ChangeKind::Typing, ref, thread});
        return;
    }

    if (thread) {
        Message   *root  = findMessage(ref, thread);
        auto      &list  = *listFor(c, thread, true);
        const bool atEnd = list.empty() || ts > list.back().ts;
        list.insert(lowerByTs(list, ts), std::move(m));
        if (root) {
            ++root->replyCount;
            root->latestReply = std::max(root->latestReply, ts);
            if (author != kNoUser && root->replyUsers.size() < 5 &&
                std::find(root->replyUsers.begin(), root->replyUsers.end(), author) ==
                    root->replyUsers.end())
                root->replyUsers.push_back(author);
        }
        emit(
            {atEnd ? ChangeKind::Append : ChangeKind::Insert,
             ref,
             thread,
             atEnd ? 0 : ts,
             atEnd ? 1u : 0u}
        );
        if (root)
            emit({ChangeKind::Update, ref, 0, thread});
    } else {
        const bool atEnd   = c.messages.empty() || ts > c.messages.back().ts;
        const bool unread  = author != me && ts > c.lastRead;
        const bool mention = unread && mentions(m);
        c.messages.insert(lowerByTs(c.messages, ts), std::move(m));
        c.latest = std::max(c.latest, ts);
        if (c.isDirect())
            c.member = true; // a closed DM comes back with its next message
        if (unread) {
            ++c.unread;
            if (mention)
                ++c.mentions;
        }
        emit(
            {atEnd ? ChangeKind::Append : ChangeKind::Insert,
             ref,
             0,
             atEnd ? 0 : ts,
             atEnd ? 1u : 0u}
        );
        emit({ChangeKind::Meta, ref});
    }
    if (typingChanged)
        emit({ChangeKind::Typing, ref, thread});
}

bool Store::updateMessage(ConvRef ref, Ts ts, const std::function<void(Message &)> &fn) {
    Message *m = findMessage(ref, ts);
    if (!m)
        return false;
    fn(*m);
    emit({ChangeKind::Update, ref, m->isReply() ? m->threadTs : 0, ts});
    return true;
}

bool Store::removeMessage(ConvRef ref, Ts ts) {
    Message *m = findMessage(ref, ts);
    if (!m)
        return false;
    Conversation &c      = _convs[ref];
    const Ts      thread = m->isReply() ? m->threadTs : 0;
    auto         &list   = *listFor(c, thread, false);
    list.erase(lowerByTs(list, ts));
    if (thread) {
        if (Message *root = findMessage(ref, thread)) {
            if (root->replyCount)
                --root->replyCount;
            root->latestReply = list.empty() ? 0 : list.back().ts;
            emit({ChangeKind::Remove, ref, thread, ts});
            emit({ChangeKind::Update, ref, 0, thread});
            return true;
        }
    } else {
        // Its replies go with it, as with Slack's message_deleted.
        std::erase_if(c.threads, [&](const Thread &t) { return t.root == ts; });
        recountUnread(c);
    }
    emit({ChangeKind::Remove, ref, thread, ts});
    if (!thread)
        emit({ChangeKind::Meta, ref});
    return true;
}

bool Store::setReaction(ConvRef ref, Ts ts, std::string_view name, UserRef u, bool on) {
    return updateMessage(ref, ts, [&](Message &m) {
        auto it = std::find_if(m.reactions.begin(), m.reactions.end(), [&](const Reaction &r) {
            return r.name == name;
        });
        if (on) {
            if (it == m.reactions.end()) {
                m.reactions.push_back(Reaction{std::string(name), 1, {u}});
            } else if (std::find(it->users.begin(), it->users.end(), u) == it->users.end()) {
                it->users.push_back(u);
                ++it->count;
            }
        } else if (it != m.reactions.end()) {
            const auto before = it->users.size();
            std::erase(it->users, u);
            if (it->users.size() != before && it->count)
                --it->count;
            if (it->count == 0)
                m.reactions.erase(it);
        }
    });
}

bool Store::reactedByMe(const Reaction &r) const {
    return me != kNoUser && std::find(r.users.begin(), r.users.end(), me) != r.users.end();
}

bool Store::mentions(const Message &m) const {
    if (answersAreMentions)
        return m.subtype() != "progress";
    return mentionsMe(m.text);
}

void Store::recountUnread(Conversation &c) {
    c.unread   = 0;
    c.mentions = 0;
    auto it    = std::upper_bound(
        c.messages.begin(), c.messages.end(), c.lastRead, [](Ts t, const Message &m) {
            return t < m.ts;
        }
    );
    for (; it != c.messages.end(); ++it) {
        if (it->user == me)
            continue;
        ++c.unread;
        if (mentions(*it))
            ++c.mentions;
    }
}

void Store::markRead(ConvRef ref, Ts ts) {
    if (ref >= _convs.size())
        return;
    Conversation &c = _convs[ref];
    if (ts > c.lastRead)
        c.lastRead = ts;
    recountUnread(c);
    emit({ChangeKind::Meta, ref});
}

void Store::markUnread(ConvRef ref, Ts ts) {
    if (ref >= _convs.size() || ts <= 0)
        return;
    Conversation &c = _convs[ref];
    c.lastRead      = ts - 1;
    recountUnread(c);
    emit({ChangeKind::Meta, ref});
}

namespace {
std::vector<Store::Mark>::iterator findMark(std::vector<Store::Mark> &v, ConvRef c, Ts ts) {
    return std::find_if(v.begin(), v.end(), [&](const Store::Mark &m) {
        return m.conv == c && m.ts == ts;
    });
}

std::vector<Store::Draft>::iterator findDraft(std::vector<Store::Draft> &v, ConvRef c, Ts thread) {
    return std::find_if(v.begin(), v.end(), [&](const Store::Draft &d) {
        return d.conv == c && d.thread == thread;
    });
}
} // namespace

bool Store::threadMuted(ConvRef c, Ts root) const {
    auto &v = const_cast<std::vector<Mark> &>(_mutedThreads);
    return findMark(v, c, root) != v.end();
}

void Store::setThreadMuted(ConvRef c, Ts root, bool on) {
    auto it = findMark(_mutedThreads, c, root);
    if (on && it == _mutedThreads.end())
        _mutedThreads.push_back({c, root, 1});
    else if (!on && it != _mutedThreads.end())
        _mutedThreads.erase(it);
    else
        return;
    ++_localRev;
    emit({ChangeKind::Meta, c}); // app-local state: the workspace cache keeps it
}

const Store::Draft *Store::draft(ConvRef c, Ts thread) const {
    auto &v  = const_cast<std::vector<Draft> &>(_drafts);
    auto  it = findDraft(v, c, thread);
    return it == v.end() ? nullptr : &*it;
}

void Store::setDraft(ConvRef c, Ts thread, std::string html, std::vector<std::string> files) {
    const bool none = html.empty() && files.empty();
    auto       it   = findDraft(_drafts, c, thread);
    if (it == _drafts.end()) {
        if (none)
            return;
        _drafts.push_back({c, thread, std::move(html), std::move(files)});
    } else if (none) {
        _drafts.erase(it);
    } else {
        if (it->html == html && it->files == files)
            return;
        it->html  = std::move(html);
        it->files = std::move(files);
    }
    ++_localRev;
    emit({ChangeKind::Meta, c}); // app-local state: the workspace cache keeps it
}

const Store::AiTranscript *Store::aiTranscript(const std::string &fileId) const {
    const auto it = _aiTranscripts.find(fileId);
    return it == _aiTranscripts.end() ? nullptr : &it->second;
}

void Store::setAiTranscript(const std::string &fileId, std::string text, std::string by) {
    if (fileId.empty())
        return;
    _aiTranscripts[fileId] = {std::move(text), std::move(by)};
    ++_localRev;
    const auto has = [&](const Message &m) {
        for (const File &f : m.files())
            if (f.id == fileId)
                return true;
        return false;
    };
    for (ConvRef c = 0; c < ConvRef(_convs.size()); ++c) {
        for (const Message &m : _convs[c].messages)
            if (has(m))
                emit({ChangeKind::Update, c, 0, m.ts});
        for (const Thread &t : _convs[c].threads)
            for (const Message &m : t.replies)
                if (has(m))
                    emit({ChangeKind::Update, c, t.root, m.ts});
    }
}

int64_t Store::reminderAt(ConvRef c, Ts ts) const {
    auto &v  = const_cast<std::vector<Mark> &>(_reminders);
    auto  it = findMark(v, c, ts);
    return it == v.end() ? 0 : it->value;
}

void Store::setReminderAt(ConvRef c, Ts ts, int64_t due) {
    auto it = findMark(_reminders, c, ts);
    if (it != _reminders.end() ? it->value != due : due > 0)
        ++_localRev; // the workspace cache saves it
    if (it != _reminders.end())
        _reminders.erase(it);
    if (due > 0)
        _reminders.push_back({c, ts, due});
    emit({ChangeKind::Update, c, 0, ts});
}

namespace {
// What a Saved messages card shows of the message.
void takePreview(Store::SavedItem &s, const Message &m) {
    s.thread    = m.isReply() ? m.threadTs : 0;
    s.text      = m.text;
    s.author    = m.user;
    s.botName   = m.extra ? m.extra->botName : std::string();
    s.botAvatar = m.extra ? m.extra->botAvatar : std::string();
    s.previewed = true;
}
} // namespace

// Reminders (soonest first) ahead of plain bookmarks (newest first). After
// one item moved the list is nearly sorted: an insertion pass.
void Store::sortSaved() {
    const auto before = [](const SavedItem &a, const SavedItem &b) {
        if ((a.due > 0) != (b.due > 0))
            return a.due > 0;
        if (a.due > 0 && a.due != b.due)
            return a.due < b.due;
        if (a.savedAt != b.savedAt)
            return a.savedAt > b.savedAt;
        return a.ts > b.ts;
    };
    for (size_t i = 1; i < _saved.size(); ++i)
        for (size_t j = i; j > 0 && before(_saved[j], _saved[j - 1]); --j)
            std::swap(_saved[j], _saved[j - 1]);
}

const Store::SavedItem *Store::findSaved(ConvRef c, Ts ts) const {
    for (const SavedItem &s : _saved)
        if (s.conv == c && s.ts == ts)
            return &s;
    return nullptr;
}

void Store::setSavedItem(ConvRef c, Ts ts, bool on, int64_t due, int64_t savedAt) {
    auto it = std::find_if(_saved.begin(), _saved.end(), [&](const SavedItem &s) {
        return s.conv == c && s.ts == ts;
    });
    if (!on) {
        if (it == _saved.end())
            return;
        _saved.erase(it);
    } else {
        if (it == _saved.end()) {
            SavedItem s;
            s.conv = c;
            s.ts   = ts;
            it     = _saved.insert(_saved.end(), std::move(s));
        }
        if (it->due != due)
            it->fired = false; // rescheduled: it goes off again
        it->due = due;
        if (savedAt > 0)
            it->savedAt = savedAt;
        else if (it->savedAt <= 0)
            it->savedAt = base::nowSecs();
        if (!it->previewed)
            if (const Message *m = findMessage(c, ts))
                takePreview(*it, *m);
        sortSaved();
    }
    emit({ChangeKind::Update, c, 0, ts});
}

void Store::setSavedPreview(ConvRef c, Ts ts, const Message *m) {
    for (SavedItem &s : _saved)
        if (s.conv == c && s.ts == ts) {
            if (m)
                takePreview(s, *m);
            else
                s.previewed = true; // gone: "No preview available"
            emit({ChangeKind::Update, c, 0, ts});
            return;
        }
}

void Store::setReminderFired(ConvRef c, Ts ts, bool fired) {
    for (SavedItem &s : _saved)
        if (s.conv == c && s.ts == ts && s.fired != fired) {
            s.fired = fired;
            emit({ChangeKind::Update, c, 0, ts});
            return;
        }
}

void Store::setScheduled(std::vector<ScheduledItem> items) {
    std::stable_sort(
        items.begin(), items.end(), [](const ScheduledItem &a, const ScheduledItem &b) {
            return a.at < b.at;
        }
    );
    const auto same = [](const ScheduledItem &a, const ScheduledItem &b) {
        return a.id == b.id && a.version == b.version && a.conv == b.conv && a.thread == b.thread &&
               a.threadKnown == b.threadKnown && a.at == b.at && a.text == b.text;
    };
    if (std::equal(items.begin(), items.end(), _scheduled.begin(), _scheduled.end(), same))
        return;
    _scheduled = std::move(items);
    emit({ChangeKind::Meta});
}

void Store::removeScheduled(const std::string &id) {
    const auto it = std::find_if(_scheduled.begin(), _scheduled.end(), [&](const ScheduledItem &s) {
        return s.id == id;
    });
    if (it == _scheduled.end())
        return;
    _scheduled.erase(it);
    emit({ChangeKind::Meta});
}

const Store::Section *Store::sectionOf(std::string_view convId) const {
    for (const Section &s : _sections)
        if (std::find(s.convs.begin(), s.convs.end(), convId) != s.convs.end())
            return &s;
    return nullptr;
}

const Store::Section *Store::findSection(std::string_view id) const {
    for (const Section &s : _sections)
        if (s.id == id)
            return &s;
    return nullptr;
}

void Store::setSections(std::vector<Section> sections) {
    if (sections == _sections)
        return;
    _sections = std::move(sections);
    ++_sectionsRev;
    emit({ChangeKind::Meta});
}

const Store::Usergroup *Store::findUsergroup(std::string_view id) const {
    for (const Usergroup &g : _usergroups)
        if (g.id == id)
            return &g;
    return nullptr;
}

void Store::setUsergroups(std::vector<Usergroup> groups) {
    std::vector<std::string> mine;
    const std::string       &meId = user(me).id;
    for (Usergroup &g : groups) {
        if (!g.mine && !meId.empty())
            g.mine = std::find(g.users.begin(), g.users.end(), meId) != g.users.end();
        std::vector<std::string>().swap(g.users);
        if (g.mine)
            mine.push_back(g.id);
    }
    myGroups = std::move(mine);
    ++_textRev;
    ++_groupRev;
    // Which groups changed: added, removed or edited.
    for (const Usergroup &g : groups)
        if (const Usergroup *old = findUsergroup(g.id); !old || !(*old == g))
            logText(TextChange::Kind::Usergroup, g.id);
    for (const Usergroup &g : _usergroups)
        if (std::none_of(groups.begin(), groups.end(), [&](const Usergroup &x) {
                return x.id == g.id;
            }))
            logText(TextChange::Kind::Usergroup, g.id);
    _usergroups = std::move(groups);
    emit({ChangeKind::Users});
}

const std::string *Store::channelName(std::string_view id) const {
    const auto it = _channelNames.find(std::string(id));
    return it == _channelNames.end() ? nullptr : &it->second;
}

UserRef Store::linkedAuthor(std::string_view conv, std::string_view ts) const {
    const auto it = _linkedAuthors.find(str::concat({conv, "/", ts}));
    return it == _linkedAuthors.end() ? kNoUser : it->second;
}

void Store::setLinkedAuthor(std::string_view conv, std::string_view ts, UserRef u) {
    if (!conv.empty() && !ts.empty() && u != kNoUser)
        _linkedAuthors[str::concat({conv, "/", ts})] = u;
}

void Store::setChannelName(std::string id, std::string name) {
    ++_textRev;
    logText(TextChange::Kind::Channel, id);
    _channelNames[std::move(id)] = std::move(name);
    emit({ChangeKind::Users});
}

// At most this many changes are listed; past it, observers re-bind all.
constexpr size_t kTextLogMax = 256;

void Store::logText(TextChange::Kind kind, std::string id) {
    if (_textLogFloor == _textRev)
        return; // this step is unlisted already
    if (_textLog.size() >= kTextLogMax) {
        // Drop the older half; whoever saw less than the dropped can't list.
        const size_t drop = _textLog.size() / 2;
        _textLogFloor     = _textLog[drop - 1].rev;
        _textLog.erase(_textLog.begin(), _textLog.begin() + long(drop));
    }
    _textLog.push_back({_textRev, {kind, std::move(id)}});
}

void Store::textUnlisted() {
    _textLog.clear();
    _textLogFloor = _textRev;
}

bool Store::textChangesSince(uint64_t since, std::vector<TextChange> *out) const {
    if (since < _textLogFloor)
        return false;
    for (const LoggedText &l : _textLog)
        if (l.rev > since)
            out->push_back(l.change);
    return true;
}

void Store::announceReply(ConvRef c, const Message &m) {
    const Message *was = _arrived;
    _arrived           = &m;
    emit({ChangeKind::Arrived, c, m.threadTs, m.ts});
    _arrived = was;
}

void Store::setUnreadThreads(int n) {
    n = std::max(n, 0);
    if (n == _unreadThreads)
        return;
    _unreadThreads = n;
    emit({ChangeKind::Meta, kNoConv});
}

void Store::setUnreadActivity(int n) {
    n = std::max(n, 0);
    if (n == _unreadActivity)
        return;
    _unreadActivity = n;
    emit({ChangeKind::Meta, kNoConv});
}

std::string Store::workspaceLink() const {
    if (!workspaceUrl.empty())
        return workspaceUrl.back() == '/' ? workspaceUrl : workspaceUrl + '/';
    return "https://app.slack.com/client/" + workspaceId + '/';
}

std::string Store::conversationLink(ConvRef c) const {
    const std::string base = workspaceUrl.empty() ? "https://slack.com/" : workspaceLink();
    return base + "archives/" + conversation(c).id;
}

std::string Store::permalink(ConvRef c, Ts ts, Ts thread) const {
    std::string t = formatTs(ts);
    t.erase(std::remove(t.begin(), t.end(), '.'), t.end());
    std::string out = conversationLink(c) + "/p" + t;
    if (thread && thread != ts)
        out += "?thread_ts=" + formatTs(thread) + "&cid=" + conversation(c).id;
    return out;
}

bool Store::mentionsMe(std::string_view text) const {
    // A direct mention, a broadcast keyword,
    // or a user group I belong to — the one predicate every badge uses.
    if (me != kNoUser) {
        const std::string tag = "<@" + user(me).id;
        for (size_t i = text.find(tag); i != std::string_view::npos; i = text.find(tag, i + 1)) {
            const size_t after = i + tag.size();
            if (after < text.size() && (text[after] == '>' || text[after] == '|'))
                return true;
        }
    }
    if (!myGroups.empty()) {
        constexpr std::string_view tag = "<!subteam^";
        for (size_t i = text.find(tag); i != std::string_view::npos; i = text.find(tag, i + 1)) {
            const size_t start = i + tag.size();
            size_t       end   = start;
            while (end < text.size() && text[end] != '>' && text[end] != '|')
                ++end;
            const auto id = text.substr(start, end - start);
            if (std::find(myGroups.begin(), myGroups.end(), id) != myGroups.end())
                return true;
        }
    }
    return text.find("<!here") != std::string_view::npos ||
           text.find("<!channel") != std::string_view::npos ||
           text.find("<!everyone") != std::string_view::npos;
}

void Store::setTyping(ConvRef ref, UserRef u, Ts thread, bool on, int64_t sinceMs) {
    if (ref >= _typing.size() || u == me)
        return;
    auto      &list = _typing[ref];
    const auto it   = std::find_if(list.begin(), list.end(), [&](const Typing &t) {
        return t.user == u && t.thread == thread;
    });
    if (on && it != list.end() && it->sinceMs != sinceMs)
        it->sinceMs = sinceMs; // the same typer, a turn that began at another time
    else if (on == (it != list.end()))
        return; // no change: a refreshed indicator fires nothing
    else if (on)
        list.push_back(Typing{u, thread, sinceMs});
    else
        list.erase(it);
    emit({ChangeKind::Typing, ref, thread});
}

const std::vector<Typing> &Store::typing(ConvRef ref) const {
    return ref < _typing.size() ? _typing[ref] : kNoTyping;
}

void Store::customEmojiChanged() {
    _emojiSorted = false;
    ++_emojiRev;
    ++_textRev;
}

void Store::sortCustomEmoji() const {
    if (_emojiSorted)
        return;
    _emojiSorted = true;
    _emojiImages.clear();
    _emojiNames.clear();
    _emojiNames.reserve(_customEmoji.size());
    for (const auto &[name, value] : _customEmoji) {
        _emojiNames.push_back(name);
        if (value.compare(0, 6, "alias:") != 0)
            _emojiImages.push_back({name, value});
    }
    std::sort(_emojiNames.begin(), _emojiNames.end());
    std::sort(_emojiImages.begin(), _emojiImages.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
}

const std::vector<Store::CustomEmoji> &Store::customEmojiImages() const {
    sortCustomEmoji();
    return _emojiImages;
}

const std::vector<std::string_view> &Store::customEmojiNames() const {
    sortCustomEmoji();
    return _emojiNames;
}

void Store::setCustomEmoji(std::string name, std::string value) {
    auto [it, added] = _customEmoji.try_emplace(std::move(name));
    if (!added && it->second == value)
        return; // a reload of the same set changes nothing
    it->second = std::move(value);
    customEmojiChanged();
    logText(TextChange::Kind::Emoji, it->first);
}

void Store::replaceCustomEmoji(std::unordered_map<std::string, std::string> all) {
    if (all == _customEmoji)
        return;
    // The names added, changed or removed.
    std::vector<std::string> changed;
    for (const auto &[name, value] : all)
        if (const auto it = _customEmoji.find(name);
            it == _customEmoji.end() || it->second != value)
            changed.push_back(name);
    for (const auto &[name, value] : _customEmoji)
        if (!all.count(name))
            changed.push_back(name);
    _customEmoji = std::move(all);
    customEmojiChanged();
    if (changed.size() > kTextLogMax / 2) {
        textUnlisted(); // a first load: everything is new anyway
        return;
    }
    for (std::string &name : changed)
        logText(TextChange::Kind::Emoji, std::move(name));
}

Store::EmojiGlyph Store::emojiFor(std::string_view name) const {
    EmojiGlyph g;
    // "+1::skin-tone-3": modifiers only apply to a Unicode base; a custom
    // image base ignores them.
    if (const size_t sep = name.find("::"); sep != std::string_view::npos && sep > 0) {
        g.unicode = emoji::toUnicode(name);
        if (!g.unicode.empty())
            return g;
        return emojiFor(name.substr(0, sep));
    }
    g.unicode = emoji::toUnicode(name);
    if (!g.unicode.empty())
        return g;
    // A raw glyph given as a name (Teams sends reactions as the emoji itself):
    // shortcodes are ASCII, so any non-ASCII byte means it is the glyph.
    for (char c : name)
        if (uint8_t(c) > 0x7F) {
            g.unicode = std::string(name);
            return g;
        }
    std::string cur(name);
    for (int hops = 0; hops < 8; ++hops) { // bounded alias-chain walk
        const auto it = _customEmoji.find(cur);
        if (it == _customEmoji.end())
            break;
        if (it->second.compare(0, 6, "alias:") == 0) {
            cur       = it->second.substr(6);
            g.unicode = emoji::toUnicode(cur);
            if (!g.unicode.empty())
                return g;
            continue;
        }
        g.image = it->second;
        return g;
    }
    return g;
}

Store::ObserverId Store::observe(ConvRef conv, Observer fn) {
    const ObserverId id = _nextObserver++;
    // During dispatch the slot vector must not reallocate under the running
    // callback; new observers join once the outermost emit() is done.
    (_dispatching ? _joining : _observers).push_back(Slot{id, conv, std::move(fn)});
    return id;
}

void Store::unobserve(ObserverId id) {
    for (auto &s : _joining)
        if (s.id == id)
            s.id = 0;
    for (auto &s : _observers) {
        if (s.id == id) {
            if (_dispatching) {
                // Can't erase under a running loop (or destroy the running
                // std::function): mark it dead and compact afterwards.
                s.id          = 0;
                _needsCompact = true;
            } else {
                std::erase_if(_observers, [id](const Slot &x) { return x.id == id; });
            }
            return;
        }
    }
}

void Store::emit(const Change &ch) {
    const bool global = ch.kind == ChangeKind::Roster || ch.kind == ChangeKind::Users;
    if (ch.kind == ChangeKind::Users)
        noteUserRevisions();
    if (global || ch.kind == ChangeKind::Meta)
        ++_metaRev;
    ++_dispatching;
    for (auto &s : _observers) {
        if (s.id == 0)
            continue; // unobserved during this dispatch
        if (s.conv != kAnyConv && !global && s.conv != ch.conv)
            continue;
        s.fn(ch);
    }
    if (--_dispatching == 0) {
        if (_needsCompact) {
            std::erase_if(_observers, [](const Slot &x) { return x.id == 0; });
            _needsCompact = false;
        }
        for (auto &s : _joining)
            if (s.id)
                _observers.push_back(std::move(s));
        _joining.clear();
    }
}

} // namespace model
