// Slack Web API JSON -> the model (see slack_json.h). A message keeps raw
// mrkdwn, so Block Kit blocks that say more than its text become the
// equivalent mrkdwn once, at mapping time.
#include "app/slack/slack_json.h"

#include "app/mrkdwn/mrkdwn.h"
#include "base/i18n.h"
#include "base/str.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace slack::mapjson {

namespace {

using json::Value;
using model::File;

// The string fields a mapper copies verbatim, as data (json.h).
using json::owned;
using json::readStrings;
using json::StrField;

std::string trimmed(const Value &v) {
    return std::string(str::trim(v.str()));
}

// ":palm_tree:" → "palm_tree"; ":baby::skin-tone-3:" → "baby".
std::string statusEmoji(std::string_view e) {
    if (!e.empty() && e.front() == ':')
        e.remove_prefix(1);
    if (!e.empty() && e.back() == ':')
        e.remove_suffix(1);
    if (const size_t sep = e.find("::"); sep != std::string_view::npos)
        e = e.substr(0, sep);
    return std::string(e);
}

// "latest" is an object on channels ({"ts": …}) and a bare ts on some IMs.
model::Ts latestTs(const Value &c) {
    const Value v = c["latest"];
    return model::parseTs(v.isObject() ? v["ts"].str() : v.str());
}

// ── Block Kit → mrkdwn ──────────────────────────────────────────────────────

// Wraps `s` in an emphasis mark, keeping edge whitespace outside it: Slack
// only honours "*x*" when the marks touch the text.
void wrapMark(std::string &out, std::string_view s, char mark) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\n'))
        --e;
    out.append(s.substr(0, b));
    if (e > b) {
        out += mark;
        out.append(s.substr(b, e - b));
        out += mark;
    }
    out.append(s.substr(e));
}

// One rich_text inline element, as mrkdwn tokens.
void richInline(std::string &out, const Value &el) {
    const std::string_view type = el["type"].str();
    if (type == "text") {
        // Block text is raw; the mrkdwn it becomes escapes & < > like `text`.
        const std::string t     = mrkdwn::escapeEntities(el["text"].str());
        const Value       style = el["style"];
        // One mark, the first that matches in this precedence.
        const char        mark  = style["code"].boolean()     ? '`'
                                  : style["bold"].boolean()   ? '*'
                                  : style["italic"].boolean() ? '_'
                                  : style["strike"].boolean() ? '~'
                                                              : 0;
        if (mark)
            wrapMark(out, t, mark);
        else
            out.append(t);
    } else if (type == "user") {
        out.append(str::concat({"<@", el["user_id"].str(), ">"}));
    } else if (type == "channel") {
        out.append(str::concat({"<#", el["channel_id"].str(), ">"}));
    } else if (type == "usergroup") {
        out.append(str::concat({"<!subteam^", el["usergroup_id"].str(), ">"}));
    } else if (type == "emoji") {
        out.append(str::concat({":", el["name"].str(), ":"}));
        if (const int64_t tone = el["skin_tone"].integer(); tone >= 2 && tone <= 6)
            out.append(str::concat({":skin-tone-", str::number(tone), ":"}));
    } else if (type == "link") {
        const std::string_view url   = el["url"].str();
        const std::string      label = mrkdwn::escapeEntities(el["text"].str());
        out.append(
            label.empty() || label == url ? str::concat({"<", url, ">"})
                                          : str::concat({"<", url, "|", label, ">"})
        );
    } else if (type == "broadcast") {
        out.append(str::concat({"<!", el["range"].str(), ">"}));
    } else if (type == "date") {
        out.append(el["fallback"].str());
    } else if (type == "message_mention") {
        out.append(str::concat({"<", el["url"].str(), ">"}));
    } else {
        out.append(el["text"].str()); // unknown inline type: its text, if any
    }
}

void richInlines(std::string &out, const Value &elements) {
    for (const Value el : elements)
        richInline(out, el);
}

void trimTrailingNewlines(std::string &s) {
    while (!s.empty() && s.back() == '\n')
        s.pop_back();
}

// A rich_text block: sections, lists, code, quotes.
void richText(std::string &out, const Value &block) {
    for (const Value section : block["elements"]) {
        const std::string_view stype = section["type"].str();
        if (stype == "rich_text_preformatted") {
            std::string body;
            richInlines(body, section["elements"]);
            out.append(str::concat({"```", body, "```\n"}));
        } else if (stype == "rich_text_quote") {
            std::string body;
            richInlines(body, section["elements"]);
            trimTrailingNewlines(body);
            out += "> ";
            for (char ch : body) {
                out += ch;
                if (ch == '\n')
                    out += "> ";
            }
            out += '\n';
        } else if (stype == "rich_text_list") {
            const bool        ordered = section["style"].str() == "ordered";
            int64_t           n       = section["offset"].integer();
            const std::string indent(
                size_t(std::clamp<int64_t>(section["indent"].integer(), 0, 3)) * 4, ' '
            );
            for (const Value item : section["elements"]) {
                out += indent;
                out.append(ordered ? str::concat({str::number(++n), ". "}) : std::string("• "));
                richInlines(out, item["elements"]);
                out += '\n';
            }
        } else {
            richInlines(out, section["elements"]);
            if (out.empty() || out.back() != '\n')
                out += '\n';
        }
    }
    trimTrailingNewlines(out);
}

