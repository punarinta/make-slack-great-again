// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "json_mappers.h"
#include "text/mrkdwn_parser.h"
#include "util/slack_links.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace slack {
namespace JsonMappers {

User toUser(const QJsonObject &o) {
    auto       profile     = o.value("profile").toObject();
    // display_name / real_name are often "" (empty string, not null) → must check after toString()
    // Prefer real_name: enterprise workspaces often auto-provision display_name from
    // AD/LDAP as a username slug (e.g. "john.doe.dept") while real_name holds the
    // human-readable full name ("John Doe").
    const auto rn          = profile.value("real_name").toString().trimmed();
    const auto dn          = profile.value("display_name").toString().trimmed();
    const auto displayName = !rn.isEmpty() ? rn : !dn.isEmpty() ? dn : o.value("name").toString();
    // Strip enclosing colons: ":palm_tree:" → "palm_tree"
    // Also strip skin-tone modifier suffix: ":baby::skin-tone-3:" → "baby"
    auto       rawEmoji    = profile.value("status_emoji").toString();
    if (rawEmoji.startsWith(':'))
        rawEmoji = rawEmoji.mid(1);
    if (rawEmoji.endsWith(':'))
        rawEmoji.chop(1);
    const int skinToneSep = rawEmoji.indexOf("::");
    if (skinToneSep != -1)
        rawEmoji = rawEmoji.left(skinToneSep);
    return User{
        .id            = UserId{o.value("id").toString()},
        .name          = o.value("name").toString(),
        .displayName   = displayName,
        .avatarUrl     = profile.value("image_72").toString(),
        .isBot         = o.value("is_bot").toBool(),
        .isExternal    = o.value("is_stranger").toBool(),
        .isStranger    = o.value("is_stranger").toBool(),
        .teamId        = o.value("team_id").toString(),
        .isActive      = false, // filled by presence poll
        .isDeactivated = o.value("deleted").toBool(),
        .isAdmin       = o.value("is_admin").toBool() || o.value("is_owner").toBool(),
        .isOwner       = o.value("is_owner").toBool() || o.value("is_primary_owner").toBool(),
        .statusEmoji   = rawEmoji,
        .statusText    = profile.value("status_text").toString(),
        .title         = profile.value("title").toString(),
        .email         = profile.value("email").toString(),
        .hasTz         = o.contains("tz_offset"),
        .tzOffset      = o.value("tz_offset").toInt(),
    };
}

Conversation toConversation(const QJsonObject &o) {
    const auto typeStr = o.value("is_im").toBool()        ? "im"
                         : o.value("is_mpim").toBool()    ? "mpim"
                         : o.value("is_private").toBool() ? "private_channel"
                                                          : "public_channel";

    ConvKind kind = ConvKind::PublicChannel;
    if (typeStr == "im")
        kind = ConvKind::Im;
    else if (typeStr == "mpim")
        kind = ConvKind::Mpim;
    else if (typeStr == "private_channel")
        kind = ConvKind::PrivateChannel;

    std::optional<UserId> dmUser;
    if (kind == ConvKind::Im)
        dmUser = UserId{o.value("user").toString()};

    std::vector<UserId> members;
    if (kind == ConvKind::Mpim) {
        for (const auto &v : o.value("members").toArray())
            members.push_back(UserId{v.toString()});
    }

    const QString     notifPref  = o.value("notification_preference").toString();
    NotificationLevel notifLevel = NotificationLevel::Default;
    if (notifPref == "everything")
        notifLevel = NotificationLevel::All;
    else if (notifPref == "nothing")
        notifLevel = NotificationLevel::Mute;
    else if (notifPref == "mentions")
        notifLevel = NotificationLevel::Mentions;

    const auto topic   = o.value("topic").toObject().value("value").toString().trimmed();
    const auto purpose = o.value("purpose").toObject().value("value").toString().trimmed();

    // Compute timestamps locally so we can use both in the unread fallback below.
    const QString lastRead = o.value("last_read").toString();
    const QString latestTs = [&]() -> QString {
        const auto v = o.value("latest");
        if (v.isObject())
            return v.toObject().value("ts").toString();
        if (v.isString())
            return v.toString(); // DMs return ts directly
        return {};
    }();

    // unread_count is not always populated by the API (often 0 for public channels).
    // latestTs > lastRead is a reliable fallback: Slack timestamps are zero-padded
    // fixed-width strings so lexicographic comparison is identical to numeric.
    const int rawUnread = o.value("unread_count").toInt();
    const int unread    = rawUnread > 0 ? rawUnread
                          : (!latestTs.isEmpty() && !lastRead.isEmpty() && latestTs > lastRead) ? 1
                                                                                                : 0;

    // Channel canvas (conversations.info; conversations.list may omit "properties").
    const auto [canvasFileId, canvasIsEmpty] = channelCanvas(o);

    // Huddle state: a live huddle attaches a `room` object to the channel.
    const auto huddle = readHuddleRoom(o.value("room").toObject());

    return Conversation{
        .id   = ConversationId{o.value("id").toString()},
        .kind = kind,
        .name = o.value("name").toString(o.value("user").toString()), // Im: use user id as fallback
        .description        = !topic.isEmpty() ? topic : purpose,
        .isMember           = o.value("is_member").toBool(true),
        .memberCount        = o.value("num_members").toInt(),
        .lastRead           = lastRead,
        .latestTs           = latestTs,
        .unread             = unread,
        .mentionCount       = o.value("mention_count").toInt(),
        .dmUser             = dmUser,
        .members            = std::move(members),
        .isMuted            = o.value("is_muted").toBool(),
        .isStarred          = o.value("is_starred").toBool(),
        .notifLevel         = notifLevel,
        .canvasFileId       = canvasFileId,
        .canvasIsEmpty      = canvasIsEmpty,
        .huddleActive       = huddle.active,
        .huddleLink         = huddle.link,
        .huddleParticipants = huddle.participants,
    };
}

// A huddle's `room` is ended once Slack sets either signal. has_ended is the
// explicit flag; date_end is the end timestamp (0/absent = still live). Slack
// sends date_end as a JSON number in some payloads and a string in others —
// QJsonValue::toDouble() silently yields 0 for a string, so a string date_end
// would otherwise read as "still live" and strand the huddle banner forever
// (the exact failure where an end-edit arrives but the green bar never clears).
static bool roomHasEnded(const QJsonObject &room) {
    if (room.value("has_ended").toBool(false))
        return true;
    const auto   dateEnd = room.value("date_end");
    // String form (e.g. "1718...") parses via QString; number form via toDouble
    // (a unix-second ts overflows a 32-bit int, so read as double either way).
    const double end     = dateEnd.isString() ? dateEnd.toString().toDouble() : dateEnd.toDouble();
    return end != 0.0;
}

HuddleRoom readHuddleRoom(const QJsonObject &room) {
    HuddleRoom h;
    // A "huddle" (not a third-party Call). Ignore everything else.
    if (room.isEmpty() || room.value("call_family").toString() != "huddle")
        return h;
    // Ongoing while not ended. A freshly-announced "prewarmed" huddle has no
    // participants yet but is live, so we do NOT require a non-empty list.
    h.active = !roomHasEnded(room);
    h.link   = room.value("huddle_link").toString();
    for (const auto &v : room.value("participants").toArray())
        h.participants.push_back(UserId{v.toString()});
    // Nobody connected yet → show the host so the indicator still has a face.
    if (h.participants.empty()) {
        const QString host = room.value("created_by").toString();
        if (!host.isEmpty())
            h.participants.push_back(UserId{host});
    }
    return h;
}

std::pair<QString, bool> channelCanvas(const QJsonObject &channel) {
    const auto props  = channel.value("properties").toObject();
    const auto canvas = props.value("canvas").toObject();
    if (const auto fileId = canvas.value("file_id").toString(); !fileId.isEmpty())
        return {fileId, canvas.value("is_empty").toBool()};
    // Free-team shape: the channel canvas is a "canvas" tab.
    for (const auto tab : props.value("tabs").toArray()) {
        const auto o = tab.toObject();
        if (o.value("type").toString() == QLatin1String("canvas")) {
            const auto fileId = o.value("data").toObject().value("file_id").toString();
            if (!fileId.isEmpty())
                return {fileId, false};
        }
    }
    return {{}, false};
}

QJsonArray toCanvasChanges(const std::vector<CanvasChange> &changes) {
    QJsonArray out;
    for (const auto &c : changes) {
        QJsonObject op;
        switch (c.op) {
        case CanvasChange::Op::InsertAtStart:
            op.insert("operation", "insert_at_start");
            break;
        case CanvasChange::Op::InsertAtEnd:
            op.insert("operation", "insert_at_end");
            break;
        case CanvasChange::Op::InsertAfter:
            op.insert("operation", "insert_after");
            break;
        case CanvasChange::Op::InsertBefore:
            op.insert("operation", "insert_before");
            break;
        case CanvasChange::Op::ReplaceSection:
        case CanvasChange::Op::ReplaceAll:
            op.insert("operation", "replace");
            break;
        case CanvasChange::Op::DeleteSection:
            op.insert("operation", "delete");
            break;
        case CanvasChange::Op::Rename:
            op.insert("operation", "rename");
            break;
        }
        if (!c.sectionId.isEmpty())
            op.insert("section_id", c.sectionId);
        if (c.op == CanvasChange::Op::Rename)
            op.insert("title_content", QJsonObject{{"type", "markdown"}, {"markdown", c.markdown}});
        else if (c.op != CanvasChange::Op::DeleteSection)
            op.insert(
                "document_content", QJsonObject{{"type", "markdown"}, {"markdown", c.markdown}}
            );
        out.append(op);
    }
    return out;
}

static std::vector<Reaction> parseReactions(const QJsonArray &arr) {
    std::vector<Reaction> out;
    for (auto v : arr) {
        auto                r = v.toObject();
        std::vector<UserId> users;
        for (auto u : r.value("users").toArray())
            users.push_back(UserId{u.toString()});
        out.push_back(
            Reaction{r.value("name").toString(), r.value("count").toInt(), std::move(users)}
        );
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
        const auto             style = el.value("style").toObject();
        // A text element's emphasis comes from its style object, but Slack's
        // text→rich_text conversion (and some bots, e.g. Outlook Calendar) leave
        // <!date^…>, <url|label> and <@user> tokens unexpanded inside the raw
        // text. resolveTokens() expands those (and :emoji:) without touching *_~`.
        const TextWithEntities run   = MrkdwnParser::resolveTokens(el.value("text").toString());
        const int              start = b.text.size();
        b.text += run.text;
        // Style span wraps the whole run; token spans nest inside it (the
        // renderer builds containment from offset/length ordering).
        if (style.value("bold").toBool())
            b.entities.push_back({EntityType::Bold, start, (int)run.text.size(), {}});
        else if (style.value("italic").toBool())
            b.entities.push_back({EntityType::Italic, start, (int)run.text.size(), {}});
        else if (style.value("strike").toBool())
            b.entities.push_back({EntityType::Strike, start, (int)run.text.size(), {}});
        else if (style.value("code").toBool())
            b.entities.push_back({EntityType::Code, start, (int)run.text.size(), {}});
        for (auto e : run.entities) {
            e.offset += start;
            b.entities.push_back(e);
        }
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
    } else if (type == "message_mention") {
        // A link to another message, pasted into a rich_text block. Slack sends
        // the permalink AND the parts it points at — including the author, which
        // the URL alone can't give us, so the chip can read "alice in #general"
        // like the official client. `text`/`url` are the permalink; the host only
        // exists there, so parse it out and let the element's own fields win.
        auto ref          = SlackLinks::parseMessageLink(el.value("url").toString());
        ref.conv          = el.value("channel_id").toString(ref.conv);
        ref.ts            = el.value("message_ts").toString(ref.ts);
        ref.author        = el.value("author_id").toString();
        // Slack repeats the message's own ts as thread_ts on a thread ROOT; only
        // a genuine reply gets a thread target (same rule as parseMessageLink).
        const auto thread = el.value("thread_ts").toString();
        ref.threadTs      = (thread.isEmpty() || thread == ref.ts) ? QString() : thread;
        const int start   = b.text.size();
        b.text += el.value("text").toString(el.value("url").toString());
        if (ref.isValid()) {
            b.entities.push_back(
                {EntityType::MessageLink,
                 start,
                 (int)b.text.size() - start,
                 SlackLinks::refToToken(ref)}
            );
        }
    } else if (type == "broadcast") {
        const auto range = el.value("range").toString();
        const int  start = b.text.size();
        if (range == "here") {
            b.text += "@here";
            b.entities.push_back({EntityType::HereCommand, start, (int)b.text.size() - start, {}});
        } else {
            b.text += "@" + range;
            b.entities.push_back(
                {EntityType::ChannelCommand, start, (int)b.text.size() - start, {}}
            );
        }
    } else {
        // Unknown inline type — emit its text (token-resolved) if present.
        const auto text = el.value("text").toString();
        if (!text.isEmpty()) {
            const TextWithEntities run   = MrkdwnParser::resolveTokens(text);
            const int              start = b.text.size();
            b.text += run.text;
            for (auto e : run.entities) {
                e.offset += start;
                b.entities.push_back(e);
            }
        }
    }
}

// Parse a rich_text block's elements array to TextWithEntities.
static TextWithEntities richTextToTWE(const QJsonObject &block) {
    Builder b;
    for (const auto &sectionVal : block.value("elements").toArray()) {
        const auto section = sectionVal.toObject();
        const auto stype   = section.value("type").toString();
        const auto elems   = section.value("elements").toArray();

        if (stype == "rich_text_preformatted" || stype == "rich_text_quote") {
            const int start = b.text.size();
            // The wrapping entity must land BEFORE the ones its content emits:
            // the renderer derives containment from (offset, length) with ties
            // broken by insertion order, so a quote spanning exactly one child
            // (e.g. a lone pasted message link) would otherwise become the CHILD
            // and be dropped — the quote bar silently disappeared.
            const int at    = (int)b.entities.size();
            for (const auto &ev : elems)
                richInlineToTWE(ev.toObject(), b);
            b.entities.insert(
                b.entities.begin() + at,
                {stype == "rich_text_quote" ? EntityType::Blockquote : EntityType::Pre,
                 start,
                 (int)b.text.size() - start,
                 {}}
            );
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
            for (const auto &ev : elems)
                richInlineToTWE(ev.toObject(), b);
            if (!b.text.endsWith('\n'))
                b.text += "\n";
        }
    }
    // Trim trailing newline
    while (b.text.endsWith('\n'))
        b.text.chop(1);
    return TextWithEntities{b.text, b.entities};
}

File toFile(const QJsonObject &o) {
    File f{
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
    // Full thumbnail ladder — the UI picks the variant matching the physical
    // (DPR-scaled) preview size, so previews stay crisp on any screen density.
    static constexpr int kThumbSides[] = {64, 80, 160, 360, 480, 720, 800, 960, 1024};
    for (int side : kThumbSides) {
        const QString key = QStringLiteral("thumb_%1").arg(side);
        const QString url = o.value(key).toString();
        if (url.isEmpty())
            continue;
        f.thumbs.push_back(
            FileThumb{
                o.value(key + "_w").toInt(side),
                o.value(key + "_h").toInt(),
                url,
            }
        );
    }
    // Animated GIF uploads: thumb_N is a static first frame; the thumb_N_gif
    // variants carry the animation (and share thumb_N's dimensions).
    static constexpr int kAnimThumbSides[] = {360, 480};
    for (int side : kAnimThumbSides) {
        const QString url = o.value(QStringLiteral("thumb_%1_gif").arg(side)).toString();
        if (url.isEmpty())
            continue;
        f.animThumbs.push_back(
            FileThumb{
                o.value(QStringLiteral("thumb_%1_w").arg(side)).toInt(side),
                o.value(QStringLiteral("thumb_%1_h").arg(side)).toInt(),
                url,
            }
        );
    }
    // PDFs: Slack prerenders the first page server-side (thumb_pdf + thumb_pdf_w/h).
    if (f.thumbUrl.isEmpty() && o.contains("thumb_pdf")) {
        f.thumbUrl    = o.value("thumb_pdf").toString();
        f.imageWidth  = o.value("thumb_pdf_w").toInt(f.imageWidth);
        f.imageHeight = o.value("thumb_pdf_h").toInt(f.imageHeight);
    }
    return f;
}

// Parse a text-object ({"type":"mrkdwn"|"plain_text","text":"..."}).
static TextWithEntities parseTextObj(const QJsonObject &o) {
    const auto type = o.value("type").toString();
    const auto text = o.value("text").toString();
    if (type == "mrkdwn")
        return MrkdwnParser::parse(text);
    // plain_text: no mrkdwn emphasis (*_~` stay literal), but Slack still expands
    // :emoji: shortcodes — e.g. a bot header ":mega: Notification" — unless the
    // object opts out with "emoji": false (default true). resolveTokens does the
    // emoji pass and leaves marks alone; decodeEntities matches the title path.
    if (o.value("emoji").toBool(true))
        return MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(text));
    return TextWithEntities{MrkdwnParser::decodeEntities(text), {}};
}

// A button element — Block Kit shape ("text" is a plain_text object) or the
// legacy attachment-actions shape ("text" is a plain string).
static BotButton toButton(const QJsonObject &el) {
    const auto textVal = el.value("text");
    return BotButton{
        .text =
            textVal.isObject() ? textVal.toObject().value("text").toString() : textVal.toString(),
        .url   = el.value("url").toString(),
        .style = el.value("style").toString(),
    };
}

Block toBlock(const QJsonObject &o) {
    Block b;
    b.typeStr = o.value("type").toString();

    if (b.typeStr == "rich_text") {
        b.text = richTextToTWE(o);
    } else if (b.typeStr == "image") {
        b.imageUrl    = o.value("image_url").toString();
        b.altText     = o.value("alt_text").toString();
        b.imageWidth  = o.value("image_width").toInt();
        b.imageHeight = o.value("image_height").toInt();
        if (o.contains("title"))
            b.text = parseTextObj(o.value("title").toObject());
    } else if (b.typeStr == "header" || b.typeStr == "section") {
        if (o.contains("text"))
            b.text = parseTextObj(o.value("text").toObject());
        // Section blocks may carry a "fields" array instead of (or alongside) "text";
        // append each field as its own line, shifting entity offsets accordingly.
        for (const auto &fv : o.value("fields").toArray()) {
            const TextWithEntities ft = parseTextObj(fv.toObject());
            if (ft.text.isEmpty())
                continue;
            if (!b.text.text.isEmpty())
                b.text.text += "\n";
            const int base = b.text.text.size();
            b.text.text += ft.text;
            for (auto e : ft.entities) {
                e.offset += base;
                b.text.entities.push_back(e);
            }
        }
        // Accessory button (e.g. "View details" next to a section's text).
        const auto acc = o.value("accessory").toObject();
        if (acc.value("type").toString() == "button")
            b.buttons.push_back(toButton(acc));
    } else if (b.typeStr == "context") {
        // Context blocks have an "elements" array; concatenate text elements
        // (mrkdwn ones parsed, with entity offsets shifted to the joined text).
        TextWithEntities ct;
        for (const auto &ev : o.value("elements").toArray()) {
            const auto el    = ev.toObject();
            const auto etype = el.value("type").toString();
            if (etype != "mrkdwn" && etype != "plain_text")
                continue;
            const TextWithEntities part = parseTextObj(el);
            if (part.text.isEmpty())
                continue;
            if (!ct.text.isEmpty())
                ct.text += "  ";
            const int base = ct.text.size();
            ct.text += part.text;
            for (auto e : part.entities) {
                e.offset += base;
                ct.entities.push_back(e);
            }
        }
        b.text = ct;
    } else if (b.typeStr == "table") {
        // Table messages (Slack's newer editor). Each row is an array of cells;
        // a cell is a nested rich_text block, {"type":"raw_text","text":…}, or
        // null (the header row pads short rows with nulls) → empty cell.
        for (const auto &rv : o.value("rows").toArray()) {
            std::vector<TextWithEntities> row;
            for (const auto &cv : rv.toArray()) {
                const auto cell = cv.toObject();
                const auto ct   = cell.value("type").toString();
                if (ct == "rich_text")
                    row.push_back(richTextToTWE(cell));
                else if (ct == "raw_text")
                    row.push_back(TextWithEntities{cell.value("text").toString(), {}});
                else
                    row.push_back({});
            }
            b.tableRows.push_back(std::move(row));
        }
    } else if (b.typeStr == "actions") {
        // Buttons render as inert chips (URL buttons are clickable); other
        // interactive elements (selects, datepickers) aren't representable.
        for (const auto &ev : o.value("elements").toArray()) {
            const auto el = ev.toObject();
            if (el.value("type").toString() == "button")
                b.buttons.push_back(toButton(el));
        }
    }
    return b;
}

Attachment toAttachment(const QJsonObject &o) {
    std::vector<Block> blocks;
    for (const auto &bv : o.value("blocks").toArray())
        blocks.push_back(toBlock(bv.toObject()));
    std::vector<BotButton> buttons;
    for (const auto &av : o.value("actions").toArray()) {
        const auto ao = av.toObject();
        if (ao.value("type").toString() == "button")
            buttons.push_back(toButton(ao));
    }
    std::vector<AttachmentField> fields;
    for (const auto &fv : o.value("fields").toArray()) {
        const auto fo = fv.toObject();
        fields.push_back(
            AttachmentField{
                .title = fo.value("title").toString(),
                .value = MrkdwnParser::parse(fo.value("value").toString()),
            }
        );
    }
    // Shared-message unfurl: the attachment describes a quoted Slack message, so
    // its own `files` array (the quoted message's uploads) matters too.
    const bool        isMsgUnfurl = o.value("is_msg_unfurl").toBool();
    std::vector<File> files;
    if (isMsgUnfurl)
        for (const auto &fv : o.value("files").toArray())
            files.push_back(toFile(fv.toObject()));

    return Attachment{
        .fallback      = o.value("fallback").toString(),
        .color         = o.value("color").toString(),
        .pretext       = o.value("pretext").toString(),
        .authorName    = o.value("author_name").toString(),
        .title         = o.value("title").toString(),
        .titleLink     = o.value("title_link").toString(),
        .text          = MrkdwnParser::parse(o.value("text").toString()),
        .imageUrl      = o.value("image_url").toString(),
        .thumbUrl      = o.value("thumb_url").toString(),
        .faviconUrl    = o.value("service_icon").toString(),
        .footer        = o.value("footer").toString(),
        .imageWidth    = o.value("image_width").toInt(),
        .imageHeight   = o.value("image_height").toInt(),
        .thumbWidth    = o.value("thumb_width").toInt(),
        .thumbHeight   = o.value("thumb_height").toInt(),
        .fields        = std::move(fields),
        .blocks        = std::move(blocks),
        .buttons       = std::move(buttons),
        // `ts` on a message unfurl is the QUOTED message's ts (the unfurling
        // message has its own) — the card shows it as the quote's time.
        .isMsgUnfurl   = isMsgUnfurl,
        .authorIcon    = o.value("author_icon").toString(),
        .authorSubname = o.value("author_subname").toString(),
        .channelId     = o.value("channel_id").toString(),
        .msgDate       = isMsgUnfurl ? decimalTsToMicros(o.value("ts").toString()) : 0,
        .files         = std::move(files),
    };
}

SelfPresence toSelfPresence(const QJsonObject &o) {
    return SelfPresence{
        .loaded          = true,
        .active          = o.value("presence").toString() == "active",
        .online          = o.value("online").toBool(false),
        .autoAway        = o.value("auto_away").toBool(false),
        .manualAway      = o.value("manual_away").toBool(false),
        .connectionCount = o.value("connection_count").toInt(0),
    };
}

SearchResult toSearchResult(const QJsonObject &o) {
    return SearchResult{
        .conv     = ConversationId{o.value("channel").toObject().value("id").toString()},
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
        botName               = msg.value("username").toString();
        const auto botProfile = msg.value("bot_profile").toObject();
        if (botName.isEmpty())
            botName = botProfile.value("name").toString();
        const auto icons = botProfile.value("icons").toObject();
        botAvatarUrl =
            icons.value("image_72")
                .toString(icons.value("image_48").toString(icons.value("image_36").toString()));
        if (botAvatarUrl.isEmpty())
            botAvatarUrl = msg.value("icon_url").toString();
    }

    const QString ts = msg.value("ts").toString();
    Message       m{
              .ts         = ts,
              .date       = decimalTsToMicros(ts), // epoch micros for sort + display
              .threadRoot = msg.contains("thread_ts") && msg.value("thread_ts") != msg.value("ts")
                                ? std::optional<Ts>(msg.value("thread_ts").toString())
                                : std::nullopt,
              .replyCount = msg.value("reply_count").toInt(),
              .replyUsers =
            [&] {
                std::vector<UserId> v;
                for (const auto &u : msg.value("reply_users").toArray())
                    v.push_back(UserId{u.toString()});
                return v;
            }(),
              .latestReply  = msg.contains("latest_reply")
                                  ? std::optional<Ts>(msg.value("latest_reply").toString())
                                  : std::nullopt,
        // Author of the thread root, present on reply events; drives the
        // "reply to a thread I started" notification (isFollowedThreadReply).
              .parentUserId = UserId{msg.value("parent_user_id").toString()},
              .author       = UserId{msg.value("user").toString(msg.value("bot_id").toString())},
              .botName      = botName,
              .botAvatarUrl = botAvatarUrl,
              .text         = MrkdwnParser::parse(msg.value("text").toString()),
              .rawText      = msg.value("text").toString(),
              .reactions    = parseReactions(msg.value("reactions").toArray()),
              .edited       = msg.contains("edited"),
              .subtype = msg.contains("subtype") ? std::optional<QString>(msg.value("subtype").toString())
                                                 : std::nullopt,
              .files       = std::move(files),
              .blocks      = std::move(blocks),
              .attachments = std::move(attachments),
    };
    presentHuddleThread(m);
    return m;
}

std::vector<User> toUsers(const QJsonArray &a) {
    std::vector<User> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto u = toUser(v.toObject());
        if (!u.id.value.isEmpty())
            out.push_back(std::move(u));
    }
    return out;
}

