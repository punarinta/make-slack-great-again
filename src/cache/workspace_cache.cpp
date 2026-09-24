// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "workspace_cache.h"
#include "cache_evictor.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

// ── JSON serialization helpers ────────────────────────────────────────────────

static QJsonObject toJson(const TextEntity &e) {
    QJsonObject o;
    o["t"] = static_cast<int>(e.type);
    o["o"] = e.offset;
    o["l"] = e.length;
    if (!e.data.isEmpty())
        o["d"] = e.data;
    return o;
}
static TextEntity entityFromJson(const QJsonObject &o) {
    TextEntity e;
    e.type   = static_cast<EntityType>(o["t"].toInt());
    e.offset = o["o"].toInt();
    e.length = o["l"].toInt();
    e.data   = o["d"].toString();
    return e;
}

static QJsonObject toJson(const TextWithEntities &t) {
    QJsonObject o;
    o["x"] = t.text;
    if (!t.entities.empty()) {
        QJsonArray arr;
        for (const auto &e : t.entities)
            arr.append(toJson(e));
        o["e"] = arr;
    }
    return o;
}
static TextWithEntities tweFromJson(const QJsonObject &o) {
    TextWithEntities t;
    t.text = o["x"].toString();
    for (const auto &v : o["e"].toArray())
        t.entities.push_back(entityFromJson(v.toObject()));
    return t;
}

static QJsonObject toJson(const Reaction &r) {
    QJsonObject o;
    o["n"] = r.name;
    o["c"] = r.count;
    QJsonArray users;
    for (const auto &u : r.users)
        users.append(u.value);
    o["u"] = users;
    return o;
}
static Reaction reactionFromJson(const QJsonObject &o) {
    Reaction r;
    r.name  = o["n"].toString();
    r.count = o["c"].toInt();
    for (const auto &v : o["u"].toArray())
        r.users.push_back(UserId{v.toString()});
    return r;
}

static QJsonObject toJson(const File &f) {
    QJsonObject o;
    o["id"] = f.id;
    o["na"] = f.name;
    o["mi"] = f.mimeType;
    o["pt"] = f.prettyType;
    o["up"] = f.urlPrivate;
    o["pl"] = f.permalink;
    o["th"] = f.thumbUrl;
    o["iw"] = f.imageWidth;
    o["ih"] = f.imageHeight;
    o["sz"] = static_cast<double>(f.size);
    if (!f.urlPrivateDownload.isEmpty())
        o["ud"] = f.urlPrivateDownload;
    if (f.durationMs > 0)
        o["dm"] = static_cast<double>(f.durationMs);
    if (!f.aacUrl.isEmpty())
        o["aac"] = f.aacUrl;
    if (!f.subtype.isEmpty())
        o["st"] = f.subtype;
    if (!f.fileType.isEmpty())
        o["ft"] = f.fileType;
    if (!f.transcriptStatus.isEmpty())
        o["tst"] = f.transcriptStatus;
    if (!f.transcriptPreview.isEmpty())
        o["tpv"] = f.transcriptPreview;
    if (!f.transcriptVttUrl.isEmpty())
        o["tvt"] = f.transcriptVttUrl;
    if (!f.title.isEmpty())
        o["ti"] = f.title;
    if (!f.thumbs.empty()) {
        QJsonArray arr;
        for (const auto &t : f.thumbs)
            arr.append(QJsonObject{{"w", t.width}, {"h", t.height}, {"u", t.url}});
        o["tb"] = arr;
    }
    if (!f.animThumbs.empty()) {
        QJsonArray arr;
        for (const auto &t : f.animThumbs)
            arr.append(QJsonObject{{"w", t.width}, {"h", t.height}, {"u", t.url}});
        o["ta"] = arr;
    }
    return o;
}
static File fileFromJson(const QJsonObject &o) {
    File f;
    f.id                 = o["id"].toString();
    f.name               = o["na"].toString();
    f.mimeType           = o["mi"].toString();
    f.prettyType         = o["pt"].toString();
    f.urlPrivate         = o["up"].toString();
    f.permalink          = o["pl"].toString();
    f.thumbUrl           = o["th"].toString();
    f.imageWidth         = o["iw"].toInt();
    f.imageHeight        = o["ih"].toInt();
    f.size               = static_cast<qint64>(o["sz"].toDouble());
    f.urlPrivateDownload = o["ud"].toString();
    f.durationMs         = static_cast<qint64>(o["dm"].toDouble());
    f.aacUrl             = o["aac"].toString();
    f.subtype            = o["st"].toString();
    f.fileType           = o["ft"].toString();
    f.transcriptStatus   = o["tst"].toString();
    f.transcriptPreview  = o["tpv"].toString();
    f.transcriptVttUrl   = o["tvt"].toString();
    f.title              = o["ti"].toString();
    for (const auto &v : o["tb"].toArray()) {
        const auto t = v.toObject();
        f.thumbs.push_back(FileThumb{t["w"].toInt(), t["h"].toInt(), t["u"].toString()});
    }
    for (const auto &v : o["ta"].toArray()) {
        const auto t = v.toObject();
        f.animThumbs.push_back(FileThumb{t["w"].toInt(), t["h"].toInt(), t["u"].toString()});
    }
    return f;
}

