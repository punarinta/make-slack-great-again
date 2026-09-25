// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_fixture.h"

#include "text/mrkdwn_parser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

namespace demo {

namespace {

QString prettyTypeFor(const QString &mime, const QString &name) {
    static const QHash<QString, QString> known = {
        {"image/png", "PNG"},
        {"image/jpeg", "JPEG"},
        {"image/gif", "GIF"},
        {"image/webp", "WebP"},
        {"image/svg+xml", "SVG"},
        {"application/pdf", "PDF"},
        {"text/csv", "CSV"},
        {"text/plain", "Plain text"},
        {"application/zip", "Zip"},
    };
    if (const auto it = known.constFind(mime); it != known.constEnd())
        return *it;
    const QString suffix = QFileInfo(name).suffix();
    return suffix.isEmpty() ? QStringLiteral("File") : suffix.toUpper();
}

File fileFrom(const QJsonObject &o, const QString &dir, int index) {
    const QString rel = o.value("path").toString();
    File          f   = fileFromLocalPath(QDir(dir).absoluteFilePath(rel), index);
    if (o.contains("name"))
        f.name = o.value("name").toString();
    if (o.contains("mime"))
        f.mimeType = o.value("mime").toString();
    if (o.contains("type"))
        f.prettyType = o.value("type").toString();
    if (o.value("width").toInt() > 0 && o.value("height").toInt() > 0) {
        f.imageWidth  = o.value("width").toInt();
        f.imageHeight = o.value("height").toInt();
        f.thumbs      = {FileThumb{f.imageWidth, f.imageHeight, f.urlPrivate}};
    }
    // Audio: a voice clip ("subtype": "slack_audio") with its transcript, or a
    // plain upload with just a duration.
    f.subtype    = o.value("subtype").toString();
    f.durationMs = qint64(o.value("durationMs").toDouble());
    if (const QString t = o.value("transcript").toString(); !t.isEmpty()) {
        f.transcriptStatus  = QStringLiteral("complete");
        f.transcriptPreview = t;
    }
    return f;
}

Attachment attachmentFrom(const QJsonObject &o, const QString &dir) {
    Attachment a;
    a.fallback   = o.value("title").toString();
    a.color      = o.value("color").toString();
    a.pretext    = o.value("pretext").toString();
    a.authorName = o.value("author").toString();
    a.title      = o.value("title").toString();
    a.titleLink  = o.value("link").toString();
    a.text       = MrkdwnParser::parse(o.value("text").toString());
    a.footer     = o.value("footer").toString(o.value("service").toString());
    if (const QString fav = o.value("favicon").toString(); !fav.isEmpty()) {
        a.faviconUrl = assetUrl(dir, fav);
        a.footerIcon = a.faviconUrl;
    }
    if (const QString img = o.value("image").toString(); !img.isEmpty()) {
        a.imageUrl     = assetUrl(dir, img);
        const QSize sz = QImageReader(QDir(dir).absoluteFilePath(img)).size();
        a.imageWidth   = o.value("width").toInt(sz.width());
        a.imageHeight  = o.value("height").toInt(sz.height());
    }
    for (const auto &fv : o.value("fields").toArray()) {
        const auto fo = fv.toObject();
        a.fields.push_back(
            AttachmentField{
                fo.value("title").toString(), MrkdwnParser::parse(fo.value("value").toString())
            }
        );
    }
    a.isLinkPreview = o.value("linkPreview").toBool(!a.titleLink.isEmpty() && a.fields.empty());
    return a;
}

std::vector<Reaction> reactionsFrom(const QJsonArray &arr) {
    std::vector<Reaction> out;
    for (const auto &rv : arr) {
        const auto ro = rv.toObject();
        Reaction   r;
        r.name = ro.value("name").toString();
        for (const auto &u : ro.value("users").toArray())
            r.users.push_back(UserId{u.toString()});
        r.count = int(r.users.size());
        if (!r.name.isEmpty() && r.count > 0)
            out.push_back(std::move(r));
    }
    return out;
}

int tzOffsetSeconds(const QString &spec, bool *ok) {
    // "+02:00" / "-05:30" / "Z"
    *ok = true;
    if (spec.isEmpty() || spec == QLatin1String("Z"))
        return 0;
    static const QRegularExpression re(R"(^([+-])(\d{1,2}):?(\d{2})?$)");
    const auto                      m = re.match(spec);
    if (!m.hasMatch()) {
        *ok = false;
        return 0;
    }
    const int sign = m.captured(1) == QLatin1String("-") ? -1 : 1;
    return sign * (m.captured(2).toInt() * 3600 + m.captured(3).toInt() * 60);
}

struct TsAllocator {
    // Guarantees strictly unique, order-preserving ts strings per conversation.
    QSet<QString> used;
    Ts            allocate(const QDateTime &when) {
        for (int off = 0;; ++off) {
            const Ts ts = tsFor(when, off);
            if (!used.contains(ts)) {
                used.insert(ts);
                return ts;
            }
        }
    }
};

} // namespace

QString assetUrl(const QString &dir, const QString &rel) {
    if (rel.isEmpty())
        return {};
    if (rel.contains(QLatin1String("://")))
        return rel; // already a URL (remote asset — discouraged, but allowed)
    return QUrl::fromLocalFile(QDir(dir).absoluteFilePath(rel)).toString();
}

File fileFromLocalPath(const QString &absPath, int index) {
    const QString url = QUrl::fromLocalFile(absPath).toString();
    File          f;
    f.id                 = QStringLiteral("FDEMO%1").arg(index, 4, 10, QLatin1Char('0'));
    f.name               = QFileInfo(absPath).fileName();
    f.mimeType           = QMimeDatabase().mimeTypeForFile(absPath).name();
    f.prettyType         = prettyTypeFor(f.mimeType, f.name);
    f.urlPrivate         = url;
    f.urlPrivateDownload = url;
    f.thumbUrl           = url;
    f.size               = QFileInfo(absPath).size();
    if (f.mimeType.startsWith("image/")) {
        const QSize sz = QImageReader(absPath).size();
        f.imageWidth   = sz.width();
        f.imageHeight  = sz.height();
        if (f.imageWidth > 0)
            f.thumbs.push_back(FileThumb{f.imageWidth, f.imageHeight, url});
    }
    return f;
}

Ts tsFor(const QDateTime &when, int microOffset) {
    const qint64 micros = when.toMSecsSinceEpoch() * 1000 + microOffset;
    return QStringLiteral("%1.%2")
        .arg(micros / 1000000)
        .arg(micros % 1000000, 6, 10, QLatin1Char('0'));
}

QDateTime parseTimeSpec(const QString &spec, const QDateTime &now, const QDateTime &prev) {
    const QString                   s = spec.trimmed();
    // Relative: "-45m" (before now) / "+7m" (after prev).
    static const QRegularExpression rel(R"(^([+-])(\d+)([smhd])$)");
    if (const auto m = rel.match(s); m.hasMatch()) {
        qint64 secs = m.captured(2).toLongLong();
        switch (m.captured(3).at(0).toLatin1()) {
        case 'm':
            secs *= 60;
            break;
        case 'h':
            secs *= 3600;
            break;
        case 'd':
            secs *= 86400;
            break;
        default:
            break;
        }
        if (m.captured(1) == QLatin1String("-"))
            return now.addSecs(-secs);
        return (prev.isValid() ? prev : now).addSecs(secs);
    }
    // Wall clock: "[-Nd] HH:MM"
    static const QRegularExpression wall(R"(^(?:(-?\d+)d\s+)?(\d{1,2}):(\d{2})$)");
    if (const auto m = wall.match(s); m.hasMatch()) {
        const int   days = m.captured(1).isEmpty() ? 0 : m.captured(1).toInt();
        const QTime t(m.captured(2).toInt(), m.captured(3).toInt());
        if (!t.isValid())
            return {};
        return QDateTime(now.date().addDays(days), t, now.timeZone());
    }
    return {};
}

std::optional<Fixture> loadFixture(const QString &path, QString *error, const QDateTime &now) {
    auto fail = [error](const QString &why) -> std::optional<Fixture> {
        if (error)
            *error = why;
        return std::nullopt;
    };

    QFileInfo info(path);
    if (info.isDir())
        info = QFileInfo(QDir(path).filePath(QStringLiteral("fixture.json")));
    QFile f(info.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open %1").arg(info.absoluteFilePath()));
    QJsonParseError perr;
    const auto      doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull() || !doc.isObject())
        return fail(QStringLiteral("%1: %2").arg(info.fileName(), perr.errorString()));
    const QJsonObject root = doc.object();

    Fixture fx;
    fx.dir = info.absolutePath();

    const auto ws    = root.value("workspace").toObject();
    fx.workspaceId   = ws.value("id").toString(QStringLiteral("DEMO"));
    fx.workspaceName = ws.value("name").toString(QStringLiteral("Demo workspace"));
    fx.workspaceIcon = assetUrl(fx.dir, ws.value("icon").toString());
    fx.me            = UserId{root.value("me").toString()};
    if (fx.me.value.isEmpty())
        return fail(QStringLiteral("\"me\" is required"));

    // ── users ──
    QSet<QString> userIds;
    for (const auto &uv : root.value("users").toArray()) {
        const auto o = uv.toObject();
        User       u;
        u.id          = UserId{o.value("id").toString()};
        u.name        = o.value("name").toString();
        u.displayName = o.value("displayName").toString();
        u.avatarUrl   = assetUrl(fx.dir, o.value("avatar").toString());
        u.isBot       = o.value("bot").toBool();
        u.isActive    = o.value("active").toBool();
        u.isAdmin     = o.value("admin").toBool();
        u.isOwner     = o.value("owner").toBool();
        u.title       = o.value("title").toString();
        u.email       = o.value("email").toString();
        u.dndEnabled  = o.value("dnd").toBool();
        u.teamId      = fx.workspaceId;
        const auto st = o.value("status").toObject();
        u.statusEmoji = st.value("emoji").toString();
        u.statusText  = st.value("text").toString();
        if (o.contains("tz")) {
            bool ok    = false;
            u.tzOffset = tzOffsetSeconds(o.value("tz").toString(), &ok);
            if (!ok)
                return fail(
                    QStringLiteral("user %1: bad tz %2").arg(u.id.value, o.value("tz").toString())
                );
            u.hasTz = true;
        }
        if (u.id.value.isEmpty() || u.name.isEmpty())
            return fail(QStringLiteral("every user needs an id and a name"));
        userIds.insert(u.id.value);
        fx.users.push_back(std::move(u));
    }
    if (!userIds.contains(fx.me.value))
        return fail(QStringLiteral("\"me\" (%1) is not in users").arg(fx.me.value));

    // ── conversations ──
    QHash<QString, int> convIndex;
    for (const auto &cv : root.value("conversations").toArray()) {
        const auto    o    = cv.toObject();
        const QString kind = o.value("kind").toString(QStringLiteral("channel"));
        Conversation  c;
        c.id = ConversationId{o.value("id").toString()};
        if (c.id.value.isEmpty())
            return fail(QStringLiteral("every conversation needs an id"));
        if (kind == QLatin1String("channel"))
            c.kind = ConvKind::PublicChannel;
        else if (kind == QLatin1String("private"))
            c.kind = ConvKind::PrivateChannel;
        else if (kind == QLatin1String("dm"))
            c.kind = ConvKind::Im;
        else if (kind == QLatin1String("group"))
            c.kind = ConvKind::Mpim;
        else
            return fail(QStringLiteral("conversation %1: unknown kind %2").arg(c.id.value, kind));
        c.name         = o.value("name").toString();
        c.description  = o.value("topic").toString();
        c.isMember     = o.value("member").toBool(true);
        c.memberCount  = o.value("memberCount").toInt();
        c.unread       = o.value("unread").toInt();
        c.mentionCount = o.value("mentions").toInt();
        c.isStarred    = o.value("starred").toBool();
        c.isMuted      = o.value("muted").toBool();
        for (const auto &m : o.value("members").toArray())
            c.members.push_back(UserId{m.toString()});
        if (c.kind == ConvKind::Im) {
            const QString peer = o.value("user").toString();
            if (!userIds.contains(peer))
                return fail(QStringLiteral("dm %1: unknown user %2").arg(c.id.value, peer));
            c.dmUser = UserId{peer};
            if (c.name.isEmpty())
                for (const auto &u : fx.users)
                    if (u.id.value == peer)
                        c.name = u.name;
        }
        if (c.kind == ConvKind::Mpim) {
            if (std::find(c.members.begin(), c.members.end(), fx.me) == c.members.end())
                c.members.push_back(fx.me);
            if (c.name.isEmpty()) {
                QStringList names;
                for (const auto &m : c.members)
                    for (const auto &u : fx.users)
                        if (u.id == m)
                            names << u.name;
                c.name = QStringLiteral("mpdm-%1-1").arg(names.join(QStringLiteral("--")));
            }
        }
        if (c.memberCount == 0 && !c.members.empty())
            c.memberCount = int(c.members.size());
        if (o.contains("canvas")) {
            const auto cv = o.value("canvas").toObject();
            Canvas     canvas;
            canvas.conv   = c.id.value;
            canvas.fileId = QStringLiteral("F0CANVAS-%1").arg(c.id.value);
            canvas.title  = cv.value("title").toString();
            QFile html(QDir(fx.dir).filePath(cv.value("html").toString()));
            if (cv.value("html").toString().isEmpty() || !html.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("conversation %1: canvas needs a readable \"html\" file")
                                .arg(c.id.value));
            canvas.html    = QString::fromUtf8(html.readAll());
            c.canvasFileId = canvas.fileId;
            fx.canvases.push_back(std::move(canvas));
        }
        convIndex.insert(c.id.value, int(fx.conversations.size()));
        fx.conversations.push_back(std::move(c));
    }

    // ── messages ──
    std::unordered_map<QString, TsAllocator> alloc;
    std::unordered_map<QString, QDateTime>   prevRoot; // conv → time of the previous root
    int                                      fileIndex = 0;

    auto parseMessage = [&](const QJsonObject &o,
                            const QString     &conv,
                            const QDateTime   &prev,
                            int                index,
                            Message           &out,
                            QDateTime         &when) -> QString {
        when = parseTimeSpec(o.value("time").toString(), now, prev);
        if (!when.isValid())
            return QStringLiteral("message %1 in %2: bad time %3")
                .arg(index)
                .arg(conv, o.value("time").toString());
        out.ts         = alloc[conv].allocate(when);
        out.date       = decimalTsToMicros(out.ts);
        const auto bot = o.value("bot").toObject();
        if (!bot.isEmpty()) {
            out.subtype      = QStringLiteral("bot_message");
            out.botName      = bot.value("name").toString();
            out.botAvatarUrl = assetUrl(fx.dir, bot.value("avatar").toString());
        }
        const QString user = o.value("user").toString();
        if (!user.isEmpty()) {
            if (!userIds.contains(user))
                return QStringLiteral("message %1 in %2: unknown user %3")
                    .arg(index)
                    .arg(conv, user);
            out.author = UserId{user};
        } else if (bot.isEmpty()) {
            return QStringLiteral("message %1 in %2: needs a user or a bot").arg(index).arg(conv);
        }
        if (o.contains("subtype"))
            out.subtype = o.value("subtype").toString();
        out.rawText   = o.value("text").toString();
        out.text      = MrkdwnParser::parse(out.rawText);
        out.reactions = reactionsFrom(o.value("reactions").toArray());
        out.edited    = o.value("edited").toBool();
        out.pinned    = o.value("pinned").toBool();
        if (out.pinned)
            out.pinnedBy = fx.me;
        for (const auto &fv : o.value("files").toArray())
            out.files.push_back(fileFrom(fv.toObject(), fx.dir, ++fileIndex));
        for (const auto &av : o.value("attachments").toArray())
            out.attachments.push_back(attachmentFrom(av.toObject(), fx.dir));
        return {};
    };

    int index = 0;
    for (const auto &mv : root.value("messages").toArray()) {
        ++index;
        const auto    o    = mv.toObject();
        const QString conv = o.value("conv").toString();
        if (!convIndex.contains(conv))
            return fail(QStringLiteral("message %1: unknown conversation %2").arg(index).arg(conv));

        Message   msg;
        QDateTime when;
        if (const QString err = parseMessage(o, conv, prevRoot[conv], index, msg, when);
            !err.isEmpty())
            return fail(err);
        prevRoot[conv] = when;

        const auto replies = o.value("replies").toArray();
        if (!replies.isEmpty()) {
            QDateTime            prev = when;
            std::vector<Message> thread;
            int                  ri = 0;
            for (const auto &rv : replies) {
                ++ri;
                Message   reply;
                QDateTime rwhen;
                if (const QString err = parseMessage(rv.toObject(), conv, prev, ri, reply, rwhen);
                    !err.isEmpty())
                    return fail(QStringLiteral("message %1, reply %2").arg(index).arg(err));
                if (rwhen < when)
                    return fail(QStringLiteral("message %1: reply %2 is dated before its root")
                                    .arg(index)
                                    .arg(ri));
                prev               = rwhen;
                reply.threadRoot   = msg.ts;
                reply.parentUserId = msg.author;
                if (!reply.author.value.isEmpty() &&
                    std::find(msg.replyUsers.begin(), msg.replyUsers.end(), reply.author) ==
                        msg.replyUsers.end() &&
                    msg.replyUsers.size() < 5)
                    msg.replyUsers.push_back(reply.author);
                thread.push_back(std::move(reply));
            }
            msg.replyCount                                               = int(thread.size());
            msg.latestReply                                              = thread.back().ts;
            fx.threads[Fixture::threadKey(ConversationId{conv}, msg.ts)] = std::move(thread);
        }
        fx.history[conv].push_back(std::move(msg));
    }

    // Oldest first, then derive per-conversation cursors from the result.
    for (auto &c : fx.conversations) {
        auto &msgs = fx.history[c.id.value];
        std::stable_sort(msgs.begin(), msgs.end(), MessageDateLess{});
        const int n = int(msgs.size());
        c.latestTs  = n ? msgs.back().ts : Ts();
        if (c.unread > 0 && c.unread < n)
            c.lastRead = msgs[n - c.unread - 1].ts;
        else if (c.unread > 0)
            c.lastRead = QStringLiteral("0");
        else
            c.lastRead = n ? msgs.back().ts : QStringLiteral("0");
    }

    // ── auto replies ──
    for (const auto &av : root.value("autoReplies").toArray()) {
        const auto o = av.toObject();
        AutoReply  r;
        r.conv     = o.value("conv").toString();
        r.user     = UserId{o.value("user").toString()};
        r.text     = o.value("text").toString();
        r.afterMs  = o.value("afterMs").toInt(r.afterMs);
        r.typingMs = o.value("typingMs").toInt(r.typingMs);
        r.inThread = o.value("inThread").toBool();
        if (!convIndex.contains(r.conv) || !userIds.contains(r.user.value) || r.text.isEmpty())
            return fail(QStringLiteral("autoReplies: each needs a known conv, user and a text"));
        fx.autoReplies.push_back(std::move(r));
    }

    // ── unfurls / ai ──
    for (const auto &uv : root.value("unfurls").toArray()) {
        const auto o = uv.toObject();
        Unfurl     u;
        u.url = o.value("url").toString();
        if (u.url.isEmpty())
            return fail(QStringLiteral("unfurls: each needs a url"));
        u.attachment               = attachmentFrom(o, fx.dir);
        u.attachment.isLinkPreview = true;
        if (u.attachment.titleLink.isEmpty())
            u.attachment.titleLink = u.url;
        fx.unfurls.push_back(std::move(u));
    }
    const auto ai = root.value("ai").toObject();
    fx.aiDefault  = ai.value("default").toString();
    for (const auto &rv : ai.value("replies").toArray()) {
        const auto o = rv.toObject();
        AiReply    r{o.value("match").toString(), o.value("text").toString()};
        if (r.match.isEmpty() || r.text.isEmpty())
            return fail(QStringLiteral("ai.replies: each needs match and text"));
        fx.aiReplies.push_back(std::move(r));
    }

    const QString start = root.value("startConversation").toString();
    if (!start.isEmpty() && !convIndex.contains(start))
        return fail(QStringLiteral("startConversation %1 is not a conversation").arg(start));
    fx.startConversation = ConversationId{
        start.isEmpty() ? (fx.conversations.empty() ? QString() : fx.conversations.front().id.value)
                        : start
    };
    return fx;
}

} // namespace demo