std::vector<Conversation> toConversations(const QJsonArray &a) {
    std::vector<Conversation> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto c = toConversation(v.toObject());
        if (!c.id.value.isEmpty())
            out.push_back(std::move(c));
    }
    return out;
}

std::vector<ConvCounts> toConvCounts(const QJsonObject &resp) {
    // client.counts groups conversations by kind into three arrays of identically
    // shaped entries. Unlike conversations.list it reports `latest` for CHANNELS
    // too, which is the whole reason we call it.
    std::vector<ConvCounts> out;
    for (const auto *key : {"channels", "mpims", "ims"}) {
        const auto arr = resp.value(QLatin1String(key)).toArray();
        out.reserve(out.size() + arr.size());
        for (const auto v : arr) {
            const auto o  = v.toObject();
            const auto id = o.value("id").toString();
            if (id.isEmpty())
                continue;
            // The endpoint reports mentions exactly; for everything else it gives
            // only the boolean has_unreads (a channel's plain-traffic count is not
            // exposed). Both DM shapes appear in the wild: `dm_count` on some
            // responses, has_unreads on all of them. A 1 stands in for "some" —
            // Session only ever compares counts for movement, never displays them.
            const int mentions = o.value("mention_count").toInt();
            const int dmCount  = o.value("dm_count").toInt();
            const int unread =
                std::max({mentions, dmCount, o.value("has_unreads").toBool() ? 1 : 0});
            out.push_back(
                ConvCounts{
                    .id           = ConversationId{id},
                    .latestTs     = o.value("latest").toString(),
                    .lastRead     = o.value("last_read").toString(),
                    .unread       = unread,
                    .mentionCount = mentions,
                }
            );
        }
    }
    return out;
}

