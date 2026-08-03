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
    o["up"] = f.urlPrivate;
    o["th"] = f.thumbUrl;
    o["iw"] = f.imageWidth;
    o["ih"] = f.imageHeight;
    o["sz"] = static_cast<double>(f.size);
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
    f.id          = o["id"].toString();
    f.name        = o["na"].toString();
    f.mimeType    = o["mi"].toString();
    f.urlPrivate  = o["up"].toString();
    f.thumbUrl    = o["th"].toString();
    f.imageWidth  = o["iw"].toInt();
    f.imageHeight = o["ih"].toInt();
    f.size        = static_cast<qint64>(o["sz"].toDouble());
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
    return o;
}
static Attachment attachmentFromJson(const QJsonObject &o) {
    Attachment a;
    a.fallback    = o["fb"].toString();
    a.color       = o["co"].toString();
    a.pretext     = o["pt"].toString();
    a.authorName  = o["an"].toString();
    a.title       = o["ti"].toString();
    a.titleLink   = o["tl"].toString();
    a.text        = tweFromJson(o["tx"].toObject());
    a.imageUrl    = o["iu"].toString();
    a.thumbUrl    = o["tu"].toString();
    a.footer      = o["fo"].toString();
    a.imageWidth  = o["iw"].toInt();
    a.imageHeight = o["ih"].toInt();
    a.thumbWidth  = o["tw"].toInt();
    a.thumbHeight = o["tg"].toInt();
    for (const auto &v : o["bl"].toArray())
        a.blocks.push_back(blockFromJson(v.toObject()));
    a.buttons = buttonsFromJson(o["bt"].toArray());
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
    return o;
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
    for (const auto &v : o["at"].toArray())
        m.attachments.push_back(attachmentFromJson(v.toObject()));
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
    if (c.locallyMuted)
        o["lm"] = true;
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
    if (o.contains("lm"))
        c.locallyMuted = o["lm"].toBool();
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
