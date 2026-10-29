#include "app/fake/fake_backend.h"

#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace fake {

using model::ConvRef;
using model::Ts;

FakeBackend::FakeBackend(model::Store &store, plat::App &app) : Backend(store), _app(app) {}

FakeBackend::~FakeBackend() = default; // _timers cancels what is pending

void FakeBackend::setFixture(std::string path, int64_t nowSecs) {
    _path = std::move(path);
    _now  = nowSecs;
}

void FakeBackend::later(int ms, std::function<void()> fn) {
    _timers.after(ms, std::move(fn));
}

// $MSGA_FAKE_SLOW_MS: extra latency for connect and the history / thread
// pages, to look at the loading states (screenshots, by hand).
static int slowMs() {
    const char *s = std::getenv("MSGA_FAKE_SLOW_MS");
    return s ? std::max(0, std::atoi(s)) : 0;
}

void FakeBackend::connect(Done done) {
    later(kReadLatencyMs + slowMs(), [this, done = std::move(done)] {
        std::string err;
        const bool  ok = loadFixture(_path, _store, &_fx, &err, _now ? _now : base::nowSecs());
        _replyUsed.assign(_fx.autoReplies.size(), 0);
        // Every fixture thread starts out read: the recording opens on a
        // quiet workspace.
        _threadRead.clear();
        for (ConvRef c = 0; ok && c < _store.conversationCount(); ++c)
            for (const model::Thread &t : _store.conversation(c).threads)
                if (!t.replies.empty())
                    _threadRead.push_back({c, t.root, t.replies.back().ts});
        if (done)
            done(ok, err);
    });
}

void FakeBackend::loadHistory(ConvRef conv, Ts, Done done) {
    // The whole fixture history is in the Store after connect(): one page.
    later(kReadLatencyMs * 2 + slowMs(), [this, conv, done = std::move(done)] {
        if (conv < _store.conversationCount())
            _store.updateConversation(conv, [](model::Conversation &c) {
                c.hasMoreBefore = false;
            });
        if (done)
            done(
                conv < _store.conversationCount(),
                conv < _store.conversationCount() ? "" : "no such conversation"
            );
    });
}

model::Ts &FakeBackend::threadRead(ConvRef conv, Ts root) {
    for (ThreadRead &r : _threadRead)
        if (r.conv == conv && r.root == root)
            return r.read;
    _threadRead.push_back({conv, root, 0});
    return _threadRead.back().read;
}

void FakeBackend::loadThreadsView(std::string, ThreadsViewDone done) {
    // subscriptions.thread.getView's shape: the root with its channel, the
    // last few replies, my read cursor.
    later(kReadLatencyMs * 2 + slowMs(), [this, done = std::move(done)] {
        ThreadsView page;
        for (ConvRef c = 0; c < _store.conversationCount(); ++c)
            for (const model::Thread &t : _store.conversation(c).threads) {
                const model::Message *root = _store.findMessage(c, t.root);
                if (t.replies.empty() || !root)
                    continue;
                bool mine = root->user == _store.me;
                for (const model::Message &r : t.replies)
                    mine = mine || r.user == _store.me;
                if (!mine)
                    continue;
                FollowedThread f;
                f.conv         = c;
                f.root         = root->clone();
                f.lastRead     = threadRead(c, t.root);
                const size_t n = std::min<size_t>(t.replies.size(), 3);
                for (size_t i = t.replies.size() - n; i < t.replies.size(); ++i)
                    f.latestReplies.push_back(t.replies[i].clone());
                for (const model::Message &r : t.replies)
                    page.totalUnreadReplies += r.ts > f.lastRead;
                page.threads.push_back(std::move(f));
            }
        std::sort(page.threads.begin(), page.threads.end(), [](const auto &a, const auto &b) {
            return a.latestReplies.back().ts > b.latestReplies.back().ts;
        });
        if (done)
            done(true, std::move(page));
    });
}

void FakeBackend::markThreadRead(ConvRef conv, Ts root, Ts ts) {
    Ts &cursor = threadRead(conv, root);
    cursor     = std::max(cursor, ts);
}

void FakeBackend::loadThread(ConvRef conv, Ts root, Done done) {
    later(kReadLatencyMs * 2 + slowMs(), [this, conv, root, done = std::move(done)] {
        const bool ok = _store.findMessage(conv, root) != nullptr;
        if (done)
            done(ok, ok ? "" : "thread_not_found");
    });
}