ThreadsViewPage toThreadsViewPage(const QJsonObject &resp) {
    ThreadsViewPage page;
    page.totalUnreadReplies = resp.value("total_unread_replies").toInt();
    page.hasMore            = resp.value("has_more").toBool();
    // Continuation cursor: the top-level max_ts, passed back as current_ts.
    // There is no response_metadata.next_cursor on this endpoint.
    page.nextCursor         = resp.value("max_ts").toString();
    const auto threads      = resp.value("threads").toArray();
    page.threads.reserve(threads.size());
    for (const auto v : threads) {
        const auto t       = v.toObject();
        const auto rootObj = t.value("root_msg").toObject();
        // Only currently-subscribed threads have been observed, but filter
        // defensively so a future inactive entry can't pollute the view.
        if (rootObj.contains("subscribed") && !rootObj.value("subscribed").toBool())
            continue;
        ThreadOverview item;
        // root_msg is a complete parent message and — unlike history messages —
        // carries the channel it lives in.
        item.conv     = ConversationId{rootObj.value("channel").toString()};
        item.root     = toMessage(rootObj);
        item.lastRead = rootObj.value("last_read").toString();
        if (item.conv.value.isEmpty() || item.root.ts.isEmpty())
            continue;
        // The replies key varies by read state: `latest_replies` on a fully-read
        // thread, `unread_replies` (with NO latest_replies at all) when there
        // are replies past last_read. Merge both — a partially-read thread
        // plausibly carries both — and dedup by ts.
        for (const auto key : {"latest_replies", "unread_replies"}) {
            const auto replies = t.value(QLatin1String(key)).toArray();
            for (const auto rv : replies) {
                auto m = toMessage(rv.toObject());
                if (!m.ts.isEmpty())
                    item.latestReplies.push_back(std::move(m));
            }
        }
        // Oldest-first for display, whatever order the server sent.
        std::sort(
            item.latestReplies.begin(),
            item.latestReplies.end(),
            [](const Message &a, const Message &b) { return a.date < b.date; }
        );
        item.latestReplies.erase(
            std::unique(
                item.latestReplies.begin(),
                item.latestReplies.end(),
                [](const Message &a, const Message &b) { return a.ts == b.ts; }
            ),
            item.latestReplies.end()
        );
        page.threads.push_back(std::move(item));
    }
    return page;
}

