#include "app/fake/fixture.h"
#include "app/model/image_size.h"

#include "base/file.h"
#include "base/mime.h"
#include "base/json.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace fake {

using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Ts;
using model::UserRef;

namespace {

using json::BoolField;
using json::owned;
using json::readBools;
using json::readStrings;
using json::StrField;

const StrField<model::User> kUserStrings[] = {
    {"id", &model::User::id},
    {"name", &model::User::name},
    {"displayName", &model::User::displayName},
    {"realName", &model::User::realName},
    {"profileName", &model::User::profileName},
    {"title", &model::User::title},
    {"email", &model::User::email},
};
const BoolField<model::User> kUserBools[] = {
    {"bot", &model::User::bot},
    {"active", &model::User::active},
    {"admin", &model::User::admin},
    {"owner", &model::User::owner},
    {"dnd", &model::User::dnd},
    {"vip", &model::User::vip},
};
const StrField<model::Conversation> kConvStrings[] = {
    {"id", &model::Conversation::id},
    {"name", &model::Conversation::name},
    {"topic", &model::Conversation::topic},
};
const BoolField<model::Conversation> kConvBools[] = {
    {"starred", &model::Conversation::starred},
    {"muted", &model::Conversation::muted},
};
const StrField<model::Attachment> kAttachmentStrings[] = {
    {"color", &model::Attachment::color},
    {"pretext", &model::Attachment::pretext},
    {"author", &model::Attachment::author},
    {"title", &model::Attachment::title},
    {"link", &model::Attachment::link},
    {"text", &model::Attachment::text},
    {"service", &model::Attachment::service},
};

// Magic first: extensions lie more often than headers; an ID3 tag (it can
// front other audio too) only when the extension says nothing.
std::string mimeFor(std::string_view name, std::string_view head) {
    const std::string_view magic = mime::sniff(head);
    if (!magic.empty() && magic != "audio/mpeg")
        return std::string(magic);
    if (const std::string_view byName = mime::fromName(name); !byName.empty())
        return std::string(byName);
    return std::string(magic.empty() ? std::string_view("application/octet-stream") : magic);
}

std::string prettyTypeFor(std::string_view type, std::string_view name) {
    if (const std::string_view l = mime::label(type); !l.empty())
        return std::string(l);
    const std::string suffix = str::asciiUpper(file::extension(name));
    return suffix.empty() ? "File" : suffix;
}

std::string assetPath(const std::string &dir, std::string_view rel) {
    if (rel.empty())
        return {};
    if (rel.find("://") != std::string_view::npos)
        return std::string(rel); // already a URL (remote asset — discouraged, but allowed)
    return file::resolve(dir, rel);
}

model::File fileFrom(json::Value o, const std::string &dir, int index) {
    model::File f = fileFromLocalPath(file::resolve(dir, o["path"].str()), index);
    if (o.has("name"))
        f.name = owned(o["name"]);
    if (o.has("mime"))
        f.mime = owned(o["mime"]);
    if (o.has("type"))
        f.prettyType = owned(o["type"]);
    if (o["width"].integer() > 0 && o["height"].integer() > 0) {
        f.width  = int32_t(o["width"].integer());
        f.height = int32_t(o["height"].integer());
    }
    // Audio: a voice clip ("subtype": "slack_audio") with its transcript, or
    // a plain upload with just a duration.
    f.subtype    = owned(o["subtype"]);
    f.durationMs = int64_t(o["durationMs"].number());
    f.transcript = owned(o["transcript"]);
    return f;
}

model::Attachment attachmentFrom(json::Value o, const std::string &dir) {
    model::Attachment a;
    readStrings(o, a, kAttachmentStrings);
    a.footer  = o.has("footer") ? owned(o["footer"]) : a.service;
    a.favicon = assetPath(dir, o["favicon"].str());
    if (const auto img = o["image"].str(); !img.empty()) {
        a.image   = assetPath(dir, img);
        int32_t w = 0, h = 0;
        model::imageSize(a.image, &w, &h);
        a.imageWidth  = int32_t(o["width"].integer(w));
        a.imageHeight = int32_t(o["height"].integer(h));
    }
    for (json::Value fv : o["fields"])
        a.fields.push_back(model::AttachmentField{owned(fv["title"]), owned(fv["value"])});
    a.linkPreview = o["linkPreview"].boolean(!a.link.empty() && a.fields.empty());
    return a;
}

bool tzOffsetSeconds(std::string_view spec, int32_t *out) {
    // "+02:00" / "-05:30" / "+0530" / "Z"
    *out = 0;
    if (spec.empty() || spec == "Z")
        return true;
    if (spec[0] != '+' && spec[0] != '-')
        return false;
    const int sign = spec[0] == '-' ? -1 : 1;
    size_t    i    = 1;
    int       h = 0, m = 0, digits = 0;
    while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9' && digits < 2)
        h = h * 10 + (spec[i++] - '0'), ++digits;
    if (!digits)
        return false;
    if (i < spec.size() && spec[i] == ':')
        ++i;
    if (i < spec.size()) {
        if (spec.size() - i != 2 || !(spec[i] >= '0' && spec[i] <= '9') ||
            !(spec[i + 1] >= '0' && spec[i + 1] <= '9'))
            return false;
        m = (spec[i] - '0') * 10 + (spec[i + 1] - '0');
    }
    *out = sign * (h * 3600 + m * 60);
    return true;
}

bool parseInt(std::string_view s, int64_t *out) {
    if (s.empty() || s.size() > 12)
        return false;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

} // namespace

bool parseTimeSpec(std::string_view spec, int64_t now, int64_t prev, int64_t *out) {
    const std::string_view s = str::trim(spec);
    if (s.empty())
        return false;
    // Relative: "-45m" (before now) / "+7m" (after prev). ^([+-])(\d+)([smhd])$
    if ((s[0] == '-' || s[0] == '+') && s.size() >= 3) {
        const char unit = s.back();
        int64_t    n    = 0;
        if ((unit == 's' || unit == 'm' || unit == 'h' || unit == 'd') &&
            parseInt(s.substr(1, s.size() - 2), &n)) {
            const int64_t mult = unit == 'm' ? 60 : unit == 'h' ? 3600 : unit == 'd' ? 86400 : 1;
            *out               = s[0] == '-' ? now - n * mult : (prev >= 0 ? prev : now) + n * mult;
            return true;
        }
    }
    // Wall clock: "[-Nd] HH:MM". ^(?:(-?\d+)d\s+)?(\d{1,2}):(\d{2})$
    int64_t          days  = 0;
    std::string_view clock = s;
    if (const size_t d = s.find('d'); d != std::string_view::npos) {
        std::string_view num = s.substr(0, d);
        const bool       neg = !num.empty() && num[0] == '-';
        if (neg)
            num.remove_prefix(1);
        if (!parseInt(num, &days) || d + 1 >= s.size() || (s[d + 1] != ' ' && s[d + 1] != '\t'))
            return false;
        if (neg)
            days = -days;
        clock = str::trim(s.substr(d + 1));
    }
    const size_t colon = clock.find(':');
    int64_t      h = 0, m = 0;
    if (colon == std::string_view::npos || colon == 0 || colon > 2 || clock.size() - colon != 3 ||
        !parseInt(clock.substr(0, colon), &h) || !parseInt(clock.substr(colon + 1), &m))
        return false;
    if (h > 23 || m > 59)
        return false; // "25:99" is no time of day
    const base::CivilTime today = base::localTime(now);
    *out = base::fromLocal(today.year, today.month, today.day + int(days), int(h), int(m));
    return true;
}

model::File fileFromLocalPath(const std::string &absPath, int index) {
    model::File f;
    char        id[16];
    std::snprintf(id, sizeof id, "FDEMO%04d", index);
    f.id   = id;
    f.name = std::string(file::baseName(absPath));
    std::string head;
    file::readRange(absPath, 0, 16, &head); // sniffing headers
    f.mime       = mimeFor(f.name, head);
    f.prettyType = prettyTypeFor(f.mime, f.name);
    f.path       = absPath;
    f.size       = std::max<int64_t>(0, file::size(absPath));
    if (f.mime.compare(0, 6, "image/") == 0)
        model::imageSize(absPath, &f.width, &f.height);
    return f;
}

// Not inlined: 20 error sites share it.
[[gnu::noinline]] bool failLoad(model::Store &store, std::string *error, std::string why) {
    store.clear();
    if (error)
        *error = std::move(why);
    return false;
}

bool loadFixture(
    std::string_view path, model::Store &store, Fixture *fx, std::string *error, int64_t now
) {
    store.clear();
    *fx       = Fixture{};
    auto fail = [&](std::string why) { return failLoad(store, error, std::move(why)); };

    std::string jsonPath = file::absolute(path);
    if (file::isDir(jsonPath))
        jsonPath = file::join(jsonPath, "fixture.json");
    json::Document doc;
    std::string    perr;
    if (!doc.parseFile(jsonPath, &perr))
        return fail(
            perr.empty() ? str::concat({"cannot open ", jsonPath})
                         : str::concat({file::baseName(jsonPath), ": ", perr})
        );
    const json::Value root = doc.root();
    if (!root.isObject())
        return fail(str::concat({file::baseName(jsonPath), ": not a JSON object"}));
    fx->dir = std::string(file::dirName(jsonPath));
    {
        const std::string gifs = owned(root["gifs"]);
        fx->gifDir             = file::resolve(fx->dir, gifs.empty() ? "assets/gifs" : gifs);
    }

    const json::Value ws = root["workspace"];
    store.workspaceId    = owned(ws["id"]);
    if (store.workspaceId.empty())
        store.workspaceId = "DEMO";
    store.workspaceName    = ws.has("name") ? owned(ws["name"]) : "Demo workspace";
    store.workspaceIcon    = assetPath(fx->dir, ws["icon"].str());
    store.workspaceUrl     = owned(ws["url"]); // optional: links fall back to slack.com
    const std::string meId = owned(root["me"]);
    if (meId.empty())
        return fail("\"me\" is required");

    // ── users ──
    for (json::Value o : root["users"]) {
        model::User u;
        readStrings(o, u, kUserStrings);
        readBools(o, u, kUserBools);
        u.avatar      = assetPath(fx->dir, o["avatar"].str());
        u.statusEmoji = owned(o["status"]["emoji"]);
        u.statusText  = owned(o["status"]["text"]);
        if (o.has("tz")) {
            if (!tzOffsetSeconds(o["tz"].str(), &u.tzOffset))
                return fail(str::concat({"user ", u.id, ": bad tz ", o["tz"].str()}));
            u.hasTz = true;
        }
        if (u.id.empty() || u.name.empty())
            return fail("every user needs an id and a name");
        store.addUser(std::move(u));
    }
    store.me = store.findUser(meId);
    if (store.me == kNoUser)
        return fail(str::concat({"\"me\" (", meId, ") is not in users"}));

    // ── conversations ──
    for (json::Value o : root["conversations"]) {
        const std::string   kind = o.has("kind") ? owned(o["kind"]) : "channel";
        model::Conversation c;
        readStrings(o, c, kConvStrings);
        readBools(o, c, kConvBools);
        if (c.id.empty())
            return fail("every conversation needs an id");
        if (kind == "channel")
            c.kind = model::ConvKind::Channel;
        else if (kind == "private")
            c.kind = model::ConvKind::Private;
        else if (kind == "dm")
            c.kind = model::ConvKind::Dm;
        else if (kind == "group")
            c.kind = model::ConvKind::Group;
        else
            return fail(str::concat({"conversation ", c.id, ": unknown kind ", kind}));
        c.member      = o["member"].boolean(true);
        c.memberCount = uint32_t(o["memberCount"].integer());
        c.unread      = uint32_t(o["unread"].integer());
        c.mentions    = uint32_t(o["mentions"].integer());
        for (json::Value m : o["members"])
            c.members.push_back(store.internUser(m.str()));
        if (c.kind == model::ConvKind::Dm) {
            const std::string peer = owned(o["user"]);
            c.dmUser               = store.findUser(peer);
            if (c.dmUser == kNoUser)
                return fail(str::concat({"dm ", c.id, ": unknown user ", peer}));
            if (c.name.empty())
                c.name = store.user(c.dmUser).name;
        }
        if (c.kind == model::ConvKind::Group) {
            if (std::find(c.members.begin(), c.members.end(), store.me) == c.members.end())
                c.members.push_back(store.me);
            if (c.name.empty()) {
                std::string names;
                for (UserRef m : c.members) {
                    if (!names.empty())
                        names += "--";
                    names += store.user(m).name;
                }
                c.name = str::concat({"mpdm-", names, "-1"});
            }
        }
        if (c.memberCount == 0 && !c.members.empty())
            c.memberCount = uint32_t(c.members.size());
        c.hasMoreBefore = false; // the fixture is the whole history
        std::string canvasHtml;
        if (o.has("canvas")) {
            const json::Value cv  = o["canvas"];
            const std::string rel = owned(cv["html"]);
            canvasHtml            = rel.empty() ? std::string() : file::resolve(fx->dir, rel);
            if (canvasHtml.empty() || !file::exists(canvasHtml) || file::isDir(canvasHtml))
                return fail(
                    str::concat({"conversation ", c.id, ": canvas needs a readable \"html\" file"})
                );
            c.canvasTitle = owned(cv["title"]);
            if (c.canvasTitle.empty())
                c.canvasTitle = "Canvas";
            c.canvasId = str::concat({"FCANVAS", c.id});
        }
        if (store.findConversation(c.id) != kNoConv)
            return fail(str::concat({"conversation ", c.id, " is listed twice"}));
        const ConvRef ref = store.addConversation(std::move(c));
        if (!canvasHtml.empty())
            fx->canvases.push_back(Canvas{ref, std::move(canvasHtml)});
    }

    // ── messages ──
    // Unique, order-preserving ts per conversation: equal times get the next
    // free microsecond.
    std::vector<std::unordered_set<Ts>> used(store.conversationCount());
    auto                                allocate = [&](ConvRef conv, int64_t secs) {
        Ts ts = secs * 1000000;
        while (!used[conv].insert(ts).second)
            ++ts;
        return ts;
    };
    std::vector<int64_t>                     prevRoot(store.conversationCount(), -1);
    std::vector<std::vector<model::Message>> history(store.conversationCount());
    std::vector<std::vector<model::Message>> threads; // each: one thread's replies
    std::vector<ConvRef>                     threadConv;
    int                                      fileIndex = 0;

    // Fills `out` from a message object; "" on success, else the reason.
    auto parseMessage = [&](json::Value        o,
                            ConvRef            conv,
                            int64_t            prev,
                            const std::string &where,
                            model::Message    &out,
                            int64_t           &when) -> std::string {
        const std::string timeSpec = owned(o["time"]);
        if (!parseTimeSpec(timeSpec, now, prev, &when))
            return str::concat({where, ": bad time ", timeSpec});
        out.ts                = allocate(conv, when);
        const json::Value bot = o["bot"];
        if (bot.isObject() && bot.size() > 0) {
            auto &x     = out.extras();
            x.subtype   = "bot_message";
            x.botName   = owned(bot["name"]);
            x.botAvatar = assetPath(fx->dir, bot["avatar"].str());
        }
        const std::string user = owned(o["user"]);
        if (!user.empty()) {
            out.user = store.findUser(user);
            if (out.user == kNoUser)
                return str::concat({where, ": unknown user ", user});
        } else if (!out.extra) {
            return str::concat({where, ": needs a user or a bot"});
        }
        if (o.has("subtype"))
            out.extras().subtype = owned(o["subtype"]);
        out.text   = owned(o["text"]);
        out.edited = o["edited"].boolean();
        out.pinned = o["pinned"].boolean();
        for (json::Value rv : o["reactions"]) {
            model::Reaction r;
            r.name = owned(rv["name"]);
            for (json::Value u : rv["users"])
                r.users.push_back(store.internUser(u.str()));
            r.count = uint32_t(r.users.size());
            if (!r.name.empty() && r.count > 0)
                out.reactions.push_back(std::move(r));
        }
        for (json::Value fv : o["files"])
            out.extras().files.push_back(fileFrom(fv, fx->dir, ++fileIndex));
        for (json::Value av : o["attachments"])
            out.extras().attachments.push_back(attachmentFrom(av, fx->dir));
        return {};
    };

    int index = 0;
    for (json::Value o : root["messages"]) {
        ++index;
        const std::string convId = owned(o["conv"]);
        const ConvRef     conv   = store.findConversation(convId);
        if (conv == kNoConv)
            return fail(
                str::concat({"message ", str::number(index), ": unknown conversation ", convId})
            );
        const std::string where = str::concat({"message ", str::number(index), " in ", convId});
        model::Message    msg;
        int64_t           when = 0;
        if (std::string err = parseMessage(o, conv, prevRoot[conv], where, msg, when); !err.empty())
            return fail(err);
        prevRoot[conv] = when;

        if (o["replies"].size() > 0) {
            int64_t                     prev = when;
            std::vector<model::Message> thread;
            int                         ri = 0;
            for (json::Value rv : o["replies"]) {
                ++ri;
                model::Message    reply;
                int64_t           rwhen = 0;
                const std::string rwhere =
                    str::concat({"message ", str::number(index), ", reply ", str::number(ri)});
                if (std::string err = parseMessage(rv, conv, prev, rwhere, reply, rwhen);
                    !err.empty())
                    return fail(err);
                if (rwhen < when)
                    return fail(str::concat({rwhere, " is dated before its root"}));
                prev           = rwhen;
                reply.threadTs = msg.ts;
                if (reply.user != kNoUser && msg.replyUsers.size() < 5 &&
                    std::find(msg.replyUsers.begin(), msg.replyUsers.end(), reply.user) ==
                        msg.replyUsers.end())
                    msg.replyUsers.push_back(reply.user);
                thread.push_back(std::move(reply));
            }
            msg.threadTs    = msg.ts; // a root carries its own ts, as in Slack
            msg.replyCount  = uint32_t(thread.size());
            msg.latestReply = thread.back().ts;
            threads.push_back(std::move(thread));
            threadConv.push_back(conv);
        }
        history[conv].push_back(std::move(msg));
    }

    // Into the store, oldest first, then the read cursors: `unread: N` marks
    // the last N top-level messages unread.
    for (ConvRef c = 0; c < history.size(); ++c) {
        const uint32_t unread   = store.conversation(c).unread,
                       mentions = store.conversation(c).mentions;
        store.addPage(c, std::move(history[c]));
        auto        &conv = store.conversation(c);
        const size_t n    = conv.messages.size();
        if (unread > 0 && unread < n)
            conv.lastRead = conv.messages[n - unread - 1].ts;
        else if (unread > 0)
            conv.lastRead = 0;
        else
            conv.lastRead = n ? conv.messages.back().ts : 0;
        conv.unread   = unread; // the fixture's numbers, not a recount
        conv.mentions = mentions;
    }
    for (size_t t = 0; t < threads.size(); ++t)
        store.addPage(threadConv[t], std::move(threads[t]));

    // ── auto replies ──
    for (json::Value o : root["autoReplies"]) {
        AutoReply r;
        r.conv     = store.findConversation(o["conv"].str());
        r.user     = store.findUser(o["user"].str());
        r.text     = owned(o["text"]);
        r.afterMs  = int(o["afterMs"].integer(r.afterMs));
        r.typingMs = int(o["typingMs"].integer(r.typingMs));
        r.inThread = o["inThread"].boolean();
        if (r.conv == kNoConv || r.user == kNoUser || r.text.empty())
            return fail("autoReplies: each needs a known conv, user and a text");
        fx->autoReplies.push_back(std::move(r));
    }

    // ── unfurls / ai ──
    for (json::Value o : root["unfurls"]) {
        Unfurl u;
        u.url = owned(o["url"]);
        if (u.url.empty())
            return fail("unfurls: each needs a url");
        u.attachment             = attachmentFrom(o, fx->dir);
        u.attachment.linkPreview = true;
        if (u.attachment.link.empty())
            u.attachment.link = u.url;
        fx->unfurls.push_back(std::move(u));
    }
    const json::Value ai = root["ai"];
    fx->aiDefault        = owned(ai["default"]);
    for (json::Value o : ai["replies"]) {
        AiReply r{owned(o["match"]), owned(o["text"])};
        if (r.match.empty() || r.text.empty())
            return fail("ai.replies: each needs match and text");
        fx->aiReplies.push_back(std::move(r));
    }

    const std::string start = owned(root["startConversation"]);
    if (!start.empty()) {
        fx->startConversation = store.findConversation(start);
        if (fx->startConversation == kNoConv)
            return fail(str::concat({"startConversation ", start, " is not a conversation"}));
    } else if (store.conversationCount()) {
        fx->startConversation = 0;
    }
    return true;
}

} // namespace fake