static QJsonArray buttonsToJson(const std::vector<BotButton> &buttons) {
    QJsonArray arr;
    for (const auto &btn : buttons)
        arr.append(QJsonObject{{"t", btn.text}, {"u", btn.url}, {"s", btn.style}});
    return arr;
}
static std::vector<BotButton> buttonsFromJson(const QJsonArray &arr) {
    std::vector<BotButton> buttons;
    for (const auto &v : arr) {
        const auto o = v.toObject();
        buttons.push_back(BotButton{o["t"].toString(), o["u"].toString(), o["s"].toString()});
    }
    return buttons;
}

static QJsonObject toJson(const Block &b) {
    QJsonObject o;
    o["ty"] = b.typeStr;
    o["tx"] = toJson(b.text);
    if (!b.imageUrl.isEmpty())
        o["iu"] = b.imageUrl;
    if (!b.altText.isEmpty())
        o["at"] = b.altText;
    if (!b.buttons.empty())
        o["bt"] = buttonsToJson(b.buttons);
    if (!b.tableRows.empty()) {
        QJsonArray rows;
        for (const auto &row : b.tableRows) {
            QJsonArray cells;
            for (const auto &cell : row)
                cells.append(toJson(cell));
            rows.append(cells);
        }
        o["tr"] = rows;
    }
    return o;
}
static Block blockFromJson(const QJsonObject &o) {
    Block b;
    b.typeStr  = o["ty"].toString();
    b.text     = tweFromJson(o["tx"].toObject());
    b.imageUrl = o["iu"].toString();
    b.altText  = o["at"].toString();
    b.buttons  = buttonsFromJson(o["bt"].toArray());
    for (const auto &rv : o["tr"].toArray()) {
        std::vector<TextWithEntities> row;
        for (const auto &cv : rv.toArray())
            row.push_back(tweFromJson(cv.toObject()));
        b.tableRows.push_back(std::move(row));
    }
    return b;
}