std::vector<MessageReminder> toMessageReminders(const QJsonObject &resp) {
    std::vector<MessageReminder> out;
    const auto                   items = resp.value("saved_items").toArray();
    out.reserve(items.size());
    for (const auto v : items) {
        const auto it = v.toObject();
        // Saved items also cover plain "save for later" (no due date) and files;
        // a reminder is a message item with a due date that is still pending.
        if (it.value("item_type").toString() != QLatin1String("message"))
            continue;
        if (it.value("state").toString() == QLatin1String("completed") ||
            it.value("is_archived").toBool())
            continue;
        MessageReminder r;
        r.conv  = ConversationId{it.value("item_id").toString()};
        r.ts    = it.value("ts").toString();
        // date_due can exceed int range (Unix seconds) — read as double.
        r.dueAt = static_cast<qint64>(it.value("date_due").toDouble());
        if (r.conv.value.isEmpty() || r.ts.isEmpty() || r.dueAt <= 0)
            continue;
        out.push_back(std::move(r));
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
        if (!r.conv.value.isEmpty())
            out.push_back(std::move(r));
    }
    return out;
}

static SlashCommand toSlashCommand(const QJsonObject &o) {
    SlashCommand c;
    c.name = o.value("name").toString();
    if (c.name.startsWith('/'))
        c.name = c.name.mid(1);
    c.desc  = o.value("desc").toString();
    c.usage = o.value("usage").toString();
    if (o.value("type").toString() == QLatin1String("app"))
        c.appId = o.value("app").toString();
    // App identity for the command palette. The endpoint is undocumented and
    // these keys are best-effort; the palette falls back gracefully when absent.
    c.appName = o.value("app_name").toString();
    c.iconUrl = o.value("icon_url").toString();
    if (c.iconUrl.isEmpty()) {
        const QJsonObject icons = o.value("icons").toObject();
        c.iconUrl               = icons.value("image_48").toString();
        if (c.iconUrl.isEmpty())
            c.iconUrl = icons.value("image_36").toString();
    }
    return c;
}

std::vector<SlashCommand> toSlashCommands(const QJsonValue &v) {
    std::vector<SlashCommand> out;
    const auto                add = [&out](const QJsonObject &o) {
        auto c = toSlashCommand(o);
        if (!c.name.isEmpty())
            out.push_back(std::move(c));
    };
    if (v.isArray()) {
        const auto a = v.toArray();
        out.reserve(a.size());
        for (auto e : a)
            add(e.toObject());
    } else if (v.isObject()) {
        const auto o = v.toObject();
        out.reserve(o.size());
        for (auto it = o.begin(); it != o.end(); ++it)
            add(it.value().toObject());
    }
    return out;
}

} // namespace JsonMappers
} // namespace slack
