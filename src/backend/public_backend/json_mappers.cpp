// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "json_mappers.h"
#include "text/mrkdwn_parser.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace JsonMappers {

User toUser(const QJsonObject &o) {
    auto profile = o.value("profile").toObject();
    // display_name / real_name are often "" (empty string, not null) → must check after toString()
    const auto dn = profile.value("display_name").toString().trimmed();
    const auto rn = profile.value("real_name").toString().trimmed();
    const auto displayName = !dn.isEmpty() ? dn
                           : !rn.isEmpty() ? rn
                           : o.value("name").toString();
    // Strip enclosing colons from status_emoji: ":palm_tree:" → "palm_tree"
    auto rawEmoji = profile.value("status_emoji").toString();
    if (rawEmoji.startsWith(':')) rawEmoji = rawEmoji.mid(1);
    if (rawEmoji.endsWith(':'))   rawEmoji.chop(1);
    return User{
        .id            = UserId{ o.value("id").toString() },
        .name          = o.value("name").toString(),
        .displayName   = displayName,
        .avatarUrl     = profile.value("image_72").toString(),
        .isBot         = o.value("is_bot").toBool(),
        .isActive      = false, // filled by presence poll
        .isDeactivated = o.value("deleted").toBool(),
        .isAdmin       = o.value("is_admin").toBool() || o.value("is_owner").toBool(),
        .statusEmoji   = rawEmoji,
        .statusText    = profile.value("status_text").toString(),
    };
}

Conversation toConversation(const QJsonObject &o) {
    const auto typeStr = o.value("is_im").toBool()   ? "im"
                       : o.value("is_mpim").toBool() ? "mpim"
                       : o.value("is_private").toBool() ? "private_channel"
                       : "public_channel";

    ConvKind kind = ConvKind::PublicChannel;
    if      (typeStr == "im")             kind = ConvKind::Im;
    else if (typeStr == "mpim")           kind = ConvKind::Mpim;
    else if (typeStr == "private_channel")kind = ConvKind::PrivateChannel;

    std::optional<UserId> dmUser;
    if (kind == ConvKind::Im)
        dmUser = UserId{ o.value("user").toString() };

    const auto topic   = o.value("topic").toObject().value("value").toString().trimmed();
    const auto purpose = o.value("purpose").toObject().value("value").toString().trimmed();

    return Conversation{
        .id          = ConversationId{ o.value("id").toString() },
        .kind        = kind,
        .name        = o.value("name").toString(
                           o.value("user").toString()), // Im: use user id as fallback
        .description = !topic.isEmpty() ? topic : purpose,
        .isMember    = o.value("is_member").toBool(true),
        .lastRead    = o.value("last_read").toString(),
        .unread      = o.value("unread_count").toInt(),
        .dmUser      = dmUser,
    };
}

static std::vector<Reaction> parseReactions(const QJsonArray &arr) {
    std::vector<Reaction> out;
    for (auto v : arr) {
        auto r = v.toObject();
        std::vector<UserId> users;
        for (auto u : r.value("users").toArray())
            users.push_back(UserId{ u.toString() });
        out.push_back(Reaction{
            r.value("name").toString(),
            r.value("count").toInt(),
            std::move(users)
        });
    }
    return out;
}

// Local text builder for rich_text block conversion.
struct Builder {
    QString                 text;
    std::vector<TextEntity> entities;
};

// Convert a rich_text inline element to a fragment of TextWithEntities.
static void richInlineToTWE(const QJsonObject &el, Builder &b) {
    // Build plain text + entity spans from a structured Slack rich_text element.
    const auto type = el.value("type").toString();
    if (type == "text") {
        const auto style = el.value("style").toObject();
        const auto text  = el.value("text").toString();
        const int  start = b.text.size();
        b.text += text;
        if (style.value("bold").toBool())
            b.entities.push_back({EntityType::Bold, start, (int)text.size(), {}});
        else if (style.value("italic").toBool())
            b.entities.push_back({EntityType::Italic, start, (int)text.size(), {}});
        else if (style.value("strike").toBool())
            b.entities.push_back({EntityType::Strike, start, (int)text.size(), {}});
        else if (style.value("code").toBool())
            b.entities.push_back({EntityType::Code, start, (int)text.size(), {}});
    } else if (type == "user") {
        const auto uid   = el.value("user_id").toString();
        const int  start = b.text.size();
        b.text += "@" + uid;
        b.entities.push_back({EntityType::UserMention, start, (int)b.text.size() - start, uid});
    } else if (type == "channel") {
        const auto cid   = el.value("channel_id").toString();
        const int  start = b.text.size();
        b.text += "#" + cid;
        b.entities.push_back({EntityType::ChannelMention, start, (int)b.text.size() - start, cid});
    } else if (type == "emoji") {
        const auto name  = el.value("name").toString();
        const int  start = b.text.size();
        b.text += ":" + name + ":";
        b.entities.push_back({EntityType::Emoji, start, (int)b.text.size() - start, name});
    } else if (type == "link") {
        const auto url   = el.value("url").toString();
        const auto label = el.value("text").toString(url);
        const int  start = b.text.size();
        b.text += label;
        b.entities.push_back({EntityType::Link, start, (int)b.text.size() - start, url});
    } else if (type == "broadcast") {
        const auto range = el.value("range").toString();
        const int  start = b.text.size();
        if (range == "here") {
            b.text += "@here";
            b.entities.push_back({EntityType::HereCommand, start, (int)b.text.size() - start, {}});
        } else {
            b.text += "@" + range;
            b.entities.push_back({EntityType::ChannelCommand, start, (int)b.text.size() - start, {}});
        }
    } else {
        // Unknown inline type — emit raw text if present
        const auto text = el.value("text").toString();
        if (!text.isEmpty()) b.text += text;
    }
}