// A text object: mrkdwn as is; plain_text escaped so it stays literal.
void textObject(std::string &out, const Value &o) {
    const std::string_view t = o["text"].str();
    if (o["type"].str() == "mrkdwn")
        out.append(t);
    else // plain_text: escaped as Slack escapes stored text
        out += mrkdwn::escapeEntities(t);
}

// Block types whose content `text` already mirrors (most rich_text blocks
// mirror the fallback text) or that carry nothing renderable here.
bool mirrorsText(std::string_view type) {
    return type == "rich_text" || type == "divider" || type == "actions" || type == "input";
}

} // namespace

namespace {

// One block's text as mrkdwn (rich_text, section, header, context, table);
// "" for blocks without text.
std::string blockMrkdwn(const Value &b) {
    const std::string_view type = b["type"].str();
    std::string            part;
    if (type == "rich_text") {
        richText(part, b);
    } else if (type == "header") {
        std::string h;
        textObject(h, b["text"]);
        wrapMark(part, h, '*');
    } else if (type == "section") {
        if (b.has("text"))
            textObject(part, b["text"]);
        for (const Value f : b["fields"]) {
            std::string ft;
            textObject(ft, f);
            if (ft.empty())
                continue;
            if (!part.empty())
                part += '\n';
            part += ft;
        }
    } else if (type == "context") {
        for (const Value el : b["elements"]) {
            const std::string_view et = el["type"].str();
            if (et != "mrkdwn" && et != "plain_text")
                continue;
            std::string t;
            textObject(t, el);
            if (t.empty())
                continue;
            if (!part.empty())
                part += "  ";
            part += t;
        }
    } else if (type == "table") {
        for (const Value row : b["rows"]) {
            std::string line;
            bool        first = true;
            for (const Value cell : row) {
                if (!first)
                    line += " | ";
                first = false;
                if (cell["type"].str() == "rich_text")
                    richText(line, cell);
                else
                    line.append(cell["text"].str());
            }
            if (!part.empty())
                part += '\n';
            part += line;
        }
    }
    return part;
}

} // namespace

std::string blocksToMrkdwn(const json::Value &blocks) {
    std::string out;
    for (const Value b : blocks) {
        std::string part = blockMrkdwn(b);
        if (part.empty())
            continue;
        if (!out.empty())
            out += '\n';
        out += part;
    }
    return out;
}

// What blocksToMrkdwn makes of the blocks toBlocks kept (it keeps every
// block with text): the text again without rendering each block twice.
std::string structureMrkdwn(const std::vector<model::Block> &blocks) {
    using K = model::Block::Kind;
    std::string out;
    for (const model::Block &b : blocks) {
        std::string part;
        if (b.kind == K::Text) {
            part = b.text;
        } else if (b.kind == K::Header) {
            wrapMark(part, b.text, '*');
        } else if (b.kind == K::Table) {
            for (const std::vector<std::string> &row : b.rows) {
                std::string line;
                for (size_t i = 0; i < row.size(); ++i) {
                    if (i)
                        line += " | ";
                    line += row[i];
                }
                if (!part.empty())
                    part += '\n';
                part += line;
            }
        }
        if (part.empty())
            continue;
        if (!out.empty())
            out += '\n';
        out += part;
    }
    return out;
}

std::vector<model::Block> toBlocks(const json::Value &blocks) {
    // Only where the shape matters: a header, a divider, an image or a table.
    bool structural = false;
    for (const Value b : blocks) {
        const std::string_view t = b["type"].str();
        structural = structural || t == "header" || t == "divider" || t == "image" || t == "table";
    }
    std::vector<model::Block> out;
    if (!structural)
        return out;
    using K = model::Block::Kind;
    for (const Value b : blocks) {
        const std::string_view type = b["type"].str();
        model::Block           x;
        if (type == "divider") {
            x.kind = K::Divider;
        } else if (type == "header") {
            x.kind = K::Header;
            textObject(x.text, b["text"]);
        } else if (type == "image") {
            x.kind   = K::Image;
            x.image  = owned(b["image_url"]);
            x.alt    = owned(b["alt_text"]);
            x.text   = owned(b["title"]["text"]);
            x.width  = int32_t(b["image_width"].integer());
            x.height = int32_t(b["image_height"].integer());
            if (x.image.empty() && x.alt.empty())
                continue;
        } else if (type == "table") {
            // Row 0 is the header (Slack pads it with nulls).
            x.kind = K::Table;
            for (const Value row : b["rows"]) {
                std::vector<std::string> cells;
                for (const Value cell : row) {
                    std::string t;
                    if (cell["type"].str() == "rich_text")
                        richText(t, cell);
                    else
                        t = owned(cell["text"]);
                    cells.push_back(std::move(t));
                }
                x.rows.push_back(std::move(cells));
            }
            if (x.rows.empty())
                continue;
        } else {
            x.text = blockMrkdwn(b);
            if (x.text.empty())
                continue;
        }
        out.push_back(std::move(x));
    }
    return out;
}