static QJsonObject toJson(const Attachment &a) {
    QJsonObject o;
    if (a.id > 0)
        o["id"] = a.id; // positional id, what chat.deleteAttachment addresses
    o["fb"] = a.fallback;
    o["co"] = a.color;
    o["pt"] = a.pretext;
    o["an"] = a.authorName;
    o["ti"] = a.title;
    o["tl"] = a.titleLink;
    o["tx"] = toJson(a.text);
    o["iu"] = a.imageUrl;
    o["tu"] = a.thumbUrl;
    o["fo"] = a.footer;
    if (!a.footerIcon.isEmpty())
        o["fc"] = a.footerIcon;
    if (a.msgDate > 0)
        o["md"] = QString::number(a.msgDate); // epoch micros; string-encoded like Message::date
    o["lp"]  = a.isLinkPreview;
    o["lpv"] = 2; // app unfurls now count as previews too
    if (a.imageWidth > 0)
        o["iw"] = a.imageWidth;
    if (a.imageHeight > 0)
        o["ih"] = a.imageHeight;
    if (a.thumbWidth > 0)
        o["tw"] = a.thumbWidth;
    if (a.thumbHeight > 0)
        o["tg"] = a.thumbHeight;
    if (!a.blocks.empty()) {
        QJsonArray arr;
        for (const auto &b : a.blocks)
            arr.append(toJson(b));
        o["bl"] = arr;
    }
    if (!a.buttons.empty())
        o["bt"] = buttonsToJson(a.buttons);
    // Classic bot "fields" rows. Jenkins-style bots put their whole body here
    // (text empty, fallback = the same string): dropping them demoted every
    // cached copy to the fallback rendering.
    if (!a.fields.empty()) {
        QJsonArray arr;
        for (const auto &f : a.fields) {
            QJsonObject fo;
            fo["t"] = f.title;
            fo["v"] = toJson(f.value);
            arr.append(fo);
        }
        o["fd"] = arr;
    }
    if (a.isMsgUnfurl) {
        o["mu"] = true;
        o["ai"] = a.authorIcon;
        o["as"] = a.authorSubname;
        o["ci"] = a.channelId;
        if (!a.files.empty()) {
            QJsonArray arr;
            for (const auto &f : a.files)
                arr.append(toJson(f));
            o["fi"] = arr;
        }
    }
    return o;
}
static Attachment attachmentFromJson(const QJsonObject &o) {
    Attachment a;
    a.id            = o["id"].toInt();
    a.fallback      = o["fb"].toString();
    a.color         = o["co"].toString();
    a.pretext       = o["pt"].toString();
    a.authorName    = o["an"].toString();
    a.title         = o["ti"].toString();
    a.titleLink     = o["tl"].toString();
    a.text          = tweFromJson(o["tx"].toObject());
    a.imageUrl      = o["iu"].toString();
    a.thumbUrl      = o["tu"].toString();
    a.footer        = o["fo"].toString();
    a.footerIcon    = o["fc"].toString();
    a.msgDate       = o["md"].toString().toLongLong();
    a.imageWidth    = o["iw"].toInt();
    a.imageHeight   = o["ih"].toInt();
    a.thumbWidth    = o["tw"].toInt();
    a.thumbHeight   = o["tg"].toInt();
    a.isLinkPreview = o["lp"].toBool();
    for (const auto &v : o["bl"].toArray())
        a.blocks.push_back(blockFromJson(v.toObject()));
    a.buttons = buttonsFromJson(o["bt"].toArray());
    for (const auto &v : o["fd"].toArray()) {
        const auto fo = v.toObject();
        a.fields.push_back(
            AttachmentField{
                .title = fo["t"].toString(),
                .value = tweFromJson(fo["v"].toObject()),
            }
        );
    }
    a.isMsgUnfurl = o["mu"].toBool();
    if (a.isMsgUnfurl) {
        a.authorIcon    = o["ai"].toString();
        a.authorSubname = o["as"].toString();
        a.channelId     = o["ci"].toString();
        for (const auto &v : o["fi"].toArray())
            a.files.push_back(fileFromJson(v.toObject()));
    }
    return a;
}