Ts FakeBackend::nextTs() {
    Ts ts = base::nowMicros();
    if (ts <= _lastTs)
        ts = _lastTs + 1;
    return _lastTs = ts;
}

void FakeBackend::send(ConvRef conv, std::string text, Ts threadTs, Done done) {
    sendWithFiles(conv, std::move(text), threadTs, {}, std::move(done));
}

void FakeBackend::sendWithFiles(
    ConvRef conv, std::string text, Ts threadTs, std::vector<std::string> files, Done done
) {
    if (conv >= _store.conversationCount()) {
        later(0, [done] {
            if (done)
                done(false, "channel_not_found");
        });
        return;
    }
    model::Message m;
    m.ts                     = nextTs();
    m.threadTs               = threadTs;
    m.user                   = _store.me;
    m.pending                = true;
    // A GIF from the picker (<gifDir/x.gif|title>) stays a link and
    // unfurls as the animated image card, a stand-in for GIPHY.
    const std::string gifDir = _fx.gifDir + "/";
    for (size_t at = 0; (at = text.find("<" + gifDir, at)) != std::string::npos;) {
        const size_t end = text.find('>', at);
        if (end == std::string::npos)
            break;
        const std::string tok = text.substr(at + 1, end - at - 1);
        model::Attachment a;
        a.image       = tok.substr(0, tok.find('|'));
        a.title       = std::string(file::baseName(a.image));
        a.link        = a.image;
        a.linkPreview = true;
        std::string head;
        if (file::readRange(a.image, 0, 10, &head) && head.size() >= 10) {
            a.imageWidth  = uint8_t(head[6]) | uint8_t(head[7]) << 8;
            a.imageHeight = uint8_t(head[8]) | uint8_t(head[9]) << 8;
        }
        m.extras().attachments.push_back(std::move(a));
        at = end;
    }
    m.text = std::move(text);
    for (const std::string &path : files) {
        // Ids continue after the fixture's (FDEMO0000…) so they stay unique.
        if (file::exists(path) && !file::isDir(path))
            m.extras().files.push_back(fileFromLocalPath(path, 5000 + int(_uploads++)));
    }
    const Ts ts = m.ts;
    _store.addMessage(conv, std::move(m));
    // Confirm asynchronously: a real server never answers synchronously.
    later(kSendConfirmMs, [this, conv, ts, threadTs, done = std::move(done)] {
        if (!_store.findMessage(conv, ts)) { // undone while in flight (undo send)
            if (done)
                done(false, "message_deleted");
            return;
        }
        _store.updateMessage(conv, ts, [](model::Message &msg) { msg.pending = false; });
        _store.markRead(conv, ts); // my own message reads the conversation up to it
        if (done)
            done(true, {});
        scheduleUnfurl(conv, ts);
        scheduleAutoReply(conv, threadTs);
    });
}

void FakeBackend::edit(ConvRef conv, Ts ts, std::string text) {
    _store.updateMessage(conv, ts, [&](model::Message &m) {
        m.text   = std::move(text);
        m.edited = true;
    });
}

void FakeBackend::remove(ConvRef conv, Ts ts) {
    _store.removeMessage(conv, ts);
}

void FakeBackend::react(ConvRef conv, Ts ts, std::string_view name, bool add) {
    _store.setReaction(conv, ts, name, _store.me, add);
}

void FakeBackend::markRead(ConvRef conv, Ts ts) {
    _store.markRead(conv, ts);
}

// The conversation / message flag setters share one Store edit each (a
// lambda per setter would be a type-erased function apiece).
void FakeBackend::setConvFlag(ConvRef conv, ConvFlag f, int v) {
    _store.updateConversation(conv, [f, v](model::Conversation &c) {
        switch (f) {
        case ConvFlag::Starred:
            c.starred = v != 0;
            break;
        case ConvFlag::Muted:
            c.muted = v != 0;
            break;
        case ConvFlag::Notify:
            c.notify = model::NotifyLevel(v);
            break;
        case ConvFlag::Member:
            c.member = v != 0;
            if (!v)
                c.starred = false; // Slack drops a left channel from Starred too
            break;
        }
    });
}