bool isSlackSystemUser(std::string_view id) {
    return id == "USLACKBOT" || id == "USLACK";
}

model::User toUser(const json::Value &o) {
    const Value                        p      = o["profile"];
    static const StrField<model::User> kTop[] = {
        {"id", &model::User::id}, {"name", &model::User::name}
    };
    static const StrField<model::User> kProfile[] = {
        {"title", &model::User::title},
        {"email", &model::User::email},
        {"image_72", &model::User::avatar},
        {"status_text", &model::User::statusText},
    };
    model::User u;
    readStrings(o, u, kTop);
    readStrings(p, u, kProfile);
    // display_name / real_name are often "" rather than absent; the Store
    // makes displayName from them (Store::realNames).
    u.realName    = trimmed(p["real_name"]);
    u.profileName = trimmed(p["display_name"]);
    u.resolveName(true);
    u.statusEmoji = statusEmoji(p["status_emoji"].str());
    u.hasTz       = o.has("tz_offset");
    u.tzOffset    = int32_t(o["tz_offset"].integer());
    // Slackbot and "Slack" (the billing/trial notifier) report is_bot=false
    // but are apps: Agents & apps, no presence.
    u.bot         = o["is_bot"].boolean() || isSlackSystemUser(u.id);
    u.admin       = o["is_admin"].boolean() || o["is_owner"].boolean();
    u.owner       = o["is_owner"].boolean() || o["is_primary_owner"].boolean();
    u.deleted     = o["deleted"].boolean();
    u.stranger    = o["is_stranger"].boolean();
    return u;
}

int namesOverride(const json::Value &prefs) {
    const Value   v = prefs["display_real_names_override"];
    // A number; a string where a pref travels as text.
    const int64_t n =
        v.isString() ? std::strtoll(std::string(v.str()).c_str(), nullptr, 10) : v.integer(kNoPref);
    return n == 1 || n == -1 || n == 0 ? int(n) : kNoPref;
}

int namesDefault(const json::Value &prefs) {
    const Value v = prefs["display_real_names"];
    return v.isBool() ? int(v.boolean()) : v.isNumber() ? int(v.integer() != 0) : kNoPref;
}

int realNamesFrom(int override, int teamDefault) {
    if (override == 1 || override == -1)
        return override == 1;
    return override == 0 ? teamDefault : kNoPref;
}

model::Conversation toConversation(const json::Value &o, model::Store &store) {
    model::Conversation c;
    c.id   = owned(o["id"]);
    c.kind = o["is_im"].boolean()        ? model::ConvKind::Dm
             : o["is_mpim"].boolean()    ? model::ConvKind::Group
             : o["is_private"].boolean() ? model::ConvKind::Private
                                         : model::ConvKind::Channel;
    // An IM has no name: its peer's id stands in (the title comes from dmUser).
    c.name = owned(o.has("name") ? o["name"] : o["user"]);
    if (c.kind == model::ConvKind::Dm)
        c.dmUser = store.internUser(o["user"].str());
    if (c.kind == model::ConvKind::Group)
        for (const Value m : o["members"])
            c.members.push_back(store.internUser(m.str()));
    const std::string topic = trimmed(o["topic"]["value"]);
    c.topic                 = !topic.empty() ? topic : trimmed(o["purpose"]["value"]);
    c.member                = o["is_member"].boolean(true);
    c.memberCount           = uint32_t(o["num_members"].integer());
    c.lastRead              = model::parseTs(o["last_read"].str());
    c.latest                = latestTs(o);
    const int64_t unread    = o["unread_count"].integer();
    c.unread   = unread > 0 ? uint32_t(unread) : (c.latest && c.lastRead && c.latest > c.lastRead);
    c.mentions = uint32_t(o["mention_count"].integer());
    c.muted    = o["is_muted"].boolean();
    c.starred  = o["is_starred"].boolean();
    const std::string_view pref = o["notification_preference"].str();
    c.notify                    = pref == "everything" ? model::NotifyLevel::All
                                  : pref == "mentions" ? model::NotifyLevel::Mentions
                                  : pref == "nothing"  ? model::NotifyLevel::Nothing
                                                       : model::NotifyLevel::Default;
    // Channel canvas: properties.canvas.file_id, or (free teams) a "canvas"
    // tab. The title lives on the file, not here (fetched on open).
    const Value props           = o["properties"];
    c.canvasId                  = std::string(props["canvas"]["file_id"].str());
    for (const Value tab : props["tabs"])
        if (c.canvasId.empty() && tab["type"].str() == "canvas")
            c.canvasId = std::string(tab["data"]["file_id"].str());
    if (!c.canvasId.empty())
        c.canvasTitle = "Canvas";
    // conversations.info's room (the list has none: Read::carryLocal keeps it).
    if (HuddleRoom h; toHuddleRoom(o["room"], store, h)) {
        c.huddleActive       = h.active;
        c.huddleLink         = std::move(h.link);
        c.huddleParticipants = std::move(h.participants);
    }
    return c;
}