static QJsonObject toJson(const Message &m) {
    QJsonObject o;
    o["ts"] = m.ts;
    o["da"] = QString::number(m.date); // epoch micros; string-encoded to avoid JSON double loss
    if (m.threadRoot)
        o["tr"] = *m.threadRoot;
    o["au"] = m.author.value;
    if (!m.botName.isEmpty())
        o["bn"] = m.botName;
    if (!m.botAvatarUrl.isEmpty())
        o["ba"] = m.botAvatarUrl;
    o["tx"] = toJson(m.text);
    o["ed"] = m.edited;
    if (m.subtype)
        o["st"] = *m.subtype;
    if (!m.reactions.empty()) {
        QJsonArray arr;
        for (const auto &r : m.reactions)
            arr.append(toJson(r));
        o["re"] = arr;
    }
    if (!m.files.empty()) {
        QJsonArray arr;
        for (const auto &f : m.files)
            arr.append(toJson(f));
        o["fi"] = arr;
    }
    if (!m.blocks.empty()) {
        QJsonArray arr;
        for (const auto &b : m.blocks)
            arr.append(toJson(b));
        o["bl"] = arr;
    }
    if (!m.attachments.empty()) {
        QJsonArray arr;
        for (const auto &a : m.attachments)
            arr.append(toJson(a));
        o["at"] = arr;
    }
    if (m.huddle) {
        QJsonArray who;
        for (const auto &u : m.huddle->attendees)
            who.append(u.value);
        o["hu"] = QJsonObject{
            {"a", who},
            {"s", QString::number(m.huddle->startSec)},
            {"e", QString::number(m.huddle->endSec)},
            {"x", m.huddle->ended},
        };
    }
    return o;
}
// Host (minus "www.") plus path (minus trailing '/'), lower-cased host; the
// part of a URL that survives Slack's unfurl canonicalisation.
static QString urlResourceKey(const QString &url) {
    const QUrl u(url);
    if (!u.isValid() || u.host().isEmpty())
        return url;
    QString host = u.host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    QString path = u.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return host + path;
}
static Message messageFromJson(const QJsonObject &o) {
    Message m;
    m.ts   = o["ts"].toString();
    // Legacy caches predate the field — backfill from the stored ts so old and
    // new entries agree to the microsecond (no cache-version bump needed).
    m.date = o.contains("da") ? o["da"].toString().toLongLong() : decimalTsToMicros(m.ts);
    if (o.contains("tr"))
        m.threadRoot = o["tr"].toString();
    m.author       = UserId{o["au"].toString()};
    m.botName      = o["bn"].toString();
    m.botAvatarUrl = o["ba"].toString();
    m.text         = tweFromJson(o["tx"].toObject());
    m.edited       = o["ed"].toBool();
    if (o.contains("st"))
        m.subtype = o["st"].toString();
    for (const auto &v : o["re"].toArray())
        m.reactions.push_back(reactionFromJson(v.toObject()));
    for (const auto &v : o["fi"].toArray())
        m.files.push_back(fileFromJson(v.toObject()));
    for (const auto &v : o["bl"].toArray())
        m.blocks.push_back(blockFromJson(v.toObject()));
    for (const auto &v : o["at"].toArray()) {
        const auto obj = v.toObject();
        auto       att = attachmentFromJson(obj);
        // Older caches discarded Slack's unfurl metadata. Only infer a preview
        // when its target is also a link in the message body; a bot attachment
        // with a linked title alone is not enough.
        if (obj.value("lpv").toInt() < 2 && !att.isLinkPreview && !att.isMsgUnfurl &&
            m.botName.isEmpty() && (!m.subtype || *m.subtype != QLatin1String("bot_message"))) {
            // Slack reports the unfurl's canonical URL (redirects resolved,
            // tracking params dropped), so compare host+path, not the string.
            const auto linksTo = [&](const QString &url) {
                if (url.isEmpty())
                    return false;
                const QString key = urlResourceKey(url);
                return std::any_of(
                    m.text.entities.begin(), m.text.entities.end(), [&](const TextEntity &e) {
                        return e.type == EntityType::Link && urlResourceKey(e.data) == key;
                    }
                );
            };
            att.isLinkPreview = linksTo(att.titleLink) || linksTo(att.imageUrl);
        }
        m.attachments.push_back(std::move(att));
    }
    if (const auto h = o["hu"].toObject(); !h.isEmpty()) {
        HuddleInfo info;
        for (const auto &v : h["a"].toArray())
            info.attendees.push_back(UserId{v.toString()});
        info.startSec = h["s"].toString().toLongLong();
        info.endSec   = h["e"].toString().toLongLong();
        info.ended    = h["x"].toBool();
        m.huddle      = std::move(info);
    }
    // Re-derive the synthesized huddle label on every load — it must follow
    // the current locale, and rows cached before the transform existed have
    // empty text.
    presentHuddleThread(m);
    return m;
}