void FakeBackend::setMessageFlag(ConvRef conv, Ts ts, bool pinFlag, bool on) {
    const model::UserRef me = _store.me;
    _store.updateMessage(conv, ts, [pinFlag, on, me](model::Message &m) {
        (pinFlag ? m.pinned : m.saved) = on;
        if (pinFlag)
            m.pinnedBy = on ? me : model::kNoUser;
    });
}

void FakeBackend::setStarred(ConvRef conv, bool starred) {
    if (starred)
        leaveSection(conv);
    setConvFlag(conv, ConvFlag::Starred, starred);
}

void FakeBackend::markUnread(ConvRef conv, Ts ts) {
    _store.markUnread(conv, ts);
}

void FakeBackend::setMuted(ConvRef conv, bool muted) {
    setConvFlag(conv, ConvFlag::Muted, muted);
}

void FakeBackend::setNotifyLevel(ConvRef conv, model::NotifyLevel level) {
    setConvFlag(conv, ConvFlag::Notify, int(level));
}

void FakeBackend::leave(ConvRef conv) {
    setConvFlag(conv, ConvFlag::Member, 0);
}

void FakeBackend::openDm(model::UserRef user, std::function<void(ConvRef)> done) {
    later(kReadLatencyMs, [this, user, done = std::move(done)] {
        ConvRef found = model::kNoConv;
        if (user < _store.userCount()) {
            for (ConvRef c = 0; c < _store.conversationCount() && found == model::kNoConv; ++c) {
                const auto &cv = _store.conversation(c);
                if (cv.kind == model::ConvKind::Dm && cv.dmUser == user)
                    found = c;
            }
            if (found == model::kNoConv) { // conversations.open creates it
                model::Conversation c;
                c.id            = "D" + _store.user(user).id;
                c.name          = _store.user(user).name;
                c.kind          = model::ConvKind::Dm;
                c.dmUser        = user;
                c.memberCount   = 2;
                c.hasMoreBefore = false;
                found           = _store.addConversation(std::move(c));
            } else if (!_store.conversation(found).member) {
                setConvFlag(found, ConvFlag::Member, 1);
            }
        }
        if (done)
            done(found);
    });
}

void FakeBackend::joinChannel(ConvRef conv, ConvDone done) {
    later(kReadLatencyMs, [this, conv, done = std::move(done)] {
        if (conv >= _store.conversationCount() || _store.conversation(conv).isDirect()) {
            if (done)
                done(model::kNoConv, "channel_not_found");
            return;
        }
        _store.updateConversation(conv, [](model::Conversation &c) {
            if (!c.member)
                ++c.memberCount;
            c.member = true;
        });
        if (done)
            done(conv, {});
    });
}

void FakeBackend::createChannel(std::string name, bool isPrivate, ConvDone done) {
    later(kReadLatencyMs, [this, name = std::move(name), isPrivate, done = std::move(done)] {
        for (ConvRef c = 0; c < _store.conversationCount(); ++c)
            if (!_store.conversation(c).isDirect() && _store.conversation(c).name == name) {
                if (done)
                    done(model::kNoConv, "name_taken");
                return;
            }
        model::Conversation c;
        c.id            = "C0" + str::number(_store.conversationCount()) + "NEW";
        c.name          = name;
        c.kind          = isPrivate ? model::ConvKind::Private : model::ConvKind::Channel;
        c.memberCount   = 1;
        c.hasMoreBefore = false;
        const ConvRef r = _store.addConversation(std::move(c));
        if (done)
            done(r, {});
    });
}

void FakeBackend::setPinned(ConvRef conv, Ts ts, bool pinned) {
    setMessageFlag(conv, ts, true, pinned);
}

void FakeBackend::setSaved(ConvRef conv, Ts ts, bool saved) {
    setMessageFlag(conv, ts, false, saved);
    _store.setReminderAt(conv, ts, 0);
    _store.setSavedItem(conv, ts, saved, 0);
}

void FakeBackend::setReminder(ConvRef conv, Ts ts, int64_t due) {
    // A reminder is a saved item with a due date; removing it removes both.
    // (The fake never fires it: Slack's due time only moves it in Later.)
    setMessageFlag(conv, ts, false, due > 0);
    _store.setReminderAt(conv, ts, due);
    _store.setSavedItem(conv, ts, due > 0, due);
}

