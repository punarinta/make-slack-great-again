#include "app/legacy/old_cache_import.h"

#include "app/cache/workspace_cache.h"
#include "app/identity.h"
#include "app/model/store.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/str.h"

#include <algorithm>
#include <set>

namespace legacy {

using model::ConvRef;
using model::kNoConv;

namespace {

// "C0123\t1712345678.000100": a thread or reminder key.
struct ConvTs {
    std::string conv;
    model::Ts   ts = 0;
};
ConvTs splitKey(std::string_view key) {
    const size_t tab = key.find('\t');
    if (tab == std::string_view::npos)
        return {};
    return {std::string(key.substr(0, tab)), model::parseTs(key.substr(tab + 1))};
}

model::User toUser(const json::Value &o) {
    model::User u;
    u.id          = o["id"].str();
    u.name        = o["na"].str();
    u.displayName = o["dn"].str();
    u.avatar      = o["av"].str();
    u.bot         = o["bo"].boolean();
    u.stranger    = o["ex"].boolean(); // is_stranger
    u.active      = o["ac"].boolean();
    u.deleted     = o["de"].boolean();
    u.admin       = o["ad"].boolean();
    u.owner       = o["ow"].boolean();
    u.statusEmoji = o["se"].str();
    u.statusText  = o["st"].str();
    u.title       = o["ti"].str();
    u.email       = o["em"].str();
    u.hasTz       = o.has("tz");
    u.tzOffset    = int32_t(o["tz"].integer());
    return u;
}

// The stored levels { Default, All, Mentions, Mute }: Mute was the
// conversation's mute (setting the level set the mute flag with it), Default
// and All both "All new posts" here. No unread counts: the cached ones may
// be weeks stale, and the backend keeps the larger of the cached
// and the server's (SlackBackend::Read::carryLocal) — a badge that would
// never clear. The server's answer brings them.
model::Conversation toConversation(const json::Value &o, model::Store &s) {
    model::Conversation c;
    c.id                = o["id"].str();
    c.kind              = model::ConvKind(std::clamp<int64_t>(o["ki"].integer(), 0, 3));
    c.name              = o["na"].str();
    c.member            = o["mb"].boolean(true);
    c.lastRead          = model::parseTs(o["lr"].str());
    c.latest            = model::parseTs(o["lt"].str());
    c.starred           = o["st"].boolean();
    c.localName         = o["ln"].str();
    const int64_t level = o["nl"].integer();
    // "mute this person" (lm) is the DM's mute here.
    c.muted             = o["mu"].boolean() || o["lm"].boolean() || level == 3;
    // nl: 0 Default (follow the global level), 1 All, 2 Mentions, 3 Mute.
    c.notify            = level == 1   ? model::NotifyLevel::All
                          : level == 2 ? model::NotifyLevel::Mentions
                                       : model::NotifyLevel::Default;
    if (const std::string_view dm = o["dm"].str(); !dm.empty())
        c.dmUser = s.internUser(dm);
    return c;
}

// The old meta.json's reminders with their previews, as the Slack backend's
// "saved" entries (SlackBackend::Read::saveExtras).
void writeSaved(json::Writer &w, const json::Value &meta) {
    struct Preview {
        std::string_view root, snippet, author, botName, botAvatar;
    };
    const auto previewOf = [](const json::Value &o) {
        return Preview{
            o["root"].str(),
            o["snippet"].str(),
            o["author"].str(),
            o["botName"].str(),
            o["botAvatar"].str()
        };
    };
    std::vector<std::pair<std::string_view, Preview>> shadows; // old reminderPreviews
    for (const json::Value p : meta["reminderPreviews"])
        shadows.emplace_back(p["key"].str(), previewOf(p));
    w.key("saved").beginArray();
    for (const json::Value r : meta["reminders"]) {
        const std::string_view conv = r["conv"].str();
        const model::Ts        ts   = model::parseTs(r["ts"].str());
        if (conv.empty() || !ts)
            continue;
        Preview p = previewOf(r);
        if (p.snippet.empty() && p.author.empty() && p.botName.empty()) {
            const std::string key = str::concat({conv, "\t", r["ts"].str()});
            for (const auto &[k, shadow] : shadows)
                if (k == key)
                    p = shadow; // the preview kept beside the reminder
        }
        w.beginArray().value(conv).value(int64_t(ts)).value(r["due"].integer());
        w.value(r["saved"].integer()).value(r["fired"].boolean());
        if (!p.snippet.empty() || !p.author.empty() || !p.botName.empty())
            w.value(p.snippet)
                .value(p.author)
                .value(int64_t(model::parseTs(p.root)))
                .value(p.botName)
                .value(p.botAvatar);
        w.endArray();
    }
    w.endArray();
}

void writeFollowed(json::Writer &w, const std::vector<ConvTs> &followed) {
    w.key("followed").beginArray();
    for (const ConvTs &t : followed)
        w.beginArray().value(t.conv).value(int64_t(t.ts)).endArray();
    w.endArray();
}

// The Slack backend's "x" of a cold import: everything the old cache had.
void writeColdExtras(
    json::Writer              &w,
    const json::Value         &meta,
    const std::vector<ConvTs> &followed,
    const model::Store        &s
) {
    writeSaved(w, meta);
    writeFollowed(w, followed);
    w.key("probed").beginArray();
    for (const json::Value p : meta["userProbeTimes"]) // unix ms → unix secs
        if (!p.key().empty())
            w.beginArray().value(p.key()).value(int64_t(p.number()) / 1000).endArray();
    w.endArray().key("sweep").value(int64_t(meta["sweepAt"].number()));
    w.key("dead").beginArray();
    for (const json::Value d : meta["deadConvIds"])
        if (!d.str().empty())
            w.value(d.str());
    w.endArray().key("ug").beginArray();
    for (const model::Store::Usergroup &g : s.usergroups()) {
        w.beginArray().value(g.id).value(g.handle).value(g.name).beginArray();
        for (const std::string &u : g.users)
            w.value(u);
        w.endArray().endArray();
    }
    w.endArray();
}

// Over an existing new cache: its "x" as it was, the old followed threads
// added to its own.
void writeWarmExtras(json::Writer &w, const json::Value &x, std::vector<ConvTs> followed) {
    for (const json::Value v : x["followed"]) {
        ConvTs t{std::string(v[0].str()), v[1].integer()};
        if (std::none_of(followed.begin(), followed.end(), [&](const ConvTs &f) {
                return f.conv == t.conv && f.ts == t.ts;
            }))
            followed.push_back(std::move(t));
    }
    for (const json::Value v : x)
        if (v.key() != "followed")
            w.key(v.key()).value(v);
    writeFollowed(w, followed);
}

std::string safeName(std::string_view key) {
    std::string out(key);
    std::replace(out.begin(), out.end(), ':', '_');
    return out;
}

std::vector<std::string> readLines(const std::string &path) {
    std::string              text;
    std::vector<std::string> out;
    if (!file::readAll(path, &text))
        return out;
    str::Splitter lines(text, '\n');
    for (std::string_view line; lines.next(&line);)
        if (line = str::trim(line); !line.empty())
            out.emplace_back(line);
    return out;
}

} // namespace

std::string oldCacheDir(plat::App &app, const std::string &key) {
    const std::string data = identity::dataDir(app);
    if (data.empty() || key.empty())
        return {};
    const std::string dir = file::join(data, "cache/" + safeName(key));
    if (file::isDir(dir))
        return dir;
    // Before multi-service: the bare team id.
    const size_t colon = key.find(':');
    if (colon == std::string::npos)
        return {};
    const std::string bare = file::join(data, "cache/" + key.substr(colon + 1));
    return file::isDir(bare) ? bare : std::string();
}

bool importSlackCache(plat::App &app, const std::string &from, const std::string &to) {
    json::Document convs, meta;
    if (to.empty() || !convs.parseFile(file::join(from, "conversations.json")) ||
        !convs.root().isArray())
        return false;
    meta.parseFile(file::join(from, "meta.json"));
    const json::Value m = meta.root();

    model::Store          s;
    cache::WorkspaceCache wc(app, s, to);
    json::Document        cur; // the new cache's meta.json, when there is one
    const bool            warm = wc.load(&cur);

    std::vector<ConvTs> followed;
    for (const json::Value k : m["followedThreads"])
        if (ConvTs t = splitKey(k.str()); !t.conv.empty() && t.ts)
            followed.push_back(std::move(t));

    if (!warm) {
        // Users and bots before the conversations: their refs resolve.
        for (const char *name : {"users.json", "bots.json"}) {
            json::Document d;
            if (d.parseFile(file::join(from, name)))
                for (const json::Value o : d.root())
                    if (!o["id"].str().empty())
                        s.addUser(toUser(o));
        }
        if (const std::string_view me = m["meId"].str(); !me.empty())
            s.me = s.internUser(me);
        s.usersChanged();
        for (const json::Value o : convs.root())
            if (!o["id"].str().empty())
                s.addConversation(toConversation(o, s));
        if (json::Document d; d.parseFile(file::join(from, "emoji.json"))) {
            for (const json::Value e : d.root())
                if (!e.key().empty() && !e.str().empty())
                    s.setCustomEmoji(std::string(e.key()), std::string(e.str()));
            wc.emojiChanged();
        }
        if (json::Document d; d.parseFile(file::join(from, "usergroups.json"))) {
            std::vector<model::Store::Usergroup> groups;
            for (const json::Value o : d.root()) {
                model::Store::Usergroup g{
                    std::string(o["id"].str()),
                    std::string(o["ha"].str()),
                    std::string(o["na"].str()),
                    {}
                };
                for (const json::Value u : o["us"])
                    g.users.emplace_back(u.str());
                if (!g.id.empty())
                    groups.push_back(std::move(g));
            }
            s.setUsergroups(std::move(groups));
        }
        for (const json::Value r : m["reminders"])
            if (const ConvRef c = s.findConversation(r["conv"].str());
                c != kNoConv && r["due"].integer() > 0)
                s.setReminderAt(c, model::parseTs(r["ts"].str()), r["due"].integer());
        if (const ConvRef c = s.findConversation(m["conv"].str()); c != kNoConv)
            wc.setLastConversation(c);
    } else {
        // Only the app's own state, where the new cache has none.
        for (const json::Value o : convs.root()) {
            const ConvRef c = s.findConversation(o["id"].str());
            if (c == kNoConv)
                continue;
            const model::Conversation  old = toConversation(o, s);
            const model::Conversation &cv  = s.conversation(c);
            if ((old.muted && !cv.muted) ||
                (old.notify != model::NotifyLevel::Default &&
                 cv.notify == model::NotifyLevel::Default) ||
                (!old.localName.empty() && cv.localName.empty()))
                s.updateConversation(c, [&old](model::Conversation &x) {
                    x.muted = x.muted || old.muted;
                    if (x.notify == model::NotifyLevel::Default)
                        x.notify = old.notify;
                    if (x.localName.empty())
                        x.localName = old.localName;
                });
        }
    }
    for (const json::Value k : m["mutedThreads"]) {
        const ConvTs  t = splitKey(k.str());
        const ConvRef c = s.findConversation(t.conv);
        if (c != kNoConv && t.ts && !s.threadMuted(c, t.ts))
            s.setThreadMuted(c, t.ts, true);
    }
    for (const json::Value t : m["aiTranscripts"])
        if (!t.key().empty() && !t["text"].str().empty() && !s.aiTranscript(std::string(t.key())))
            s.setAiTranscript(
                std::string(t.key()), std::string(t["text"].str()), std::string(t["by"].str())
            );

    const json::Value x = cur.root()["x"];
    wc.saveExtras       = [&](json::Writer &w) {
        if (warm)
            writeWarmExtras(w, x, followed);
        else
            writeColdExtras(w, m, followed, s);
    };
    wc.extrasChanged(); // meta.json, whatever else changed
    wc.flush();
    wc.close(false);
    LOG_INFO(
        "legacy",
        "imported the old cache %s into %s (%s)",
        from.c_str(),
        to.c_str(),
        warm ? "local state only" : "whole"
    );
    return true;
}

bool importClaudeCodeCache(const std::string &from, const std::string &knownPath) {
    json::Document convs, known;
    if (!convs.parseFile(file::join(from, "conversations.json")) || !known.parseFile(knownPath) ||
        !known.root().isObject())
        return false;
    model::Store dummy; // toConversation interns the DM peer somewhere
    struct Local {
        std::string        id;
        bool               starred, muted;
        model::NotifyLevel notify;
    };
    std::vector<Local> local;
    for (const json::Value o : convs.root()) {
        const model::Conversation c = toConversation(o, dummy);
        if (!c.id.empty() && (c.starred || c.muted || c.notify != model::NotifyLevel::Default))
            local.push_back({c.id, c.starred, c.muted, c.notify});
    }
    bool         changed = false;
    json::Writer w;
    w.beginObject();
    for (const json::Value v : known.root()) {
        if (v.key() != "sessions") {
            w.key(v.key()).value(v);
            continue;
        }
        w.key("sessions").beginArray();
        for (const json::Value o : v) {
            w.beginObject();
            for (const json::Value f : o)
                w.key(f.key()).value(f);
            for (const Local &l : local)
                if (l.id == o["id"].str()) {
                    if (l.starred && !o.has("starred")) {
                        w.key("starred").value(true);
                        changed = true;
                    }
                    if (l.muted && !o.has("muted")) {
                        w.key("muted").value(true);
                        changed = true;
                    }
                    if (l.notify != model::NotifyLevel::Default && !o.has("notify")) {
                        w.key("notify").value(int64_t(l.notify));
                        changed = true;
                    }
                }
            w.endObject();
        }
        w.endArray();
    }
    w.endObject();
    return changed && file::writeAtomic(knownPath, w.take(), 0600);
}

void importOldCaches(
    plat::App &app, const std::string &workspacesPath, const std::string &markerPath
) {
    // No earlier cache at all (most starts): nothing to read, not even the
    // list (this runs on every launch).
    const std::string data = identity::dataDir(app);
    if (data.empty() || !file::isDir(file::join(data, "cache")))
        return;
    // The keys only (auth::WorkspaceStore would read every credential).
    json::Document ws;
    if (workspacesPath.empty() || markerPath.empty() || !ws.parseFile(workspacesPath))
        return;
    std::vector<std::string> done = readLines(markerPath);
    const size_t             had  = done.size();
    for (const json::Value r : ws.root()["workspaces"]) {
        const std::string service(r["service"].str()), id(r["id"].str());
        const std::string key = service + ":" + id;
        if (service.empty() || id.empty() || std::find(done.begin(), done.end(), key) != done.end())
            continue;
        done.push_back(key);
        const std::string from = oldCacheDir(app, key);
        if (from.empty())
            continue;
        if (service == "slack")
            importSlackCache(app, from, cache::WorkspaceCache::dirFor(app, key));
        else if (service == "claude-code")
            importClaudeCodeCache(
                from, file::join(identity::dataDir(app), "claude-code/known-sessions.json")
            );
    }
    if (done.size() == had)
        return;
    std::string text;
    for (const std::string &k : done)
        text += k + "\n";
    if (!file::writeAtomic(markerPath, text, 0600))
        LOG_WARN("legacy", "can't write %s", markerPath.c_str());
}

} // namespace legacy