namespace {

// has_ended, or a date_end (a string read as a number would be 0 and the
// huddle live forever).
bool roomEnded(const Value &room) {
    return room["has_ended"].boolean() || epochSecs(room["date_end"]) != 0;
}

// A huddle_thread message's attendees and times.
model::Huddle huddleSummary(const Value &room, model::Store &store) {
    model::Huddle h;
    h.ended    = roomEnded(room);
    h.startSec = epochSecs(room["date_start"]);
    h.endSec   = h.ended ? epochSecs(room["date_end"]) : 0;
    for (const Value u : room[h.ended ? "participant_history" : "participants"])
        if (!u.str().empty())
            h.attendees.push_back(store.internUser(u.str()));
    return h;
}

// A Block Kit button; blockId is its block's.
model::Button toButton(const Value &el, std::string_view blockId) {
    model::Button b;
    const Value   t              = el["text"];
    b.label                      = owned(t.isObject() ? t["text"] : t);
    b.id                         = owned(el["action_id"]);
    b.url                        = owned(el["url"]);
    b.value                      = owned(el["value"]);
    b.blockId                    = std::string(blockId);
    const std::string_view style = el["style"].str();
    b.style                      = style == "primary"  ? model::Button::Style::Primary
                                   : style == "danger" ? model::Button::Style::Danger
                                                       : model::Button::Style::Default;
    return b;
}

// The buttons of a blocks list: an actions block's, a section's accessory.
void blockButtons(const Value &blocks, std::vector<model::Button> &out) {
    for (const Value b : blocks) {
        const std::string_view type = b["type"].str(), id = b["block_id"].str();
        if (type == "actions") {
            for (const Value el : b["elements"])
                if (el["type"].str() == "button")
                    out.push_back(toButton(el, id));
        } else if (
            (type == "section" || type == "header") && b["accessory"]["type"].str() == "button"
        ) {
            out.push_back(toButton(b["accessory"], id));
        }
    }
}

// An attachment's buttons (its blocks', its legacy actions', which have no
// action id: not pressable from here), marked as the `index`th's.
void attachmentButtons(const Value &a, int32_t index, std::vector<model::Button> &out) {
    const size_t first = out.size();
    blockButtons(a["blocks"], out);
    for (const Value act : a["actions"])
        if (act["type"].str() == "button")
            out.push_back(toButton(act, {}));
    for (size_t i = first; i < out.size(); ++i)
        out[i].attachment = index;
}

} // namespace

bool toHuddleRoom(const json::Value &room, model::Store &store, HuddleRoom &out) {
    if (!room.isObject() || room["call_family"].str() != "huddle")
        return false;
    out        = {};
    out.active = !roomEnded(room);
    out.link   = owned(room["huddle_link"]);
    for (const Value u : room["participants"])
        if (!u.str().empty())
            out.participants.push_back(store.internUser(u.str()));
    if (out.participants.empty() && !room["created_by"].str().empty())
        out.participants.push_back(store.internUser(room["created_by"].str()));
    return true;
}

bool newestHuddleRoom(const json::Value &messages, model::Store &store, HuddleRoom &out) {
    model::Ts newest = 0;
    Value     room;
    for (const Value m : messages)
        if (m["subtype"].str() == "huddle_thread")
            if (const model::Ts ts = model::parseTs(m["ts"].str()); ts > newest) {
                newest = ts;
                room   = m["room"];
            }
    return newest && toHuddleRoom(room, store, out);
}