static QJsonObject toJson(const User &u) {
    QJsonObject o;
    o["id"] = u.id.value;
    o["na"] = u.name;
    o["dn"] = u.displayName;
    o["av"] = u.avatarUrl;
    o["bo"] = u.isBot;
    o["ex"] = u.isExternal;
    o["ac"] = u.isActive;
    o["de"] = u.isDeactivated;
    o["ad"] = u.isAdmin;
    o["ow"] = u.isOwner;
    o["se"] = u.statusEmoji;
    o["st"] = u.statusText;
    o["ti"] = u.title;
    if (!u.email.isEmpty())
        o["em"] = u.email;
    if (u.hasTz)
        o["tz"] = u.tzOffset;
    return o;
}
static User userFromJson(const QJsonObject &o) {
    User u;
    u.id            = UserId{o["id"].toString()};
    u.name          = o["na"].toString();
    u.displayName   = o["dn"].toString();
    u.avatarUrl     = o["av"].toString();
    u.isBot         = o["bo"].toBool();
    u.isExternal    = o["ex"].toBool();
    u.isActive      = o["ac"].toBool();
    u.isDeactivated = o["de"].toBool();
    u.isAdmin       = o["ad"].toBool();
    u.isOwner       = o["ow"].toBool();
    u.statusEmoji   = o["se"].toString();
    u.statusText    = o["st"].toString();
    u.title         = o["ti"].toString();
    u.email         = o["em"].toString();
    u.hasTz         = o.contains("tz");
    u.tzOffset      = o["tz"].toInt();
    return u;
}

static QJsonObject toJson(const Conversation &c) {
    QJsonObject o;
    o["id"] = c.id.value;
    o["ki"] = static_cast<int>(c.kind);
    o["na"] = c.name;
    o["mb"] = c.isMember;
    o["lr"] = c.lastRead;
    if (!c.latestTs.isEmpty())
        o["lt"] = c.latestTs;
    o["un"] = c.unread;
    if (c.mentionCount > 0)
        o["mc"] = c.mentionCount;
    if (c.dmUser)
        o["dm"] = c.dmUser->value;
    if (c.isMuted)
        o["mu"] = true;
    if (c.isStarred)
        o["st"] = true;
    if (c.locallyMuted)
        o["lm"] = true;
    if (!c.localName.isEmpty())
        o["ln"] = c.localName;
    if (c.notifLevel != NotificationLevel::Default)
        o["nl"] = static_cast<int>(c.notifLevel);
    return o;
}
static Conversation convFromJson(const QJsonObject &o) {
    Conversation c;
    c.id           = ConversationId{o["id"].toString()};
    c.kind         = static_cast<ConvKind>(o["ki"].toInt());
    c.name         = o["na"].toString();
    c.isMember     = o["mb"].toBool();
    c.lastRead     = o["lr"].toString();
    c.latestTs     = o["lt"].toString();
    c.unread       = o["un"].toInt();
    c.mentionCount = o["mc"].toInt();
    if (o.contains("dm"))
        c.dmUser = UserId{o["dm"].toString()};
    if (o.contains("mu"))
        c.isMuted = o["mu"].toBool();
    if (o.contains("st"))
        c.isStarred = o["st"].toBool();
    if (o.contains("lm"))
        c.locallyMuted = o["lm"].toBool();
    c.localName = o["ln"].toString();
    if (o.contains("nl"))
        c.notifLevel = static_cast<NotificationLevel>(o["nl"].toInt());
    return c;
}

// ── WorkspaceCache ────────────────────────────────────────────────────────────

WorkspaceCache::WorkspaceCache(const QString &handle) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // `handle` is the WorkspaceKey form ("slack:T0123"). The ':' is illegal in a
    // path component on Windows, so sanitize it into a safe directory name.
    QString       safe = handle;
    safe.replace(QLatin1Char(':'), QLatin1Char('_'));
    _dir            = base + "/cache/" + safe;
    // One-time migration: pre-multi-service caches were keyed by the bare id
    // (the part after the service prefix). Rename it forward so an existing
    // offline cache survives the upgrade instead of being silently rebuilt.
    const int colon = handle.indexOf(QLatin1Char(':'));
    if (colon > 0 && !QDir(_dir).exists()) {
        const QString legacy = base + "/cache/" + handle.mid(colon + 1);
        if (QDir(legacy).exists())
            QDir().rename(legacy, _dir);
    }
    QDir().mkpath(_dir + "/messages");
    QDir().mkpath(_dir + "/images");
}

QString WorkspaceCache::convPath() const {
    return _dir + "/conversations.json";
}
QString WorkspaceCache::usersPath() const {
    return _dir + "/users.json";
}
QString WorkspaceCache::botsPath() const {
    return _dir + "/bots.json";
}
QString WorkspaceCache::usergroupsPath() const {
    return _dir + "/usergroups.json";
}
QString WorkspaceCache::emojiPath() const {
    return _dir + "/emoji.json";
}
QString WorkspaceCache::msgsPath(const ConversationId &conv) const {
    return _dir + "/messages/" + conv.value + ".json";
}
QString WorkspaceCache::metaPath() const {
    return _dir + "/meta.json";
}
QString WorkspaceCache::imgPath(const QString &url) const {
    const auto hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex();
    return _dir + "/images/" + hash;
}

