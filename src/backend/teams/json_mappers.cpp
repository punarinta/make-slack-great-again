// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "json_mappers.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <unordered_map>

namespace teams {

QString channelConvId(const QString &teamId, const QString &channelId) {
    return teamId + QLatin1Char('|') + channelId;
}

bool isChannelConvId(const QString &convId) {
    return convId.contains(QLatin1Char('|'));
}

std::pair<QString, QString> splitChannelConvId(const QString &convId) {
    const int bar = convId.indexOf(QLatin1Char('|'));
    if (bar < 0)
        return {QString(), convId};
    return {convId.left(bar), convId.mid(bar + 1)};
}

QString graphReactionType(const QString &name) {
    static const QHash<QString, QString> m = {
        {QStringLiteral("+1"), QStringLiteral("like")},
        {QStringLiteral("thumbsup"), QStringLiteral("like")},
        {QStringLiteral("heart"), QStringLiteral("heart")},
        {QStringLiteral("hearts"), QStringLiteral("heart")},
        {QStringLiteral("joy"), QStringLiteral("laugh")},
        {QStringLiteral("laughing"), QStringLiteral("laugh")},
        {QStringLiteral("open_mouth"), QStringLiteral("surprised")},
        {QStringLiteral("astonished"), QStringLiteral("surprised")},
        {QStringLiteral("cry"), QStringLiteral("sad")},
        {QStringLiteral("disappointed"), QStringLiteral("sad")},
        {QStringLiteral("rage"), QStringLiteral("angry")},
        {QStringLiteral("angry"), QStringLiteral("angry")},
    };
    return m.value(name, name); // unknown → pass through (Graph accepts unicode/custom)
}

namespace {

qint64 isoToMicros(const QString &iso) {
    if (iso.isEmpty())
        return 0;
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(iso, Qt::ISODate);
    return dt.isValid() ? dt.toMSecsSinceEpoch() * 1000 : 0;
}

QString decodeEntities(QString s) {
    // Numeric character references first (so a literal "&#38;" doesn't get
    // double-decoded by the &amp; pass).
    static const QRegularExpression numRef(QStringLiteral("&#(\\d+);"));
    QString                         out;
    qsizetype                       last = 0;
    auto                            it   = numRef.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        out += QChar(m.captured(1).toInt());
        last = m.capturedEnd();
    }
    out += s.mid(last);
    out.replace(QLatin1String("&lt;"), QStringLiteral("<"));
    out.replace(QLatin1String("&gt;"), QStringLiteral(">"));
    out.replace(QLatin1String("&quot;"), QStringLiteral("\""));
    out.replace(QLatin1String("&#39;"), QStringLiteral("'"));
    out.replace(QLatin1String("&apos;"), QStringLiteral("'"));
    out.replace(QLatin1String("&nbsp;"), QStringLiteral(" "));
    out.replace(QLatin1String("&amp;"), QStringLiteral("&"));
    return out;
}

std::optional<EntityType> tagToType(const QString &name) {
    if (name == QLatin1String("b") || name == QLatin1String("strong"))
        return EntityType::Bold;
    if (name == QLatin1String("i") || name == QLatin1String("em"))
        return EntityType::Italic;
    if (name == QLatin1String("u"))
        return EntityType::Underline;
    if (name == QLatin1String("s") || name == QLatin1String("strike") ||
        name == QLatin1String("del"))
        return EntityType::Strike;
    if (name == QLatin1String("code"))
        return EntityType::Code;
    if (name == QLatin1String("pre"))
        return EntityType::Pre;
    if (name == QLatin1String("blockquote"))
        return EntityType::Blockquote;
    if (name == QLatin1String("a"))
        return EntityType::Link;
    return std::nullopt;
}

// Graph returns a reaction's reactionType as either the Unicode emoji itself
// (e.g. "👍" — passes through; the emoji resolver renders the glyph directly) or
// one of the six legacy enum strings. Map the latter to the Slack shortcode our
// emoji table knows, so they render as glyphs instead of a literal ":like:".
// Reverses graphReactionType, so toggling a mapped reaction round-trips.
QString reactionShortName(const QString &reactionType) {
    static const QHash<QString, QString> m = {
        {QStringLiteral("like"), QStringLiteral("thumbsup")},
        {QStringLiteral("heart"), QStringLiteral("heart")},
        {QStringLiteral("laugh"), QStringLiteral("laughing")},
        {QStringLiteral("surprised"), QStringLiteral("open_mouth")},
        {QStringLiteral("sad"), QStringLiteral("cry")},
        {QStringLiteral("angry"), QStringLiteral("angry")},
    };
    return m.value(reactionType, reactionType);
}

} // namespace