// Parse a rich_text block's elements array to TextWithEntities.
static TextWithEntities richTextToTWE(const QJsonObject &block) {
    Builder b;
    for (const auto &sectionVal : block.value("elements").toArray()) {
        const auto section = sectionVal.toObject();
        const auto stype   = section.value("type").toString();
        const auto elems   = section.value("elements").toArray();

        if (stype == "rich_text_preformatted") {
            const int start = b.text.size();
            for (const auto &ev : elems) richInlineToTWE(ev.toObject(), b);
            b.entities.push_back({EntityType::Pre, start, (int)b.text.size() - start, {}});
        } else if (stype == "rich_text_quote") {
            const int start = b.text.size();
            for (const auto &ev : elems) richInlineToTWE(ev.toObject(), b);
            b.entities.push_back({EntityType::Blockquote, start, (int)b.text.size() - start, {}});
        } else if (stype == "rich_text_list") {
            const auto style   = section.value("style").toString(); // "bullet" or "ordered"
            int        itemIdx = 0;
            for (const auto &ev : elems) {
                // Each list item is a rich_text_section
                const auto item = ev.toObject();
                if (style == "ordered")
                    b.text += QString::number(++itemIdx) + ". ";
                else
                    b.text += "• ";
                for (const auto &ie : item.value("elements").toArray())
                    richInlineToTWE(ie.toObject(), b);
                b.text += "\n";
            }
        } else {
            // rich_text_section (most common) — just parse inline elements
            for (const auto &ev : elems) richInlineToTWE(ev.toObject(), b);
            if (!b.text.endsWith('\n')) b.text += "\n";
        }
    }
    // Trim trailing newline
    while (b.text.endsWith('\n')) b.text.chop(1);
    return TextWithEntities{ b.text, b.entities };
}

File toFile(const QJsonObject &o) {
    return File{
        .id          = o.value("id").toString(),
        .name        = o.value("name").toString(),
        .mimeType    = o.value("mimetype").toString(),
        .prettyType  = o.value("pretty_type").toString(),
        .urlPrivate  = o.value("url_private").toString(),
        .permalink   = o.value("permalink").toString(),
        .thumbUrl    = o.value("thumb_360").toString(o.value("thumb_480").toString()),
        .imageWidth  = o.value("original_w").toInt(o.value("thumb_360_w").toInt()),
        .imageHeight = o.value("original_h").toInt(o.value("thumb_360_h").toInt()),
        .size        = (qint64)o.value("size").toDouble(),
    };
}

// Parse a text-object ({"type":"mrkdwn"|"plain_text","text":"..."}).
static TextWithEntities parseTextObj(const QJsonObject &o) {
    const auto type = o.value("type").toString();
    const auto text = o.value("text").toString();
    if (type == "mrkdwn") return MrkdwnParser::parse(text);
    return TextWithEntities{text, {}};
}

Block toBlock(const QJsonObject &o) {
    Block b;
    b.typeStr = o.value("type").toString();

    if (b.typeStr == "rich_text") {
        b.text = richTextToTWE(o);
    } else if (b.typeStr == "image") {
        b.imageUrl = o.value("image_url").toString();
        b.altText  = o.value("alt_text").toString();
        if (o.contains("title"))
            b.text = parseTextObj(o.value("title").toObject());
    } else if (b.typeStr == "header" || b.typeStr == "section") {
        if (o.contains("text"))
            b.text = parseTextObj(o.value("text").toObject());
    } else if (b.typeStr == "context") {
        // Context blocks have an "elements" array; concatenate text elements.
        Builder cb;
        for (const auto &ev : o.value("elements").toArray()) {
            const auto el = ev.toObject();
            const auto etype = el.value("type").toString();
            if (etype == "mrkdwn" || etype == "plain_text") {
                if (!cb.text.isEmpty()) cb.text += "  ";
                cb.text += el.value("text").toString();
            }
        }
        b.text = TextWithEntities{cb.text, cb.entities};
    } else if (b.typeStr == "actions") {
        // Just show action block text for now; full interactivity in Phase 4.
        Builder ab;
        for (const auto &ev : o.value("elements").toArray()) {
            const auto el = ev.toObject();
            const auto label = el.value("text").toObject().value("text").toString();
            if (!label.isEmpty()) {
                if (!ab.text.isEmpty()) ab.text += "  ";
                ab.text += "[" + label + "]";
            }
        }
        b.text = TextWithEntities{ab.text, ab.entities};
    }
    return b;
}