void FakeBackend::deleteFile(ConvRef conv, Ts ts, const std::string &fileId) {
    _store.updateMessage(conv, ts, [&fileId](model::Message &m) {
        if (m.extra)
            std::erase_if(m.extra->files, [&](const model::File &f) { return f.id == fileId; });
    });
}

FakeBackend::Capabilities FakeBackend::capabilities() const {
    Capabilities c;
    c.memberList       = true;
    c.threadsView      = true;
    c.messageReminders = true;
    c.canvases         = true;
    c.fileUpload       = true;
    c.customSections   = true; // this run only
    return c;
}

void FakeBackend::setPresence(bool, Done done) {
    // Accepted, and nothing changes: the snapshot it
    // reports stays phantom-away, so the toggle settles back.
    later(kReadLatencyMs, [done = std::move(done)] {
        if (done)
            done(true, {});
    });
}

void FakeBackend::setStatus(std::string emoji, std::string text, int64_t, Done done) {
    later(kReadLatencyMs, [this, emoji = std::move(emoji), text = std::move(text), done] {
        if (_store.me != model::kNoUser) {
            model::User &u = _store.user(_store.me);
            u.statusEmoji  = emoji;
            u.statusText   = text;
            _store.usersChanged();
        }
        if (done)
            done(true, {});
    });
}

void FakeBackend::loadMembers(ConvRef conv, MembersDone done) {
    // A group DM names its members; a fixture channel only carries a count,
    // so it holds everyone who is not a bot.
    std::vector<model::UserRef> members;
    if (conv < _store.conversationCount())
        members = _store.conversation(conv).members;
    if (members.empty())
        for (model::UserRef u = 0; u < _store.userCount(); ++u)
            if (!_store.user(u).bot && !_store.user(u).placeholder)
                members.push_back(u);
    later(kReadLatencyMs, [members = std::move(members), done = std::move(done)] {
        if (done)
            done(members, {});
    });
}

int64_t FakeBackend::nowSecs() const {
    return _now ? _now : Backend::nowSecs();
}

void FakeBackend::loadMyProfile(std::function<void(MyProfile)> done) {
    later(kReadLatencyMs, [this, done = std::move(done)] {
        const model::User &me = _store.user(_store.me);
        if (done)
            done({me.displayName, me.name, me.email, _phone, me.avatar});
    });
}

void FakeBackend::updateProfile(std::string name, std::string email, std::string phone, Done done) {
    later(
        kReadLatencyMs,
        [this,
         name  = std::move(name),
         email = std::move(email),
         phone = std::move(phone),
         done  = std::move(done)] {
            if (_store.me != model::kNoUser) {
                model::User &u = _store.user(_store.me);
                u.displayName  = name;
                u.email        = email;
                _phone         = phone;
                _store.usersChanged();
            }
            if (done)
                done(true, {});
        }
    );
}

void FakeBackend::setPhoto(std::string path, Done done) {
    later(kReadLatencyMs, [this, path = std::move(path), done = std::move(done)] {
        if (_store.me != model::kNoUser) {
            _store.user(_store.me).avatar = path;
            _store.usersChanged();
        }
        if (done)
            done(true, {});
    });
}

void FakeBackend::searchGifs(std::string, std::function<void(std::vector<Gif>)> done) {
    std::vector<Gif>            gifs;
    std::vector<file::DirEntry> entries;
    const std::string           dir = _fx.gifDir;
    if (file::listDir(dir, &entries))
        for (const file::DirEntry &e : entries) {
            if (e.isDir || file::extension(e.name) != "gif")
                continue;
            Gif g;
            g.url = g.preview = dir + "/" + e.name;
            // The title: the file name, dashes as spaces, capitalised.
            g.title           = e.name.substr(0, e.name.size() - 4); // less ".gif"
            for (char &c : g.title)
                if (c == '-')
                    c = ' ';
            if (!g.title.empty() && g.title[0] >= 'a' && g.title[0] <= 'z')
                g.title[0] = char(g.title[0] - 'a' + 'A');
            std::string head; // the logical screen size: bytes 6-9 of a GIF
            if (file::readRange(g.url, 0, 10, &head) && head.size() >= 10) {
                g.width  = uint8_t(head[6]) | uint8_t(head[7]) << 8;
                g.height = uint8_t(head[8]) | uint8_t(head[9]) << 8;
            }
            gifs.push_back(std::move(g));
        }
    std::sort(gifs.begin(), gifs.end(), [](const Gif &a, const Gif &b) { return a.url < b.url; });
    later(kReadLatencyMs, [gifs = std::move(gifs), done = std::move(done)] {
        if (done)
            done(gifs);
    });
}