std::vector<InlineImage> extractInlineImages(const QString &html) {
    std::vector<InlineImage>        out;
    static const QRegularExpression imgRe(
        QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression srcRe(
        QStringLiteral("src\\s*=\\s*\"([^\"]*)\""), QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression wRe(
        QStringLiteral("width\\s*=\\s*\"?(\\d+)"), QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression hRe(
        QStringLiteral("height\\s*=\\s*\"?(\\d+)"), QRegularExpression::CaseInsensitiveOption
    );
    auto it = imgRe.globalMatch(html);
    while (it.hasNext()) {
        const QString tag = it.next().captured(0);
        const auto    sm  = srcRe.match(tag);
        if (!sm.hasMatch())
            continue;
        InlineImage im;
        im.url = decodeEntities(sm.captured(1)); // URLs carry &amp; etc.
        if (const auto wm = wRe.match(tag); wm.hasMatch())
            im.width = wm.captured(1).toInt();
        if (const auto hm = hRe.match(tag); hm.hasMatch())
            im.height = hm.captured(1).toInt();
        if (!im.url.isEmpty())
            out.push_back(im);
    }
    return out;
}

namespace JsonMappers {

TextWithEntities htmlToText(const QString &html) {
    TextWithEntities out;
    struct Open {
        EntityType type;
        int        start;
        QString    data;
    };
    std::vector<Open>               stack;
    static const QRegularExpression hrefRe(QStringLiteral("href\\s*=\\s*\"([^\"]*)\""));
    static const QRegularExpression nameRe(QStringLiteral("[\\s/]"));
    const qsizetype                 n = html.size();
    qsizetype                       i = 0;
    while (i < n) {
        if (html[i] == QLatin1Char('<')) {
            const qsizetype end = html.indexOf(QLatin1Char('>'), i);
            if (end < 0)
                break;
            QString tag        = html.mid(i + 1, end - i - 1).trimmed();
            i                  = end + 1;
            const bool closing = tag.startsWith(QLatin1Char('/'));
            if (closing)
                tag = tag.mid(1).trimmed();
            const QString name = tag.section(nameRe, 0, 0).toLower();

            if (name == QLatin1String("br")) {
                out.text += QLatin1Char('\n');
                continue;
            }
            if (name == QLatin1String("p") || name == QLatin1String("div")) {
                if (closing && !out.text.isEmpty() && !out.text.endsWith(QLatin1Char('\n')))
                    out.text += QLatin1Char('\n');
                continue;
            }
            const auto type = tagToType(name);
            if (!type)
                continue; // unknown tag — its inner text is still emitted
            if (!closing) {
                QString data;
                if (*type == EntityType::Link) {
                    const auto m = hrefRe.match(tag);
                    if (m.hasMatch())
                        data = m.captured(1);
                }
                stack.push_back({*type, static_cast<int>(out.text.size()), data});
            } else {
                for (int k = static_cast<int>(stack.size()) - 1; k >= 0; --k) {
                    if (stack[k].type == *type) {
                        const Open op = stack[k];
                        stack.erase(stack.begin() + k);
                        const int len = static_cast<int>(out.text.size()) - op.start;
                        if (len > 0)
                            out.entities.push_back({op.type, op.start, len, op.data});
                        break;
                    }
                }
            }
        } else {
            const qsizetype lt = html.indexOf(QLatin1Char('<'), i);
            const qsizetype to = lt < 0 ? n : lt;
            out.text += decodeEntities(html.mid(i, to - i));
            i = to;
        }
    }
    // Trim trailing whitespace/newlines (a closing </p>/</div> emits a '\n' that
    // would otherwise leave a blank line — e.g. before an attachment chip). Only
    // the *trailing* end, so entity offsets (measured from the start) stay valid;
    // clamp any entity that ran into the trimmed tail.
    while (!out.text.isEmpty() && out.text.back().isSpace())
        out.text.chop(1);
    for (auto &e : out.entities) {
        if (e.offset >= out.text.size())
            e.length = 0;
        else if (e.offset + e.length > out.text.size())
            e.length = static_cast<int>(out.text.size()) - e.offset;
    }
    return out;
}

User toUser(const QJsonObject &o) {
    User u;
    u.id          = UserId{o.value(QStringLiteral("id")).toString()};
    u.displayName = o.value(QStringLiteral("displayName")).toString();
    u.name        = o.value(QStringLiteral("userPrincipalName")).toString();
    if (u.name.isEmpty())
        u.name = o.value(QStringLiteral("mail")).toString();
    u.email = o.value(QStringLiteral("mail")).toString();
    if (u.email.isEmpty())
        u.email = o.value(QStringLiteral("userPrincipalName")).toString();
    u.title = o.value(QStringLiteral("jobTitle")).toString();
    return u;
}

User toMember(const QJsonObject &m) {
    User u;
    u.id          = UserId{m.value(QStringLiteral("userId")).toString()};
    u.displayName = m.value(QStringLiteral("displayName")).toString();
    u.name        = m.value(QStringLiteral("email")).toString();
    u.email       = u.name;
    return u;
}

Message toMessage(const QJsonObject &m) {
    Message msg;
    msg.ts   = m.value(QStringLiteral("id")).toString();
    msg.date = isoToMicros(m.value(QStringLiteral("createdDateTime")).toString());

    const auto body    = m.value(QStringLiteral("body")).toObject();
    const auto content = body.value(QStringLiteral("content")).toString();
    if (body.value(QStringLiteral("contentType")).toString() == QLatin1String("html"))
        msg.text = htmlToText(content);
    else
        msg.text.text = content;
    msg.rawText = content;

    const auto from = m.value(QStringLiteral("from")).toObject();
    if (const auto user = from.value(QStringLiteral("user")).toObject(); !user.isEmpty())
        msg.author = UserId{user.value(QStringLiteral("id")).toString()};
    else if (const auto app = from.value(QStringLiteral("application")).toObject(); !app.isEmpty())
        msg.botName = app.value(QStringLiteral("displayName")).toString();

    msg.edited = !m.value(QStringLiteral("lastEditedDateTime")).toString().isEmpty();

    // Reactions: group Graph's per-user entries by reactionType.
    std::unordered_map<QString, Reaction> rx;
    std::vector<QString>                  order;
    for (const auto rv : m.value(QStringLiteral("reactions")).toArray()) {
        const auto ro   = rv.toObject();
        const auto type = reactionShortName(ro.value(QStringLiteral("reactionType")).toString());
        if (type.isEmpty())
            continue;
        auto it = rx.find(type);
        if (it == rx.end()) {
            order.push_back(type);
            it = rx.emplace(type, Reaction{type, 0, {}}).first;
        }
        it->second.count++;
        const auto uid = ro.value(QStringLiteral("user"))
                             .toObject()
                             .value(QStringLiteral("user"))
                             .toObject()
                             .value(QStringLiteral("id"))
                             .toString();
        if (!uid.isEmpty())
            it->second.users.push_back(UserId{uid});
    }
    for (const auto &t : order)
        msg.reactions.push_back(rx[t]);

    if (const auto replyTo = m.value(QStringLiteral("replyToId")).toString(); !replyTo.isEmpty())
        msg.threadRoot = replyTo;

    // Thread summary. Graph's chatMessage carries no reply count of its own, but a
    // `?$expand=replies` list request embeds the replies plus a `replies@odata.count`
    // sibling key on each root. Surface that as replyCount/replyUsers/latestReply so
    // the message-list reply bar appears and the thread can be opened (loadThread
    // then fetches the full reply chain). Absent on chat/reply messages → no-op.
    if (m.contains(QStringLiteral("replies@odata.count")) ||
        m.contains(QStringLiteral("replies"))) {
        const auto replies = m.value(QStringLiteral("replies")).toArray();
        msg.replyCount     = m.value(QStringLiteral("replies@odata.count")).toInt(replies.size());
        qint64 newest      = 0;
        for (const auto rv : replies) {
            const auto ro = rv.toObject();
            if (ro.value(QStringLiteral("messageType")).toString() != QLatin1String("message"))
                continue;
            const auto uid = ro.value(QStringLiteral("from"))
                                 .toObject()
                                 .value(QStringLiteral("user"))
                                 .toObject()
                                 .value(QStringLiteral("id"))
                                 .toString();
            if (!uid.isEmpty() && msg.replyUsers.size() < 5 &&
                std::find(msg.replyUsers.begin(), msg.replyUsers.end(), UserId{uid}) ==
                    msg.replyUsers.end())
                msg.replyUsers.push_back(UserId{uid});
            const auto rDate = isoToMicros(ro.value(QStringLiteral("createdDateTime")).toString());
            if (rDate >= newest) {
                newest          = rDate;
                msg.latestReply = ro.value(QStringLiteral("id")).toString();
            }
        }
    }

    // File attachments. Teams attaches files as "reference" attachments pointing at
    // a SharePoint/OneDrive URL; surface them as File chips (permalink opens in the
    // browser — the contentUrl isn't a Graph download endpoint, so urlPrivate is
    // left empty). Other attachment kinds (cards, message references) are ignored.
    static const QMimeDatabase mimeDb;
    for (const auto av : m.value(QStringLiteral("attachments")).toArray()) {
        const auto a = av.toObject();
        if (a.value(QStringLiteral("contentType")).toString() != QLatin1String("reference"))
            continue;
        File f;
        f.id          = a.value(QStringLiteral("id")).toString();
        f.name        = a.value(QStringLiteral("name")).toString();
        f.permalink   = a.value(QStringLiteral("contentUrl")).toString();
        const auto mt = mimeDb.mimeTypeForFile(f.name, QMimeDatabase::MatchExtension);
        f.mimeType    = mt.name();
        f.prettyType  = mt.comment();
        msg.files.push_back(std::move(f));
    }

    return msg;
}

Conversation toChatConversation(const QJsonObject &chat, const QString &meId) {
    Conversation c;
    c.id               = ConversationId{chat.value(QStringLiteral("id")).toString()};
    c.isMember         = true;
    const auto type    = chat.value(QStringLiteral("chatType")).toString();
    const auto members = chat.value(QStringLiteral("members")).toArray();

    if (type == QLatin1String("oneOnOne")) {
        c.kind = ConvKind::Im;
        for (const auto mv : members) {
            const auto m   = mv.toObject();
            const auto uid = m.value(QStringLiteral("userId")).toString();
            if (!uid.isEmpty() && uid != meId) {
                c.name   = m.value(QStringLiteral("displayName")).toString();
                c.dmUser = UserId{uid};
                break;
            }
        }
    } else {
        c.kind              = ConvKind::Mpim;
        const QString topic = chat.value(QStringLiteral("topic")).toString();
        QStringList   names;
        for (const auto mv : members) {
            const auto m   = mv.toObject();
            const auto uid = m.value(QStringLiteral("userId")).toString();
            if (!uid.isEmpty())
                c.members.push_back(UserId{uid});
            if (uid != meId)
                names << m.value(QStringLiteral("displayName")).toString();
        }
        c.name = topic.isEmpty() ? names.join(QStringLiteral(", ")) : topic;
    }

    // lastMessagePreview.id is the most-recent message id — a usable latest marker.
    c.latestTs = chat.value(QStringLiteral("lastMessagePreview"))
                     .toObject()
                     .value(QStringLiteral("id"))
                     .toString();
    return c;
}

Conversation
toChannelConversation(const QJsonObject &channel, const QString &teamId, const QString &teamName) {
    Conversation c;
    c.id = ConversationId{channelConvId(teamId, channel.value(QStringLiteral("id")).toString())};
    const auto mt = channel.value(QStringLiteral("membershipType")).toString();
    c.kind = mt == QLatin1String("private") ? ConvKind::PrivateChannel : ConvKind::PublicChannel;
    c.name = channel.value(QStringLiteral("displayName")).toString();
    // The parent team name disambiguates same-named channels across teams.
    c.description = teamName;
    c.isMember    = true;
    return c;
}

SearchResult toSearchResult(const QJsonObject &res) {
    SearchResult r;
    r.msg         = toMessage(res);
    const auto ch = res.value(QStringLiteral("channelIdentity")).toObject();
    if (!ch.isEmpty())
        r.conv = ConversationId{channelConvId(
            ch.value(QStringLiteral("teamId")).toString(),
            ch.value(QStringLiteral("channelId")).toString()
        )};
    else
        r.conv = ConversationId{res.value(QStringLiteral("chatId")).toString()};
    return r;
}

bool presenceActive(const QString &a) {
    return a.startsWith(QLatin1String("Available")) || a.startsWith(QLatin1String("Busy")) ||
           a == QLatin1String("DoNotDisturb");
}

MyProfile toMyProfile(const QJsonObject &me) {
    MyProfile p;
    p.realName    = me.value(QStringLiteral("displayName")).toString();
    p.displayName = me.value(QStringLiteral("displayName")).toString();
    p.email       = me.value(QStringLiteral("mail")).toString();
    if (p.email.isEmpty())
        p.email = me.value(QStringLiteral("userPrincipalName")).toString();
    p.phone = me.value(QStringLiteral("mobilePhone")).toString();
    if (p.phone.isEmpty()) {
        const auto phones = me.value(QStringLiteral("businessPhones")).toArray();
        if (!phones.isEmpty())
            p.phone = phones.first().toString();
    }
    return p;
}

} // namespace JsonMappers
} // namespace teams