Attachment toAttachment(const QJsonObject &o) {
    std::vector<Block> blocks;
    for (const auto &bv : o.value("blocks").toArray())
        blocks.push_back(toBlock(bv.toObject()));
    return Attachment{
        .fallback   = o.value("fallback").toString(),
        .color      = o.value("color").toString(),
        .pretext    = o.value("pretext").toString(),
        .authorName = o.value("author_name").toString(),
        .title      = o.value("title").toString(),
        .titleLink  = o.value("title_link").toString(),
        .text       = MrkdwnParser::parse(o.value("text").toString()),
        .imageUrl   = o.value("image_url").toString(),
        .thumbUrl   = o.value("thumb_url").toString(),
        .faviconUrl = o.value("service_icon").toString(),
        .footer     = o.value("footer").toString(),
        .blocks     = std::move(blocks),
    };
}

SearchResult toSearchResult(const QJsonObject &o) {
    return SearchResult{
        .conv     = ConversationId{ o.value("channel").toObject().value("id").toString() },
        .convName = o.value("channel").toObject().value("name").toString(),
        .msg      = toMessage(o),
    };
}

Message toMessage(const QJsonObject &o) {
    // Handle message_changed subtype — actual message is nested under "message"
    auto msg = o;
    if (o.value("subtype").toString() == "message_changed")
        msg = o.value("message").toObject();

    std::vector<File> files;
    for (const auto &fv : msg.value("files").toArray())
        files.push_back(toFile(fv.toObject()));

    std::vector<Block> blocks;
    for (const auto &bv : msg.value("blocks").toArray())
        blocks.push_back(toBlock(bv.toObject()));

    std::vector<Attachment> attachments;
    for (const auto &av : msg.value("attachments").toArray())
        attachments.push_back(toAttachment(av.toObject()));

    // Extract bot display name and avatar from username / bot_profile / icon_url.
    QString botName;
    QString botAvatarUrl;
    if (msg.contains("bot_id")) {
        botName = msg.value("username").toString();
        const auto botProfile = msg.value("bot_profile").toObject();
        if (botName.isEmpty())
            botName = botProfile.value("name").toString();
        const auto icons = botProfile.value("icons").toObject();
        botAvatarUrl = icons.value("image_72").toString(
                       icons.value("image_48").toString(
                       icons.value("image_36").toString()));
        if (botAvatarUrl.isEmpty())
            botAvatarUrl = msg.value("icon_url").toString();
    }

    return Message{
        .ts          = msg.value("ts").toString(),
        .threadRoot  = msg.contains("thread_ts") && msg.value("thread_ts") != msg.value("ts")
                       ? std::optional<Ts>(msg.value("thread_ts").toString())
                       : std::nullopt,
        .replyCount  = msg.value("reply_count").toInt(),
        .replyUsers  = [&]{
            std::vector<UserId> v;
            for (const auto &u : msg.value("reply_users").toArray())
                v.push_back(UserId{u.toString()});
            return v;
        }(),
        .latestReply = msg.contains("latest_reply")
                       ? std::optional<Ts>(msg.value("latest_reply").toString())
                       : std::nullopt,
        .author      = UserId{ msg.value("user").toString(
                               msg.value("bot_id").toString()) },
        .botName      = botName,
        .botAvatarUrl = botAvatarUrl,
        .text        = MrkdwnParser::parse(msg.value("text").toString()),
        .rawText     = msg.value("text").toString(),
        .reactions   = parseReactions(msg.value("reactions").toArray()),
        .edited      = msg.contains("edited"),
        .subtype     = msg.contains("subtype")
                       ? std::optional<QString>(msg.value("subtype").toString())
                       : std::nullopt,
        .files       = std::move(files),
        .blocks      = std::move(blocks),
        .attachments = std::move(attachments),
    };
}

std::vector<User> toUsers(const QJsonArray &a) {
    std::vector<User> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto u = toUser(v.toObject());
        if (!u.id.value.isEmpty()) out.push_back(std::move(u));
    }
    return out;
}

std::vector<Conversation> toConversations(const QJsonArray &a) {
    std::vector<Conversation> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto c = toConversation(v.toObject());
        if (!c.id.value.isEmpty()) out.push_back(std::move(c));
    }
    return out;
}

std::vector<Message> toMessages(const QJsonArray &a, bool reverseOrder) {
    std::vector<Message> out;
    out.reserve(a.size());
    for (auto v : a)
        out.push_back(toMessage(v.toObject()));
    if (reverseOrder)
        std::reverse(out.begin(), out.end());
    return out;
}

std::vector<SearchResult> toSearchResults(const QJsonArray &a) {
    std::vector<SearchResult> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto r = toSearchResult(v.toObject());
        if (!r.conv.value.isEmpty()) out.push_back(std::move(r));
    }
    return out;
}

} // namespace JsonMappers