void FakeBackend::userTyping(ConvRef, Ts) {
    // Nobody is watching the fake workspace type.
}

void FakeBackend::search(std::string query, std::function<void(std::vector<SearchHit>)> done) {
    std::vector<SearchHit> hits;
    // Trimmed, case-insensitive substring over the rendered text (so a query
    // never matches markup like "<@U…>").
    std::string_view       q = query;
    while (!q.empty() && q.front() == ' ')
        q.remove_prefix(1);
    while (!q.empty() && q.back() == ' ')
        q.remove_suffix(1);
    if (!q.empty()) {
        for (ConvRef c = 0; c < _store.conversationCount(); ++c) {
            const auto &conv = _store.conversation(c);
            auto        scan = [&](const std::vector<model::Message> &msgs, Ts thread) {
                for (const auto &m : msgs)
                    if (utf8::containsFolded(mrkdwn::parse(m.text).text, q))
                        hits.push_back(SearchHit{c, m.ts, thread, m.text});
            };
            scan(conv.messages, 0);
            for (const auto &t : conv.threads)
                scan(t.replies, t.root);
        }
        std::sort(hits.begin(), hits.end(), [](const SearchHit &a, const SearchHit &b) {
            return a.ts > b.ts;
        });
    }
    later(kReadLatencyMs * 4, [done = std::move(done), hits = std::move(hits)]() mutable {
        if (done)
            done(std::move(hits));
    });
}

// ── Canvases ────────────────────────────────────────────────────────────────

namespace {

// Canvas markdown's inline marks (**b**, _i_ / *i*, ~~s~~, `c`, [t](u)) as
// the tags Slack's canvas HTML uses.
std::string inlineHtml(std::string_view t) {
    std::string out;
    bool        on[4] = {};
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '`') {
            const size_t e = t.find('`', i + 1);
            if (e != std::string_view::npos) {
                out += "<code>";
                str::appendEscapedHtml(&out, t.substr(i + 1, e - i - 1));
                out += "</code>";
                i = e + 1;
                continue;
            }
        }
        if (t[i] == '[') {
            const size_t close = t.find("](", i);
            const size_t end   = close == std::string_view::npos ? close : t.find(')', close);
            if (end != std::string_view::npos) {
                out += "<a href=\"";
                str::appendEscapedHtml(&out, t.substr(close + 2, end - close - 2));
                out += "\">";
                str::appendEscapedHtml(&out, t.substr(i + 1, close - i - 1));
                out += "</a>";
                i = end + 1;
                continue;
            }
        }
        static const struct {
            std::string_view mark, tag;
        } kMarks[] = {{"**", "b"}, {"~~", "del"}, {"_", "i"}, {"*", "i"}};
        bool hit   = false;
        for (int k = 0; k < 4; ++k)
            if (t.substr(i, kMarks[k].mark.size()) == kMarks[k].mark) {
                const int slot = k == 3 ? 2 : k;
                out += on[slot] ? "</" : "<";
                out += kMarks[k].tag;
                out += '>';
                on[slot] = !on[slot];
                i += kMarks[k].mark.size();
                hit = true;
                break;
            }
        if (!hit)
            str::appendEscapedHtml(&out, t.substr(i++, 1));
    }
    return out;
}