namespace {

// An attachment's "ts": a string or a number of seconds.
model::Ts attachmentTs(const Value &v) {
    if (v.isNumber())
        return model::Ts(std::llround(v.number() * 1e6));
    return model::parseTs(v.str());
}

// The image variant a message shows: picked from the thumb ladder by the
// physical preview size (360 px wide at most, so 720 covers 2x
// screens); the full file only when Slack made no thumb (or for GIFs,
// whose thumbs are a still first frame).
const char *const kThumbs[] = {
    "thumb_720", "thumb_800", "thumb_960", "thumb_1024", "thumb_480", "thumb_360", "thumb_160"
};

File toFile(const Value &o) {
    // url_private is also the AAC transcode of a voice clip (url_private_download
    // is the original), which is what the player wants.
    static const StrField<File> kStrings[] = {
        {"id", &File::id},
        {"name", &File::name},
        {"mimetype", &File::mime},
        {"pretty_type", &File::prettyType},
        {"subtype", &File::subtype},
        {"url_private", &File::path},
        {"permalink", &File::permalink},
    };
    File f;
    readStrings(o, f, kStrings);
    if (o["transcription"]["status"].str() == "complete") {
        f.transcript    = owned(o["transcription"]["preview"]["content"]);
        f.transcriptVtt = owned(o["vtt"]);
    }
    f.size       = o["size"].integer();
    f.width      = int32_t(o["original_w"].integer(o["thumb_360_w"].integer()));
    f.height     = int32_t(o["original_h"].integer(o["thumb_360_h"].integer()));
    f.durationMs = o["duration_ms"].integer();
    // The original: url_private_download (a voice clip's own bytes, not the
    // transcode), else url_private.
    f.original   = owned(o["url_private_download"]);
    if (f.original.empty())
        f.original = f.path;
    if (str::startsWith(f.mime, "image/") && f.mime != "image/gif")
        for (const char *key : kThumbs)
            if (const std::string_view t = o[key].str(); !t.empty()) {
                f.path = std::string(t);
                break;
            }
    if (f.width == 0 && o.has("thumb_pdf")) {
        // A PDF's preview: the server-rendered first page.
        f.thumb  = owned(o["thumb_pdf"]);
        f.width  = int32_t(o["thumb_pdf_w"].integer());
        f.height = int32_t(o["thumb_pdf_h"].integer());
    }
    if (f.isCanvas())
        f.title = mrkdwn::decodeEntities(o["title"].str()); // entity-escaped
    if (f.original == f.path)
        f.original.clear(); // source() falls back to path
    return f;
}

std::string hexColor(std::string_view c) {
    if (c.empty() || c.front() == '#')
        return std::string(c);
    const bool hex = std::all_of(c.begin(), c.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
    return hex ? str::concat({"#", c}) : std::string(c); // "good" / "danger" stay names
}

model::Attachment toAttachment(const Value &o) {
    using A                             = model::Attachment;
    static const StrField<A> kStrings[] = {
        {"pretext", &A::pretext},
        {"author_name", &A::author},
        {"title", &A::title},
        {"title_link", &A::link},
        {"text", &A::text},
        {"service_name", &A::service},
        {"service_icon", &A::favicon},
        {"footer", &A::footer},
    };
    model::Attachment a;
    readStrings(o, a, kStrings);
    a.color = hexColor(o["color"].str());
    if (a.link.empty())
        a.link = owned(o.has("original_url") ? o["original_url"] : o["from_url"]);
    if (o.has("image_url")) {
        a.image       = owned(o["image_url"]);
        a.imageWidth  = int32_t(o["image_width"].integer());
        a.imageHeight = int32_t(o["image_height"].integer());
    } else {
        a.image       = owned(o["thumb_url"]);
        a.imageWidth  = int32_t(o["thumb_width"].integer());
        a.imageHeight = int32_t(o["thumb_height"].integer());
    }
    for (const Value f : o["fields"])
        a.fields.push_back({owned(f["title"]), owned(f["value"])});
    a.id        = int32_t(o["id"].integer());
    a.msgUnfurl = o["is_msg_unfurl"].boolean();
    if (a.msgUnfurl) {
        // A shared message: who wrote it, where and when, and its files.
        a.authorIcon = owned(o["author_icon"]);
        a.channel    = owned(o["channel_id"]);
        a.ts         = attachmentTs(o["ts"]);
        a.app        = !o["author_subname"].str().empty();
        for (const Value f : o["files"])
            a.files.push_back(toFile(f));
    }
    a.blocks      = toBlocks(o["blocks"]);
    // Generated previews: app cards, shared-message links, URL unfurls. Bot
    // attachments without unfurl metadata stay standalone content.
    a.linkPreview = o["is_msg_unfurl"].boolean() || o["is_app_unfurl"].boolean() ||
                    o["is_unfurl"].boolean() || !o["original_url"].str().empty() ||
                    !o["from_url"].str().empty();
    // A blocks-only attachment (its blocks shown as structure, or as
    // their mrkdwn), else the fallback when the card would otherwise be empty.
    if (a.text.empty() && a.blocks.empty())
        a.text = blocksToMrkdwn(o["blocks"]);
    // A card of buttons alone shows them, not the fallback ("[no preview
    // available]").
    std::vector<model::Button> buttons;
    attachmentButtons(o, 0, buttons);
    if (a.text.empty() && a.title.empty() && a.pretext.empty() && a.fields.empty() &&
        a.image.empty() && a.blocks.empty() && buttons.empty())
        a.text = owned(o["fallback"]);
    return a;
}

// message_mention: a link to another message names its author, which
// the permalink in `text` doesn't; the Store keeps it for the link chip.
void noteLinkedAuthors(const Value &v, model::Store &store, int depth = 0) {
    if (depth > 6)
        return;
    if (v["type"].str() == "message_mention") {
        const std::string_view author = v["author_id"].str();
        if (!author.empty())
            store.setLinkedAuthor(
                v["channel_id"].str(), v["message_ts"].str(), store.internUser(author)
            );
        return;
    }
    for (const Value el : v["elements"])
        noteLinkedAuthors(el, store, depth + 1);
}

} // namespace

model::Message toMessage(const json::Value &in, model::Store &store) {
    const Value    o = in["subtype"].str() == "message_changed" ? in["message"] : in;
    model::Message m;
    m.ts          = model::parseTs(o["ts"].str());
    m.threadTs    = model::parseTs(o["thread_ts"].str());
    m.latestReply = model::parseTs(o["latest_reply"].str());
    m.replyCount  = uint32_t(o["reply_count"].integer());
    for (const Value u : o["reply_users"])
        m.replyUsers.push_back(store.internUser(u.str()));
    // A bot post without a user is authored by its bot id.
    m.user   = store.internUser(o.has("user") ? o["user"].str() : o["bot_id"].str());
    m.edited = o.has("edited");
    m.pinned = o["pinned_to"].size() > 0;
    if (const std::string_view by = o["pinned_info"]["pinned_by"].str(); m.pinned && !by.empty())
        m.pinnedBy = store.internUser(by);
    for (const Value r : o["reactions"]) {
        model::Reaction x;
        x.name  = owned(r["name"]);
        x.count = uint32_t(r["count"].integer());
        for (const Value u : r["users"])
            x.users.push_back(store.internUser(u.str()));
        m.reactions.push_back(std::move(x));
    }

    // Text: the blocks when there are any, falling back to `text` only when
    // they produce nothing. rich_text blocks mirror `text`, so the
    // raw text is kept unless some block says more (or the text is empty).
    const Value blocks = o["blocks"];
    bool        richer = false, rich = false;
    for (const Value b : blocks) {
        richer = richer || !mirrorsText(b["type"].str());
        if (b["type"].str() == "rich_text") {
            rich = true;
            noteLinkedAuthors(b, store);
        }
    }
    std::vector<model::Block> structure = toBlocks(blocks);
    m.text                              = owned(o["text"]);
    if (richer || m.text.empty()) {
        std::string fromBlocks =
            structure.empty() ? blocksToMrkdwn(blocks) : structureMrkdwn(structure);
        if (!fromBlocks.empty())
            m.text = std::move(fromBlocks);
    } else if (rich) {
        // Some clients send a flattened `text` (line breaks turned into
        // spaces, list items run together); the blocks keep the lines.
        std::string fromBlocks = blocksToMrkdwn(blocks);
        if (std::count(fromBlocks.begin(), fromBlocks.end(), '\n') >
            std::count(m.text.begin(), m.text.end(), '\n'))
            m.text = std::move(fromBlocks);
    }

    const std::string_view subtype = o["subtype"].str();
    const bool             bot     = o.has("bot_id");
    if (!subtype.empty() || bot || o["files"].size() || o["attachments"].size() ||
        !structure.empty()) {
        model::MessageExtras &x = m.extras();
        x.subtype               = std::string(subtype);
        if (bot) {
            // username (a custom per-post name) > bot_profile.name; icons
            // image_72 > 48 > 36 > the post's icon_url.
            const Value profile = o["bot_profile"];
            x.botName           = owned(o["username"]);
            if (x.botName.empty())
                x.botName = owned(profile["name"]);
            x.botAvatar = firstIcon(profile["icons"]);
            if (x.botAvatar.empty())
                x.botAvatar = owned(o["icon_url"]);
        }
        for (const Value f : o["files"])
            x.files.push_back(toFile(f));
        for (const Value a : o["attachments"])
            x.attachments.push_back(toAttachment(a));
        // Headers, dividers, images and tables are drawn as blocks (images
        // have no text form at all); `text` stays the plain fallback.
        x.blocks = std::move(structure);
        x.botId  = owned(o["bot_id"]);
        // Buttons: the message's blocks, then each attachment's.
        blockButtons(blocks, x.buttons);
        int32_t n = 0;
        for (const Value a : o["attachments"])
            attachmentButtons(a, ++n, x.buttons);
        // A huddle thread: no Slackbot author, the frozen "A
        // huddle started" block dropped, the room's summary kept; the name
        // line and the text say whether it is still going.
        if (subtype == "huddle_thread") {
            x.huddle = huddleSummary(o["room"], store);
            m.user   = model::kNoUser;
            x.botName =
                x.huddle.ended ? i18n::tr("A huddle happened") : i18n::tr("A huddle started");
            x.botAvatar = {};
            x.blocks.clear();
            m.text = x.botName;
        }
    }
    return m;
}

std::vector<Counts> toCounts(const json::Value &resp) {
    std::vector<Counts> out;
    for (const char *key : {"channels", "mpims", "ims"}) {
        for (const Value o : resp[key]) {
            Counts c;
            c.id = owned(o["id"]);
            if (c.id.empty())
                continue;
            c.latest   = model::parseTs(o["latest"].str());
            c.lastRead = model::parseTs(o["last_read"].str());
            // Mentions are exact; for the rest only has_unreads (or an IM's
            // dm_count) — a 1 means "some", the poll only compares for movement.
            c.mentions = uint32_t(o["mention_count"].integer());
            c.unread   = std::max(
                {c.mentions,
                 uint32_t(o["dm_count"].integer()),
                 uint32_t(o["has_unreads"].boolean())}
            );
            out.push_back(std::move(c));
        }
    }
    return out;
}

void starredConversationIds(const json::Value &items, std::vector<std::string> &out) {
    for (const Value it : items) {
        const std::string_view t = it["type"].str();
        if (t != "channel" && t != "im" && t != "group" && t != "mpim")
            continue;
        // A message / file star also carries `channel`: never trust that alone.
        if (it.has("message") || it.has("file") || it.has("comment"))
            continue;
        if (const std::string_view id = it["channel"].str(); !id.empty())
            out.emplace_back(id);
    }
}

void toSections(
    const Value                                  &resp,
    std::vector<model::Store::Section>           &out,
    std::unordered_map<std::string, std::string> &system
) {
    // Follow the chain from the one no other section points at; any left
    // off it (a broken chain) follow in the order listed.
    const Value                     all = resp["channel_sections"];
    std::vector<Value>              list;
    std::unordered_set<std::string> pointed;
    for (const Value s : all) {
        list.push_back(s);
        if (const std::string_view n = s["next_channel_section_id"].str(); !n.empty())
            pointed.emplace(n);
    }
    std::vector<Value>              ordered;
    std::unordered_set<std::string> placed;
    const auto                      byId = [&](std::string_view id) -> int {
        for (size_t i = 0; i < list.size(); ++i)
            if (!id.empty() && list[i]["channel_section_id"].str() == id)
                return int(i);
        return -1;
    };
    for (size_t h = 0; h < list.size(); ++h) {
        if (pointed.count(std::string(list[h]["channel_section_id"].str())))
            continue;
        for (int i = int(h);
             i >= 0 && placed.emplace(list[size_t(i)]["channel_section_id"].str()).second;
             i = byId(list[size_t(i)]["next_channel_section_id"].str()))
            ordered.push_back(list[size_t(i)]);
    }
    for (const Value &s : list)
        if (placed.emplace(s["channel_section_id"].str()).second)
            ordered.push_back(s);
    for (const Value &s : ordered) {
        const std::string_view type = s["type"].str();
        const std::string      id(s["channel_section_id"].str());
        if (type != "standard") {
            if (!type.empty() && !id.empty())
                system[std::string(type)] = id;
            continue;
        }
        if (id.empty() || s["is_redacted"].boolean())
            continue;
        model::Store::Section sec;
        sec.id    = id;
        sec.name  = s["name"].str();
        sec.emoji = s["emoji"].str();
        // client.channels says channel_ids; users.channelSections.list pages them.
        const Value page =
            s.has("channel_ids_page") ? s["channel_ids_page"]["channel_ids"] : s["channel_ids"];
        for (const Value c : page)
            if (!c.str().empty())
                sec.convs.emplace_back(c.str());
        out.push_back(std::move(sec));
    }
}

std::string firstIcon(const Value &icons) {
    for (const char *k : {"image_72", "image_48", "image_36"})
        if (const std::string_view v = icons[k].str(); !v.empty())
            return std::string(v);
    return {};
}

int64_t epochSecs(const Value &v) {
    if (v.isString())
        return std::strtoll(std::string(v.str()).c_str(), nullptr, 10);
    const double d = v.number(); // a fraction cut off, as a string's is
    return d > -9.2e18 && d < 9.2e18 ? int64_t(d) : 0;
}

bool toFeedThread(const Value &t, model::Store &store, FeedThread &out) {
    const Value rootObj = t["root_msg"];
    if (rootObj.has("subscribed") && !rootObj["subscribed"].boolean())
        return false;
    // root_msg is a whole message and, unlike history ones, names its channel.
    out.conv     = store.findConversation(rootObj["channel"].str());
    out.root     = model::parseTs(rootObj["ts"].str());
    out.lastRead = model::parseTs(rootObj["last_read"].str());
    if (out.conv == model::kNoConv || !out.root)
        return false;
    // latest_replies on a read thread, unread_replies (alone) on one with
    // news: both, deduplicated.
    const std::string &me = store.user(store.me).id;
    for (const char *k : {"latest_replies", "unread_replies"})
        for (const Value r : t[k]) {
            model::Message m = toMessage(r, store);
            if (!m.ts || std::any_of(out.replies.begin(), out.replies.end(), [&](const auto &x) {
                    return x.ts == m.ts;
                }))
                continue;
            m.threadTs = out.root;
            out.replies.push_back(std::move(m));
            out.parentIsMe.push_back(!me.empty() && r["parent_user_id"].str() == me);
        }
    // Oldest first, the flags with them (a handful: insertion order).
    for (size_t i = 1; i < out.replies.size(); ++i)
        for (size_t j = i; j > 0 && out.replies[j].ts < out.replies[j - 1].ts; --j) {
            std::swap(out.replies[j], out.replies[j - 1]);
            std::swap(out.parentIsMe[j], out.parentIsMe[j - 1]);
        }
    return true;
}

bool toThreadSub(const Value &t, model::Store &store, ThreadSub &out) {
    out.conv     = store.findConversation(t["channel_id"].str());
    out.root     = model::parseTs(t["timestamp"].str());
    out.latest   = model::parseTs(t["latest"].str());
    out.lastRead = model::parseTs(t["last_read"].str());
    return out.conv != model::kNoConv && out.root;
}

// ── The activity feed ───────────────────────────────────────────────────────

namespace {

using Kind = model::Backend::ActivityItem::Kind;

constexpr const char *kMentionTypes =
    "at_user,at_user_group,at_channel,at_everyone,keyword,unjoined_channel_mention";
// Every type the page shows, as the All tab asks for them.
constexpr const char *kAllTypes = "at_user,at_user_group,at_channel,at_everyone,keyword,"
                                  "unjoined_channel_mention,thread_v2,message_reaction,dm,"
                                  "bot_dm_bundle,channel";

bool activityKind(std::string_view type, Kind &out) {
    if (type == "thread_v2")
        out = Kind::Thread;
    else if (type == "message_reaction")
        out = Kind::Reaction;
    else if (type == "dm" || type == "bot_dm_bundle")
        out = Kind::Dm;
    else if (type == "channel")
        out = Kind::Channel;
    else if (
        type == "at_user" || type == "at_user_group" || type == "at_channel" ||
        type == "at_everyone" || type == "keyword" || type == "unjoined_channel_mention"
    )
        out = Kind::Mention;
    else
        return false;
    return true;
}

} // namespace

const char *activityTypes(model::Backend::ActivityFilter filter) {
    using F = model::Backend::ActivityFilter;
    switch (filter) {
    case F::Dms:
        return "dm";
    case F::Mentions:
        return kMentionTypes;
    case F::Threads:
        return "thread_v2";
    case F::All:
        break;
    }
    return kAllTypes;
}

bool toActivityItem(const Value &v, model::Store &store, model::Backend::ActivityItem &out) {
    const Value item = v["item"];
    const auto  type = item["type"].str();
    if (!activityKind(type, out.kind))
        return false;
    out.key             = std::string(v["key"].str());
    out.type            = std::string(type);
    out.feedTs          = std::string(v["feed_ts"].str());
    out.unread          = v["is_unread"].boolean();
    // Mentions and reactions point at the message; the bundles (a thread,
    // a DM, a channel) at their newest one.
    const Value payload = item["bundle_info"]["payload"];
    Value       msg;
    switch (out.kind) {
    case Kind::Mention:
    case Kind::Reaction:
        msg = item["message"];
        break;
    case Kind::Dm:
        msg = payload.has("dm_entry") ? payload["dm_entry"]["latest_message"] : payload["message"];
        break;
    case Kind::Channel:
        msg = payload["channel_entry"]["latest_message"];
        break;
    case Kind::Thread:
        break;
    }
    std::string_view channel;
    if (out.kind == Kind::Thread) {
        const Value t = payload["thread_entry"];
        channel       = t["channel_id"].str();
        out.ts        = model::parseTs(t["latest_ts"].str());
        out.thread    = model::parseTs(t["thread_ts"].str());
        out.count     = int(t["unread_msg_count"].integer());
    } else {
        channel = msg["channel"].str();
        out.ts  = model::parseTs(msg["ts"].str());
        // A reply: its root (a root's own thread_ts is itself).
        if (const model::Ts root = model::parseTs(msg["thread_ts"].str()); root && root != out.ts)
            out.thread = root;
        out.count = int(item["bundle_info"]["unread_count"].integer());
    }
    if (out.kind == Kind::Reaction) {
        out.actor    = store.internUser(item["reaction"]["user"].str());
        out.reaction = std::string(item["reaction"]["name"].str());
    }
    out.conv = store.findConversation(channel);
    return out.conv != model::kNoConv && out.ts;
}

int activityUnread(const Value &resp) {
    const Value counts = resp["activity_v2"];
    int         n      = 0;
    for (std::string_view types = kAllTypes; !types.empty();) {
        const size_t comma = types.find(',');
        n += int(counts[types.substr(0, comma)].integer());
        types = comma == std::string_view::npos ? std::string_view() : types.substr(comma + 1);
    }
    return n;
}

} // namespace slack::mapjson