QByteArray WorkspaceCache::readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool WorkspaceCache::writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(data);
    return true;
}

bool WorkspaceCache::writeJson(const QString &path, const QJsonDocument &doc) {
    return writeFile(path, doc.toJson(QJsonDocument::Compact));
}

void WorkspaceCache::saveConversations(const std::vector<Conversation> &convs) {
    QJsonArray arr;
    for (const auto &c : convs)
        arr.append(toJson(c));
    writeJson(convPath(), QJsonDocument(arr));
}

std::vector<Conversation> WorkspaceCache::loadConversations() const {
    const auto data = readFile(convPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<Conversation> result;
    for (const auto &v : doc.array())
        result.push_back(convFromJson(v.toObject()));
    return result;
}

void WorkspaceCache::saveUsers(const std::vector<User> &users) {
    QJsonArray arr;
    for (const auto &u : users)
        arr.append(toJson(u));
    writeJson(usersPath(), QJsonDocument(arr));
}

std::vector<User> WorkspaceCache::loadUsers() const {
    const auto data = readFile(usersPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<User> result;
    for (const auto &v : doc.array())
        result.push_back(userFromJson(v.toObject()));
    return result;
}

void WorkspaceCache::saveBots(const QHash<QString, User> &bots) {
    QJsonArray arr;
    for (const auto &u : bots)
        arr.append(toJson(u));
    writeJson(botsPath(), QJsonDocument(arr));
}

void WorkspaceCache::saveUsergroups(const std::vector<Usergroup> &groups) {
    QJsonArray arr;
    for (const auto &g : groups) {
        QJsonArray users;
        for (const auto &u : g.users)
            users.append(u.value);
        arr.append(QJsonObject{{"id", g.id}, {"ha", g.handle}, {"na", g.name}, {"us", users}});
    }
    writeJson(usergroupsPath(), QJsonDocument(arr));
}

std::vector<Usergroup> WorkspaceCache::loadUsergroups() const {
    const auto data = readFile(usergroupsPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<Usergroup> result;
    for (const auto &v : doc.array()) {
        const auto o = v.toObject();
        Usergroup  g;
        g.id     = o["id"].toString();
        g.handle = o["ha"].toString();
        g.name   = o["na"].toString();
        for (const auto &u : o["us"].toArray())
            g.users.push_back(UserId{u.toString()});
        if (!g.id.isEmpty())
            result.push_back(std::move(g));
    }
    return result;
}

QHash<QString, User> WorkspaceCache::loadBots() const {
    const auto data = readFile(botsPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    QHash<QString, User> result;
    for (const auto &v : doc.array()) {
        auto u = userFromJson(v.toObject());
        if (!u.id.value.isEmpty())
            result[u.id.value] = std::move(u);
    }
    return result;
}

void WorkspaceCache::saveMessages(const ConversationId &conv, const std::vector<Message> &msgs) {
    const int  total = static_cast<int>(msgs.size());
    const int  start = std::max(0, total - kMaxMessages);
    QJsonArray arr;
    for (int i = start; i < total; ++i)
        arr.append(toJson(msgs[i]));
    writeJson(msgsPath(conv), QJsonDocument(arr));
}

std::vector<Message> WorkspaceCache::loadMessages(const ConversationId &conv) const {
    const auto data = readFile(msgsPath(conv));
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<Message> result;
    for (const auto &v : doc.array())
        result.push_back(messageFromJson(v.toObject()));
    return result;
}

QJsonObject &WorkspaceCache::metaObject() const {
    if (!_meta)
        _meta = QJsonDocument::fromJson(readFile(metaPath())).object();
    return *_meta;
}

void WorkspaceCache::writeMeta() {
    writeJson(metaPath(), QJsonDocument(metaObject()));
}

void WorkspaceCache::saveLastConv(const ConversationId &conv, const QString &displayName) {
    // meta.json also carries other keys (activity sweep stamp etc.) — they
    // survive because the cached object is mutated in place, no re-read needed.
    auto &o   = metaObject();
    o["conv"] = conv.value;
    o["name"] = displayName;
    writeMeta();
}

std::pair<ConversationId, QString> WorkspaceCache::loadLastConv() const {
    const auto &o = metaObject();
    if (o.isEmpty())
        return {};
    return {ConversationId{o.value("conv").toString()}, o.value("name").toString()};
}

void WorkspaceCache::saveMeUserId(const UserId &id) {
    metaObject()["meId"] = id.value;
    writeMeta();
}

UserId WorkspaceCache::loadMeUserId() const {
    return UserId{metaObject().value("meId").toString()};
}

void WorkspaceCache::saveActivitySweepAt(qint64 unixSecs) {
    metaObject()["sweepAt"] = unixSecs;
    writeMeta();
}

qint64 WorkspaceCache::loadActivitySweepAt() const {
    return metaObject().value("sweepAt").toVariant().toLongLong();
}

void WorkspaceCache::saveMutedThreads(const QStringList &keys) {
    metaObject()["mutedThreads"] = QJsonArray::fromStringList(keys);
    writeMeta();
}

QStringList WorkspaceCache::loadMutedThreads() const {
    QStringList out;
    for (const auto &v : metaObject().value("mutedThreads").toArray())
        out.append(v.toString());
    return out;
}

void WorkspaceCache::saveFollowedThreads(const QStringList &keys) {
    metaObject()["followedThreads"] = QJsonArray::fromStringList(keys);
    writeMeta();
}

QStringList WorkspaceCache::loadFollowedThreads() const {
    QStringList out;
    for (const auto &v : metaObject().value("followedThreads").toArray())
        out.append(v.toString());
    return out;
}

void WorkspaceCache::saveAiTranscripts(const QHash<QString, AiTranscript> &byFileId) {
    QJsonObject o;
    for (auto it = byFileId.constBegin(); it != byFileId.constEnd(); ++it)
        o[it.key()] = QJsonObject{{"text", it.value().text}, {"by", it.value().provider}};
    metaObject()["aiTranscripts"] = o;
    writeMeta();
}

QHash<QString, AiTranscript> WorkspaceCache::loadAiTranscripts() const {
    QHash<QString, AiTranscript> out;
    const QJsonObject            o = metaObject().value("aiTranscripts").toObject();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const QJsonObject e = it.value().toObject();
        AiTranscript      t{e.value("text").toString(), e.value("by").toString()};
        if (!t.text.isEmpty())
            out.insert(it.key(), t);
    }
    return out;
}

void WorkspaceCache::saveReminders(const std::vector<MessageReminder> &reminders) {
    QJsonArray arr;
    for (const auto &r : reminders) {
        QJsonObject o;
        o["conv"] = r.conv.value;
        o["ts"]   = r.ts;
        o["due"]  = r.dueAt;
        if (r.savedAt > 0)
            o["saved"] = r.savedAt;
        if (!r.threadRoot.isEmpty())
            o["root"] = r.threadRoot;
        if (!r.snippet.isEmpty())
            o["snippet"] = r.snippet;
        if (!r.author.value.isEmpty())
            o["author"] = r.author.value;
        if (!r.botName.isEmpty())
            o["botName"] = r.botName;
        if (!r.botAvatarUrl.isEmpty())
            o["botAvatar"] = r.botAvatarUrl;
        if (r.fired)
            o["fired"] = true;
        arr.append(o);
    }
    metaObject()["reminders"] = arr;
    writeMeta();
}

std::vector<MessageReminder> WorkspaceCache::loadReminders() const {
    std::vector<MessageReminder> out;
    const auto                   arr = metaObject().value("reminders").toArray();
    out.reserve(arr.size());
    for (const auto &v : arr) {
        const auto      o = v.toObject();
        MessageReminder r;
        r.conv         = ConversationId{o.value("conv").toString()};
        r.ts           = o.value("ts").toString();
        r.dueAt        = o.value("due").toVariant().toLongLong();
        r.savedAt      = o.value("saved").toVariant().toLongLong();
        r.threadRoot   = o.value("root").toString();
        r.snippet      = o.value("snippet").toString();
        r.author       = UserId{o.value("author").toString()};
        r.botName      = o.value("botName").toString();
        r.botAvatarUrl = o.value("botAvatar").toString();
        r.fired        = o.value("fired").toBool();
        if (!r.conv.value.isEmpty() && !r.ts.isEmpty())
            out.push_back(std::move(r));
    }
    return out;
}

void WorkspaceCache::saveReminderPreviews(const QHash<QString, ReminderPreview> &previews) {
    QJsonArray arr;
    for (auto it = previews.constBegin(); it != previews.constEnd(); ++it) {
        const auto &p = it.value();
        if (p.isEmpty())
            continue;
        QJsonObject o;
        o["key"] = it.key();
        if (!p.threadRoot.isEmpty())
            o["root"] = p.threadRoot;
        if (!p.snippet.isEmpty())
            o["snippet"] = p.snippet;
        if (!p.author.value.isEmpty())
            o["author"] = p.author.value;
        if (!p.botName.isEmpty())
            o["botName"] = p.botName;
        if (!p.botAvatarUrl.isEmpty())
            o["botAvatar"] = p.botAvatarUrl;
        arr.append(o);
    }
    metaObject()["reminderPreviews"] = arr;
    writeMeta();
}

QHash<QString, ReminderPreview> WorkspaceCache::loadReminderPreviews() const {
    QHash<QString, ReminderPreview> out;
    for (const auto &v : metaObject().value("reminderPreviews").toArray()) {
        const auto    o   = v.toObject();
        const QString key = o.value("key").toString();
        if (key.isEmpty())
            continue;
        ReminderPreview p;
        p.threadRoot   = o.value("root").toString();
        p.snippet      = o.value("snippet").toString();
        p.author       = UserId{o.value("author").toString()};
        p.botName      = o.value("botName").toString();
        p.botAvatarUrl = o.value("botAvatar").toString();
        if (!p.isEmpty())
            out.insert(key, std::move(p));
    }
    return out;
}

void WorkspaceCache::saveDeadConvIds(const QStringList &ids) {
    metaObject()["deadConvIds"] = QJsonArray::fromStringList(ids);
    writeMeta();
}

QStringList WorkspaceCache::loadDeadConvIds() const {
    QStringList out;
    for (const auto &v : metaObject().value("deadConvIds").toArray())
        out.append(v.toString());
    return out;
}

void WorkspaceCache::saveUserProbeTimes(const QHash<QString, qint64> &byUserId) {
    QJsonObject o;
    for (auto it = byUserId.constBegin(); it != byUserId.constEnd(); ++it)
        o[it.key()] = QJsonValue(qint64(it.value()));
    metaObject()["userProbeTimes"] = o;
    writeMeta();
}

QHash<QString, qint64> WorkspaceCache::loadUserProbeTimes() const {
    QHash<QString, qint64> out;
    const QJsonObject      o = metaObject().value("userProbeTimes").toObject();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        out.insert(it.key(), it.value().toVariant().toLongLong());
    return out;
}

void WorkspaceCache::saveEmojiMap(const QHash<QString, QString> &map) {
    QJsonObject o;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        o[it.key()] = it.value();
    writeJson(emojiPath(), QJsonDocument(o));
}

QHash<QString, QString> WorkspaceCache::loadEmojiMap() const {
    const auto data = readFile(emojiPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return {};
    QHash<QString, QString> result;
    const auto              obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        result[it.key()] = it.value().toString();
    return result;
}

void WorkspaceCache::saveImage(const QString &url, const QByteArray &data) {
    if (data.isEmpty())
        return;
    if (writeFile(imgPath(url), data))
        CacheEvictor::noteBytesWritten(data.size());
}

QByteArray WorkspaceCache::loadImage(const QString &url) const {
    const QString path = imgPath(url);
    const auto    data = readFile(path);
    if (data.isEmpty())
        return data;
    // LRU bookkeeping for CacheEvictor: a blob's mtime is its last-used time.
    // Bumped at most hourly — finer grain isn't worth a write per read.
    const auto now = QDateTime::currentDateTimeUtc();
    if (QFileInfo(path).lastModified().secsTo(now) > 3600) {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(now, QFileDevice::FileModificationTime);
    }
    return data;
}