// Canvas markdown → the HTML a canvas file serves: one block per line, each
// with a section id as Slack's carry ("temp:C:f<n>", from *seq).
std::string canvasMarkdownHtml(std::string_view md, uint32_t *seq) {
    std::string out;
    auto        id = [seq] { return str::concat({" id=\"temp:C:f", str::number(++*seq), "\""}); };
    std::string list; // the open list's tag
    auto        closeList = [&] {
        if (!list.empty())
            out += str::concat({"</", list, ">\n"});
        list.clear();
    };
    str::Splitter lines(md, '\n');
    for (std::string_view line; lines.next(&line);) {
        line = str::trim(line);
        if (line.empty())
            continue;
        std::string_view tag = "p", li;
        if (str::startsWith(line, "### "))
            tag = "h3", line.remove_prefix(4);
        else if (str::startsWith(line, "## "))
            tag = "h2", line.remove_prefix(3);
        else if (str::startsWith(line, "# "))
            tag = "h1", line.remove_prefix(2);
        else if (str::startsWith(line, "> "))
            tag = "blockquote", line.remove_prefix(2);
        else if (str::startsWith(line, "- [ ] ") || str::startsWith(line, "- [x] "))
            li = line[3] == 'x' ? "checked" : "checklist", line.remove_prefix(6);
        else if (str::startsWith(line, "- ") || str::startsWith(line, "* "))
            li = "ul", line.remove_prefix(2);
        else if (
            line.size() > 2 && line[0] >= '0' && line[0] <= '9' &&
            line.find(". ") != std::string_view::npos && line.find(". ") < 4
        )
            li = "ol", line.remove_prefix(line.find(". ") + 2);
        if (!li.empty()) {
            const std::string_view want = li == "ol" ? "ol" : "ul";
            if (list != want) {
                closeList();
                list = std::string(want);
                out += str::concat(
                    {"<", list, id(), li == "ul" || li == "ol" ? "" : " class=\"checklist\"", ">\n"}
                );
            }
            out += li == "checked" ? str::concat({"<li", id(), " class=\"checked\">"})
                                   : str::concat({"<li", id(), ">"});
            out += inlineHtml(line);
            out += "</li>\n";
            continue;
        }
        closeList();
        if (tag == "blockquote")
            out +=
                str::concat({"<blockquote><p", id(), ">", inlineHtml(line), "</p></blockquote>\n"});
        else
            out += str::concat({"<", tag, id(), ">", inlineHtml(line), "</", tag, ">\n"});
    }
    closeList();
    return out;
}

// [start, end) of the top-level element of `html` that holds `id`;
// false when none does (canvases.edit's invalid section).
bool sectionRange(std::string_view html, std::string_view id, size_t *start, size_t *end) {
    size_t at = html.find(str::concat({"id=\"", id, "\""}));
    if (at == std::string_view::npos)
        at = html.find(str::concat({"id='", id, "'"}));
    if (at == std::string_view::npos)
        return false;
    // Walk the top-level elements to the one containing it.
    for (size_t pos = 0; pos < html.size();) {
        pos = html.find('<', pos);
        if (pos == std::string_view::npos)
            return false;
        size_t ne = pos + 1;
        while (ne < html.size() && html[ne] != ' ' && html[ne] != '>' && html[ne] != '/')
            ++ne;
        const std::string_view name  = html.substr(pos + 1, ne - pos - 1);
        int                    depth = 0;
        size_t                 e     = pos;
        for (size_t p = pos; p < html.size();) {
            const size_t lt = html.find('<', p);
            const size_t gt = lt == std::string_view::npos ? lt : html.find('>', lt);
            if (gt == std::string_view::npos)
                return false;
            std::string_view tag   = html.substr(lt + 1, gt - lt - 1);
            const bool       close = !tag.empty() && tag[0] == '/';
            if (close)
                tag.remove_prefix(1);
            if (str::startsWith(tag, name) &&
                (tag.size() == name.size() || tag[name.size()] == ' ' || tag[name.size()] == '>'))
                depth += close ? -1 : 1;
            p = gt + 1;
            if (depth == 0) {
                e = p;
                break;
            }
        }
        if (at > pos && at < e) {
            *start = pos;
            *end   = e;
            while (*end < html.size() && html[*end] == '\n')
                ++*end;
            return true;
        }
        pos = e;
    }
    return false;
}

} // namespace

ConvRef FakeBackend::canvasConv(std::string_view fileId) const {
    for (ConvRef r = 0; r < _store.conversationCount(); ++r)
        if (_store.conversation(r).canvasId == fileId)
            return r;
    return model::kNoConv;
}

void FakeBackend::loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) {
    const ConvRef conv = canvasConv(fileId);
    std::string   title, link;
    if (conv != model::kNoConv) {
        title = _store.conversation(conv).canvasTitle;
        link  = str::concat({_store.conversationLink(conv), "/canvas/", fileId});
    }
    later(kReadLatencyMs, [conv, title, link, done = std::move(done)] {
        if (done)
            done(title, link, conv == model::kNoConv ? CanvasState::Gone : CanvasState::Ok);
    });
}

void FakeBackend::loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) {
    const ConvRef conv = canvasConv(fileId);
    std::string   html;
    bool          edited = false;
    for (const CanvasEdit &e : _canvasEdits)
        if (e.id == fileId) {
            html   = e.html;
            edited = true;
        }
    if (!edited)
        for (const auto &c : _fx.canvases)
            if (c.conv == conv)
                file::readAll(c.htmlPath, &html);
    // As Slack serves it: the title as the leading h1.
    if (edited && conv != model::kNoConv)
        html = str::concat(
            {"<h1>", inlineHtml(_store.conversation(conv).canvasTitle), "</h1>\n", html}
        );
    later(kReadLatencyMs, [conv, html = std::move(html), done = std::move(done)]() mutable {
        if (done)
            done(
                std::move(html),
                conv == model::kNoConv ? std::string("file_not_found") : std::string()
            );
    });
}

void FakeBackend::createChannelCanvas(ConvRef conv, std::string markdown, CanvasCreated done) {
    const bool has =
        conv < _store.conversationCount() && !_store.conversation(conv).canvasId.empty();
    std::string id;
    if (conv < _store.conversationCount() && !has) {
        id = str::concat({"FNEW", str::number(++_canvasSeq)});
        _canvasEdits.push_back({id, canvasMarkdownHtml(markdown, &_canvasSeq)});
    }
    later(kReadLatencyMs, [this, conv, has, id, done = std::move(done)] {
        if (!id.empty())
            _store.updateConversation(conv, [&](model::Conversation &c) {
                c.canvasId    = id;
                c.canvasTitle = "Untitled";
            });
        if (done)
            done(
                id,
                id.empty() ? std::string(has ? "canvas_already_exists" : "channel_not_found")
                           : std::string()
            );
    });
}

void FakeBackend::editCanvas(
    const std::string &fileId, std::vector<CanvasChange> changes, Done done
) {
    later(kReadLatencyMs, [this, fileId, changes = std::move(changes), done = std::move(done)] {
        const ConvRef conv = canvasConv(fileId);
        if (conv == model::kNoConv) {
            if (done)
                done(false, "canvas_not_found");
            return;
        }
        // The body as served now (a fixture canvas: its file, without the
        // title heading, which is the conversation's canvasTitle here).
        std::string body;
        bool        edited = false;
        for (const CanvasEdit &e : _canvasEdits)
            if (e.id == fileId)
                body = e.html, edited = true;
        if (!edited)
            for (const auto &cv : _fx.canvases)
                if (cv.conv == conv && file::readAll(cv.htmlPath, &body)) {
                    const size_t h1 = body.find("<h1"), end = body.find("</h1>");
                    if (h1 != std::string::npos && end != std::string::npos &&
                        body.find_first_not_of(" \t\r\n") == h1)
                        body.erase(0, end + 5);
                }
        using Op = CanvasChange::Op;
        for (const CanvasChange &c : changes) {
            if (c.op == Op::Rename) {
                _store.updateConversation(conv, [&](model::Conversation &x) {
                    x.canvasTitle = c.markdown;
                });
                continue;
            }
            if (c.op == Op::ReplaceAll) {
                body = canvasMarkdownHtml(c.markdown, &_canvasSeq);
                continue;
            }
            // A section op: on the top-level element holding the id.
            size_t a = 0, b = 0;
            if (!sectionRange(body, c.sectionId, &a, &b)) {
                if (done)
                    done(false, "invalid_section");
                return;
            }
            const std::string fresh = c.op == Op::DeleteSection
                                          ? std::string()
                                          : canvasMarkdownHtml(c.markdown, &_canvasSeq);
            if (c.op == Op::InsertBefore)
                body.insert(a, fresh);
            else if (c.op == Op::InsertAfter)
                body.insert(b, fresh);
            else
                body.replace(a, b - a, fresh);
        }
        std::erase_if(_canvasEdits, [&](const CanvasEdit &e) { return e.id == fileId; });
        _canvasEdits.push_back({fileId, std::move(body)});
        if (done)
            done(true, {});
    });
}

void FakeBackend::deleteCanvas(const std::string &fileId, Done done) {
    later(kReadLatencyMs, [this, fileId, done = std::move(done)] {
        const ConvRef conv = canvasConv(fileId);
        std::erase_if(_canvasEdits, [&](const CanvasEdit &e) { return e.id == fileId; });
        std::erase_if(_fx.canvases, [&](const Canvas &c) { return c.conv == conv; });
        if (conv != model::kNoConv)
            _store.updateConversation(conv, [](model::Conversation &x) {
                x.canvasId.clear();
                x.canvasTitle.clear();
            });
        if (done)
            done(conv != model::kNoConv, conv == model::kNoConv ? "canvas_not_found" : "");
    });
}

std::string_view FakeBackend::aiReply(std::string_view request) const {
    for (const auto &r : _fx.aiReplies)
        if (utf8::containsFolded(request, r.match))
            return r.text;
    return _fx.aiDefault;
}

Ts FakeBackend::postAs(ConvRef conv, model::UserRef user, std::string text, Ts thread) {
    model::Message m;
    m.ts        = nextTs();
    m.threadTs  = thread;
    m.user      = user;
    m.text      = std::move(text);
    const Ts ts = m.ts;
    _store.addMessage(conv, std::move(m));
    scheduleUnfurl(conv, ts);
    return ts;
}

Ts findTs(const model::Store &store, ConvRef conv, std::string_view fragment) {
    if (conv >= store.conversationCount())
        return 0;
    // Text, then file names and attachment titles: a voice clip or an image
    // post has no text of its own.
    const auto hit = [fragment](const model::Message &m) {
        if (utf8::containsFolded(mrkdwn::parse(m.text).text, fragment))
            return true;
        for (const model::File &f : m.files())
            if (utf8::containsFolded(f.name, fragment))
                return true;
        for (const model::Attachment &a : m.attachments())
            if (utf8::containsFolded(a.title, fragment))
                return true;
        return false;
    };
    const auto &c = store.conversation(conv);
    for (const auto &m : c.messages)
        if (hit(m))
            return m.ts;
    for (const auto &t : c.threads)
        for (const auto &m : t.replies)
            if (hit(m))
                return m.ts;
    return 0;
}

void FakeBackend::scheduleAutoReply(ConvRef conv, Ts thread) {
    // The next unused reply for this conversation, top-level or in-thread
    // matching the send.
    size_t i = 0;
    for (; i < _fx.autoReplies.size(); ++i)
        if (_replyUsed[i] == 0 && _fx.autoReplies[i].conv == conv &&
            _fx.autoReplies[i].inThread == (thread != 0))
            break;
    if (i == _fx.autoReplies.size())
        return;
    _replyUsed[i]            = 1;
    const AutoReply reply    = _fx.autoReplies[i];
    const int       typingMs = std::max(0, reply.typingMs);
    later(reply.afterMs, [this, conv, thread, reply, typingMs] {
        if (typingMs > 0)
            _store.setTyping(conv, reply.user, thread, true);
        later(typingMs, [this, conv, thread, reply] {
            postAs(conv, reply.user, reply.text, thread); // clears the typing indicator
        });
    });
}

void FakeBackend::scheduleUnfurl(ConvRef conv, Ts ts) {
    const model::Message *m = _store.findMessage(conv, ts);
    if (!m || m->text.find("://") == std::string::npos)
        return;
    // Links as the text holds them: <url|label>, <url> or bare.
    std::vector<model::Attachment> atts;
    const std::string             &t = m->text;
    for (size_t p = t.find("http"); p != std::string::npos; p = t.find("http", p + 1)) {
        if (t.compare(p, 7, "http://") != 0 && t.compare(p, 8, "https://") != 0)
            continue;
        size_t e = p;
        while (e < t.size() && t[e] > ' ' && t[e] != '<' && t[e] != '>' && t[e] != '|')
            ++e;
        const std::string_view url(t.data() + p, e - p);
        for (const auto &u : _fx.unfurls) {
            if (url.substr(0, u.url.size()) == u.url) {
                atts.push_back(u.attachment);
                break;
            }
        }
        p = e;
        if (p >= t.size())
            break;
    }
    if (atts.empty())
        return;
    later(kUnfurlDelayMs, [this, conv, ts, atts = std::move(atts)]() mutable {
        _store.updateMessage(conv, ts, [&](model::Message &msg) {
            auto &list = msg.extras().attachments;
            list.insert(
                list.end(),
                std::make_move_iterator(atts.begin()),
                std::make_move_iterator(atts.end())
            );
        });
    });
}

} // namespace fake
