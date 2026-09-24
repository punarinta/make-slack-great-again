// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_render.h"
#include "session/session.h"
#include "text/link_labels.h"
#include "text/mrkdwn_parser.h"
#include "ui/icon_utils.h"
#include "ui/paint_utils.h"
#include "ui/theme.h"
#include "util/emoji.h"
#include "util/emoji_font.h"
#include "util/time_format.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFontInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <limits>
#include <optional>

namespace MsgRender {

bool isMediaAttachment(const Attachment &att) {
    if (att.isMsgUnfurl || !att.pretext.isEmpty() || !att.authorName.isEmpty() ||
        !att.title.isEmpty() || !att.text.text.isEmpty() || !att.fields.empty() ||
        !att.footer.isEmpty() || !att.buttons.empty() || !att.files.empty())
        return false;

    bool hasImage = !att.imageUrl.isEmpty() || !att.thumbUrl.isEmpty();
    for (const auto &block : att.blocks) {
        if (block.typeStr == QLatin1String("image") && !block.imageUrl.isEmpty()) {
            hasImage = true;
            continue;
        }
        if (block.typeStr == QLatin1String("divider") || !block.text.text.isEmpty() ||
            !block.buttons.empty() || !block.tableRows.empty())
            return false;
    }
    return hasImage;
}

QString resolveEmoji(const QString &name) {
    return Emoji::fromName(name);
}

EmojiResolved resolveEmojiRich(const QString &name, const QHash<QString, QString> &customMap) {
    // Slack appends modifiers (notably skin tones) as "::"-separated suffixes,
    // e.g. "+1::skin-tone-3" or "raised_hands::skin-tone-2". Resolve the base
    // emoji first, then append each modifier glyph so the composed Unicode
    // sequence renders the tinted variant instead of falling back to ":name:".
    const int sep = name.indexOf(QLatin1String("::"));
    if (sep > 0) {
        const QString baseName    = name.left(sep);
        EmojiResolved base        = resolveEmojiRich(baseName, customMap);
        const bool    baseIsGlyph = base.resolved && !base.unicode.isEmpty();
        if (baseIsGlyph) {
            QString out = base.unicode;
            for (const QString &mod :
                 name.mid(sep + 2).split(QLatin1String("::"), Qt::SkipEmptyParts)) {
                const QString glyph = Emoji::fromName(mod);
                if (glyph != ":" + mod + ":")
                    out += glyph;
            }
            return {out, {}};
        }
        return base; // custom-image base — modifiers can't apply
    }

    const QString unicode = Emoji::fromName(name);
    if (unicode != ":" + name + ":")
        return {unicode, {}};
    // `name` isn't a known shortcode. If it's already a raw emoji glyph, render it
    // directly instead of the ":name:" placeholder — MS Teams returns a reaction's
    // reactionType as the Unicode emoji itself (e.g. "👍"), not a shortcode.
    // Shortcodes are ASCII; any non-ASCII codepoint means `name` is the glyph.
    for (const QChar &ch : name)
        if (ch.unicode() > 0x7F)
            return {name, {}};
    QString cur = name;
    for (int hops = 0; hops < 8; ++hops) { // bounded alias-chain walk
        const auto it = customMap.constFind(cur);
        if (it == customMap.constEnd())
            break;
        if (it->startsWith(QLatin1String("alias:"))) {
            cur                   = it->mid(6);
            const QString aliased = Emoji::fromName(cur);
            if (aliased != ":" + cur + ":")
                return {aliased, {}};
            continue;
        }
        return {{}, *it};
    }
    return {unicode, {}, /*resolved=*/false};
}

EmojiResolved resolveEmojiRich(const QString &name, const Session *session) {
    static const QHash<QString, QString> kEmpty;
    return resolveEmojiRich(name, session ? session->emojiMap() : kEmpty);
}

// Slack's body metric: 22px line-height on a 15px font (×1.4667). Everything
// line-height-related derives from that ratio applied to the active font.
static int slackLinePx() {
    return qRound(QFontInfo(QApplication::font()).pixelSize() * 22.0 / 15.0);
}

int inlineEmojiPx() {
    // Slack renders inline emoji exactly one line-height tall.
    return slackLinePx();
}

// HTML for one resolved emoji at `px` logical pixels: custom emoji as an <img>
// (the image resource is registered on the QTextDocument by the caller), built-in
// emoji as a span in the platform color-emoji font.
static QString emojiHtml(const EmojiResolved &er, int px) {
    // Unknown name: it's text, not emoji. Keep it in the body font — the emoji
    // font at line-height size turned a plain "14:43:34" into a giant spaced-out
    // ":43:" (and does the same to any custom emoji not loaded yet).
    if (!er.resolved)
        return er.unicode.toHtmlEscaped();
    const QString s = QString::number(px);
    // vertical-align:bottom sits the image on the line's descent line, the
    // same footprint the emoji-font glyph below occupies (Slack draws both one
    // line-height tall, flush with the line box). Qt's default parks an inline
    // image's BOTTOM on the baseline, which lifted custom emoji a descender
    // above the built-in ones and grew the line by the same amount.
    if (!er.imageUrl.isEmpty())
        return "<img src='" + er.imageUrl.toHtmlEscaped() + "' width='" + s + "' height='" + s +
               "' style='vertical-align:bottom'>";
    return "<span style='font-family:" + emojiFontFamily() + ";font-size:" + s + "px'>" +
           er.unicode.toHtmlEscaped() + "</span>";
}

QString docStyleSheet() {
    // Qt's natural line height already includes per-font leading, so use the
    // proportional factor that lands on Slack's line height for this font —
    // the old fixed 135% overshot it and made paragraph gaps look bloated.
    const int natural = QFontMetrics(QApplication::font()).height();
    const int pct     = std::max(100, qRound(slackLinePx() * 100.0 / natural));
    return QString("p { line-height: %1%; margin: 0; }").arg(pct);
}

QStringList collectEmojiImageUrls(
    const Message &msg, const Session *session, bool showLinkPreviews, QSet<QString> *mediaUrls
) {
    QStringList   out;
    QSet<QString> seen;
    auto          addFrom = [&](const TextWithEntities &twe) {
        for (const auto &e : twe.entities) {
            if (e.type != EntityType::Emoji)
                continue;
            const auto er = resolveEmojiRich(e.data, session);
            if (!er.imageUrl.isEmpty() && !seen.contains(er.imageUrl)) {
                seen.insert(er.imageUrl);
                out << er.imageUrl;
            }
        }
    };
    auto addBlockImage = [&](const Block &b) {
        if (b.typeStr == "image" && !b.imageUrl.isEmpty() && !seen.contains(b.imageUrl)) {
            seen.insert(b.imageUrl);
            out << b.imageUrl;
            if (mediaUrls)
                mediaUrls->insert(b.imageUrl);
        }
    };
    addFrom(msg.text);
    for (const auto &b : msg.blocks) {
        addFrom(b.text);
        addBlockImage(b);
    }
    for (const auto &att : msg.attachments) {
        if (!showLinkPreviews && (att.isLinkPreview || att.isMsgUnfurl) && !isMediaAttachment(att))
            continue;
        if (!att.pretext.isEmpty()) // pretext is parsed as mrkdwn at render time
            addFrom(MrkdwnParser::parse(att.pretext));
        if (!att.title.isEmpty()) // title is token-resolved at render time
            addFrom(MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(att.title)));
        if (!att.footer.isEmpty()) // so is the footer
            addFrom(MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(att.footer)));
        // The footer icon is a real <img> in the document (see buildAttachHtml).
        if (!att.isMsgUnfurl && !att.footerIcon.isEmpty() && !seen.contains(att.footerIcon)) {
            seen.insert(att.footerIcon);
            out << att.footerIcon;
        }
        addFrom(att.text);
        for (const auto &f : att.fields)
            addFrom(f.value);
        for (const auto &b : att.blocks) {
            addFrom(b.text);
            addBlockImage(b);
        }
        // buildAttachHtml's last resort is the fallback string, parsed as
        // mrkdwn — so its emoji become <img> tags too. Mirror that condition
        // (nothing else produced a body) rather than scanning it always: a
        // fallback normally duplicates the body, and its emoji would just
        // trigger downloads for images no document shows.
        const bool bodyless = att.text.text.isEmpty() && att.fields.empty() && att.blocks.empty() &&
                              att.title.isEmpty() && att.pretext.isEmpty() &&
                              att.authorName.isEmpty() && att.buttons.empty();
        if (bodyless && !att.fallback.isEmpty())
            addFrom(MrkdwnParser::parse(att.fallback));
        // A message unfurl's card paints the quoted author's avatar. It's not an
        // <img> in the document, but it's fetched from the same cache — listing it
        // here is what kicks off the download and repaints the row once it
        // arrives, exactly like a custom emoji.
        if (att.isMsgUnfurl && !att.authorIcon.isEmpty() && !seen.contains(att.authorIcon)) {
            seen.insert(att.authorIcon);
            out << att.authorIcon;
        }
    }
    return out;
}

// These take Message::date (epoch microseconds) — the single orderable/display
// time field — so the UI never parses a ts string as a clock.
QString formatTs(qint64 dateMicros) {
    return TimeFmt::formatTime(dateMicros / 1000000);
}

// Attachment footer time, the way Slack shows it: the clock for today's
// attachments, the date for older ones ("Aug 20").
QString formatFooterTs(qint64 dateMicros) {
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(dateMicros / 1000000);
    if (dt.date() == QDate::currentDate())
        return TimeFmt::formatTime(dt);
    return TimeFmt::formatDate(dt.date());
}

// Slack's attachment footer metrics, scaled off its 15px body font: 12px text
// (#616061, links included — they're not blue there) and a 16px service icon.
int footerFontPx() {
    return std::max(8, qRound(QFontInfo(QApplication::font()).pixelSize() * 12.0 / 15.0));
}
int footerIconPx() {
    return std::max(8, qRound(QFontInfo(QApplication::font()).pixelSize() * 16.0 / 15.0));
}

QDate tsToDate(qint64 dateMicros) {
    return QDateTime::fromSecsSinceEpoch(dateMicros / 1000000).date();
}

QString formatDateLabel(qint64 dateMicros) {
    const QDate date = tsToDate(dateMicros);
    if (!date.isValid())
        return {};
    const QDate today = QDate::currentDate();
    if (date == today)
        return QCoreApplication::translate("MsgRender", "Today");
    if (date == today.addDays(-1))
        return QCoreApplication::translate("MsgRender", "Yesterday");
    return TimeFmt::formatDate(date);
}

// Takes the reply ts directly: latestReply has no dedicated date field like
// Message::date (Option A added one only for the message itself), so this is the
// one display site that still derives time from a ts string.
QString lastReplyLabel(const Ts &ts) {
    bool   ok   = false;
    double secs = ts.toDouble(&ok);
    if (!ok)
        return {};
    const QDateTime dt    = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs));
    const QString   time  = TimeFmt::formatTime(dt);
    const QDate     today = QDate::currentDate();
    if (dt.date() == today)
        return QCoreApplication::translate("MsgRender", "today at %1").arg(time);
    if (dt.date() == today.addDays(-1))
        return QCoreApplication::translate("MsgRender", "yesterday at %1").arg(time);
    return QCoreApplication::translate("MsgRender", "%1 at %2")
        .arg(TimeFmt::formatDate(dt.date()), time);
}

QString dateTimeLabel(qint64 dateMicros) {
    const QDateTime dt      = QDateTime::fromSecsSinceEpoch(dateMicros / 1000000);
    const QString   time    = TimeFmt::formatTime(dt);
    const QDate     today   = QDate::currentDate();
    const qint64    daysAgo = dt.date().daysTo(today);
    if (daysAgo <= 0)
        return time;
    if (daysAgo == 1)
        return QCoreApplication::translate("MsgRender", "yesterday at %1").arg(time);
    // Within the week the official client names the day ("Friday at 7:59 PM").
    if (daysAgo < 7)
        return QCoreApplication::translate("MsgRender", "%1 at %2")
            .arg(TimeFmt::locale().dayName(dt.date().dayOfWeek()), time);
    return QCoreApplication::translate("MsgRender", "%1 at %2")
        .arg(TimeFmt::formatDate(dt.date()), time);
}

// Resolve a UserMention entity's display name via entity.data (the user ID).
static QString resolveMentionImpl(const QString &userId, const Session *session) {
    if (!session)
        return "@" + userId;
    const auto *u = session->findUser(UserId{userId});
    return u ? ("@" + u->displayLabel()) : ("@" + userId);
}

QString resolveMention(const QString &userId, const Session *session) {
    return resolveMentionImpl(userId, session);
}

// Resolve a ChannelMention's display name via entity.data (the channel ID).
// Slack often echoes a channel link as a bare "<#C123>" with no name part, so
// the parser can only bake "#C123"; resolving against the conversation cache
// recovers the real "#general" the official client shows (channels outside the
// list, e.g. archived ones, come from Session::fetchChannelIfNeeded). `fallback`
// is the parser's baked text, used when the name is unknown.
static QString
resolveChannelImpl(const QString &channelId, const QString &fallback, const Session *session) {
    if (session) {
        const QString name = session->mentionedChannelName(ConversationId{channelId});
        if (!name.isEmpty())
            return "#" + name;
    }
    return fallback;
}

QString convPlaceLabel(const QString &convId, const Session *session) {
    const Conversation *c = session ? session->findConversation(ConversationId{convId}) : nullptr;
    if (!c)
        return {};
    if (c->kind == ConvKind::Im && c->dmUser) {
        // Cache-only lookup (this runs while a document is being built, and a
        // users.info fetch from here would be a surprise); the id is never shown.
        const User *u = session->findUser(*c->dmUser);
        return u ? u->displayLabel() : QString();
    }
    // Group DMs carry Slack's internal "mpdm-a--b--c-1" name — never show it;
    // a name the user gave the group is fine.
    if (c->kind == ConvKind::Mpim) {
        const QString custom = groupDmCustomName(*c);
        return custom.isEmpty() ? QCoreApplication::translate("MsgRender", "group message")
                                : custom;
    }
    return c->name.isEmpty() ? QString() : "#" + c->name;
}

QString messageLinkLabel(const SlackLinks::MessageRef &ref, const Session *session) {
    const QString place = convPlaceLabel(ref.conv, session);
    // The author is only known when Slack sent the link as a rich_text
    // `message_mention`; a plain permalink URL carries no author, and finding
    // one would mean fetching the linked message.
    const User   *author =
        (session && !ref.author.isEmpty()) ? session->findUser(UserId{ref.author}) : nullptr;
    if (author && !place.isEmpty())
        return QCoreApplication::translate("MsgRender", "%1 in %2")
            .arg(author->displayLabel(), place);
    if (author)
        return author->displayLabel();
    if (!place.isEmpty())
        return place;
    return QCoreApplication::translate("MsgRender", "message");
}

bool hasMessageLink(const TextWithEntities &twe) {
    for (const auto &e : twe.entities)
        if (e.type == EntityType::MessageLink)
            return true;
    return false;
}

bool hasMessageLink(const Message &msg) {
    const auto inBlocks = [](const std::vector<Block> &blocks) {
        for (const auto &b : blocks) {
            if (hasMessageLink(b.text))
                return true;
            for (const auto &row : b.tableRows)
                for (const auto &cell : row)
                    if (hasMessageLink(cell))
                        return true;
        }
        return false;
    };
    if (hasMessageLink(msg.text) || inBlocks(msg.blocks))
        return true;
    for (const auto &att : msg.attachments) {
        if (hasMessageLink(att.text) || inBlocks(att.blocks))
            return true;
        for (const auto &f : att.fields)
            if (hasMessageLink(f.value))
                return true;
    }
    return false;
}

void registerMessageLinkIcon(QTextDocument *doc, qreal dpr) {
    doc->addResource(
        QTextDocument::ImageResource,
        QUrl(kMessageLinkIconRes),
        svgPixmapPhys(":/ui/message-square.svg", QSize(11, 11), Th::c().text.link, dpr)
    );
}

// The chip that replaces a bare message permalink: icon + conversation label on
// a tinted rounded background, the same chrome as a #channel mention.
static QString messageLinkChipHtml(const SlackLinks::MessageRef &ref, const Session *session) {
    return "<a href='" + messageAnchor(ref).toHtmlEscaped() +
           "' style='color:" + Th::qss(Th::c().text.link) +
           ";background:" + Th::qss(Th::c().message.mentionBg) +
           ";border-radius:3px;padding:0 2px;text-decoration:none'><img src='" +
           kMessageLinkIconRes + "' width='11' height='11'>&nbsp;" +
           messageLinkLabel(ref, session).toHtmlEscaped() + "</a>";
}

QString notificationText(const TextWithEntities &twe, const Session *session) {
    // Walk the leaf entities that change the displayed text — mentions, channel
    // links and emoji — and substitute their resolved form into the parsed plain
    // text. Container entities (bold/italic/links/quotes) don't alter the text,
    // so they're ignored. Leaf spans never overlap each other, so a single
    // left-to-right rebuild is safe. Only ever called when a notification fires.
    struct Repl {
        int     offset;
        int     length;
        QString text;
    };
    std::vector<Repl> repls;
    for (const auto &e : twe.entities) {
        switch (e.type) {
        case EntityType::UserMention: {
            // Prefer the live cache; fall back to the parser's baked text (the
            // "<@U7|alice>" label, or "@U7" for a bare mention) when uncached.
            const User *u = session ? session->findUser(UserId{e.data}) : nullptr;
            repls.push_back(
                {e.offset,
                 e.length,
                 u ? ("@" + u->displayLabel()) : twe.text.mid(e.offset, e.length)}
            );
            break;
        }
        case EntityType::ChannelMention:
            repls.push_back(
                {e.offset,
                 e.length,
                 resolveChannelImpl(e.data, twe.text.mid(e.offset, e.length), session)}
            );
            break;
        case EntityType::UsergroupMention: {
            const Usergroup *g = session ? session->findUsergroup(e.data) : nullptr;
            repls.push_back(
                {e.offset, e.length, g ? g->mentionLabel() : twe.text.mid(e.offset, e.length)}
            );
            break;
        }
        case EntityType::Emoji: {
            const auto    er    = resolveEmojiRich(e.data, session);
            const QString glyph = er.unicode.isEmpty() ? (":" + e.data + ":") : er.unicode;
            repls.push_back({e.offset, e.length, glyph});
            break;
        }
        case EntityType::MessageLink:
            // The chip's own words — a toast showing a bare permalink URL says
            // nothing about what was linked.
            repls.push_back(
                {e.offset, e.length, messageLinkLabel(SlackLinks::refFromToken(e.data), session)}
            );
            break;
        default:
            break;
        }
    }
    if (repls.empty())
        return twe.text;
    std::sort(repls.begin(), repls.end(), [](const Repl &a, const Repl &b) {
        return a.offset < b.offset;
    });
    QString out;
    int     pos = 0;
    for (const auto &r : repls) {
        if (r.offset < pos)
            continue; // defensive: skip any overlapping span
        out += QStringView{twe.text}.mid(pos, r.offset - pos);
        out += r.text;
        pos = r.offset + r.length;
    }
    out += QStringView{twe.text}.mid(pos);
    return out;
}

QString notificationPreview(const Message &msg, const Session *session) {
    // The OS toast wants one line of readable text. A human's message carries it
    // in `text`, so use that whenever it's there. Bot integrations (CodePipeline,
    // Amazon Q, GitHub, …) instead leave `text` empty and put everything in Block
    // Kit blocks or legacy attachments — which is why those toasts came through
    // blank. When `text` is empty, flatten the same block/attachment content the
    // message list renders into a short plain-text summary.
    const QString primary = notificationText(msg.text, session);
    if (!primary.trimmed().isEmpty())
        return primary;

    QStringList pieces;
    auto        add = [&](const TextWithEntities &twe) {
        const QString t = notificationText(twe, session).simplified();
        if (!t.isEmpty())
            pieces << t;
    };
    // Block Kit: header/section/context/rich_text all carry displayable text.
    // "image"/"divider"/"actions" blocks have none, so add() just skips them.
    auto addBlocks = [&](const std::vector<Block> &blocks) {
        for (const auto &b : blocks)
            add(b.text);
    };

    addBlocks(msg.blocks);
    for (const auto &att : msg.attachments) {
        const int before = pieces.size();
        if (!att.pretext.isEmpty())
            add(MrkdwnParser::parse(att.pretext));
        if (!att.title.isEmpty())
            add(MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(att.title)));
        add(att.text);
        for (const auto &f : att.fields) {
            const QString val = notificationText(f.value, session).simplified();
            if (val.isEmpty())
                continue;
            // Field titles are plain strings straight off the API, like the
            // attachment title — decode their HTML escapes as the renderer does.
            const QString key = MrkdwnParser::decodeEntities(f.title).simplified();
            pieces << (key.isEmpty() ? val : key + ": " + val);
        }
        addBlocks(att.blocks);
        // Slack authors `fallback` precisely as the notification-safe summary, so
        // it's the right last resort when this attachment had no richer text above.
        // It's mrkdwn like any other attachment text, so run it through the parser
        // rather than showing raw "<url|label>" tokens in the toast. Skipped when
        // the attachment renders something text can't carry (image, table block,
        // buttons) — there Slack's fallback is a placeholder like "[no preview
        // available]", which the message list drops for the same reason.
        const bool rendersNonText =
            !att.blocks.empty() || !att.imageUrl.isEmpty() || !att.buttons.empty();
        if (pieces.size() == before && !rendersNonText && !att.fallback.isEmpty())
            add(MrkdwnParser::parse(att.fallback));
    }

    return pieces.join(QStringLiteral(" · "));
}

std::vector<UserId> notificationRawMentions(const Message &msg) {
    std::vector<UserId> ids;
    auto                add = [&](const TextWithEntities &twe) {
        for (const auto &e : twe.entities) {
            if (e.type != EntityType::UserMention || e.data.isEmpty())
                continue;
            // "<@U7|alice>" baked its own label into the text — nothing to look
            // up, and waiting on users.info for it would only delay the toast.
            if (!twe.text.mid(e.offset, e.length).contains(e.data))
                continue;
            const UserId id{e.data};
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
    };
    auto addBlocks = [&](const std::vector<Block> &blocks) {
        for (const auto &b : blocks)
            add(b.text);
    };

    add(msg.text);
    addBlocks(msg.blocks);
    for (const auto &att : msg.attachments) {
        add(att.text);
        for (const auto &f : att.fields)
            add(f.value);
        addBlocks(att.blocks);
    }
    return ids;
}

static QString escapeAndBr(const QString &s) {
    return s.toHtmlEscaped().replace("\n", "<br>");
}

// Render the entities listed in `nodes` (indices into `ents`, all spanning
// [start,end) of `text`) plus the plain-text gaps between them. Container
// entities (bold, links, quotes…) recurse into their children, so nested
// spans like *<url|label>* render as a link inside <b>.
// Beyond this many nested blockquote levels we stop emitting the nested <table>
// wrapper and render the content inline. QTextDocumentLayout lays tables out with
// recursive frame layout whose cost is ~exponential in nesting depth: a quoted
// email reply chain (the IMAP backend builds one Blockquote entity per '>' level)
// measured ~16x per +4 levels — depth 16 took ~7.7 s in a plain Debug build (far
// worse and effectively forever under ASan) and froze the whole UI inside
// QTextDocument::size(), caught by the hang watchdog. Capping at 4 levels (≤5
// nested tables) keeps that same message at ~60 ms. A handful of quote bars is all
// that's ever readable anyway; deeper levels keep their text, just without another
// bar.
static constexpr int kMaxQuoteRenderDepth = 4;

static QString renderRange(
    const QString                       &text,
    int                                  start,
    int                                  end,
    const std::vector<int>              &nodes,
    const std::vector<TextEntity>       &ents,
    const std::vector<std::vector<int>> &kids,
    const Session                       *session,
    const InlineStyle                   &style,
    int                                  quoteDepth = 0
) {
    QString html;
    int     pos = start;
    for (int idx : nodes) {
        const auto &e = ents[idx];
        if (e.offset > pos)
            html += escapeAndBr(text.mid(pos, e.offset - pos));
        const auto    rawInner  = text.mid(e.offset, e.length);
        const bool    container = e.type == EntityType::Bold || e.type == EntityType::Italic ||
                                  e.type == EntityType::Underline || e.type == EntityType::Strike ||
                                  e.type == EntityType::Link || e.type == EntityType::Blockquote;
        // Only blockquotes deepen the table-nesting budget (other containers are
        // cheap inline spans/anchors).
        const int     childQuoteDepth = quoteDepth + (e.type == EntityType::Blockquote ? 1 : 0);
        const QString inner           = container ? renderRange(
                                                        text,
                                                        e.offset,
                                                        e.offset + e.length,
                                                        kids[idx],
                                                        ents,
                                                        kids,
                                                        session,
                                                        style,
                                                        childQuoteDepth
                                                    )
                                                  : rawInner.toHtmlEscaped();
        switch (e.type) {
        case EntityType::Bold:
            html += "<b>" + inner + "</b>";
            break;
        case EntityType::Italic:
            html += "<i>" + inner + "</i>";
            break;
        case EntityType::Underline:
            html += "<u>" + inner + "</u>";
            break;
        case EntityType::Strike:
            html += "<s>" + inner + "</s>";
            break;
        case EntityType::Code:
            html += "<span style='background:" + Th::qss(Th::c().message.codeBlockBg) +
                    ";color:" + Th::qss(Th::c().danger.text) +
                    ";font-family:monospace;font-size:0.88em;padding:1px 3px;border-radius:3px'>" +
                    inner + "</span>";
            break;
        case EntityType::Pre: {
            // A single-cell table, not <pre>: Qt paints a <pre> CSS background as a
            // per-line character background (stripey rows). The table carries NO
            // background/border itself — Qt rich text can't do border-radius, so the
            // rounded chrome is painted underneath by paintCodeBlockChrome(), which
            // finds these tables via codeBlockRects().
            if (html.endsWith("<br>"))
                html.chop(4); // the block carries its own top margin
            QString code = inner;
            while (code.endsWith('\n'))
                code.chop(1);
            html += "<table width='100%' cellspacing='0' cellpadding='0' "
                    "style='margin:4px 0'>"
                    "<tr><td style='padding:6px 10px;font-family:monospace;font-size:0.88em;"
                    "white-space:pre-wrap;color:" +
                    Th::qss(Th::c().message.codeText) + "'>" + code + "</td></tr></table>";
            break;
        }
        case EntityType::Blockquote:
            // Past the nesting cap, drop the <table> wrapper (see kMaxQuoteRenderDepth)
            // and render the content inline so a deep email reply chain can't make
            // QTextDocument layout hang. quoteDepth is this quote's own level (its
            // inner content was already rendered at quoteDepth+1).
            if (quoteDepth > kMaxQuoteRenderDepth) {
                html += inner;
            } else {
                // Use a table so the gray left bar renders reliably in Qt's HTML subset.
                html +=
                    "<table cellspacing='0' cellpadding='0' style='border-spacing:0;margin:4px 0'>"
                    "<tr>"
                    "<td width='3' bgcolor='" +
                    Th::c().message.codeBlockBorder.name() +
                    "' style='padding:0;border-radius:2px'></td>"
                    "<td style='padding:2px 0 2px 10px;color:" +
                    Th::qss(Th::c().message.codeText) + "'>" + inner + "</td></tr></table>";
            }
            break;
        case EntityType::Link: {
            if (LinkLabels::isGiphyMediaUrl(e.data)) {
                // A GIPHY media URL is an opaque hash, and Slack unfurls the
                // animation underneath the message anyway, so the link is drawn
                // as a "GIF" badge — a pill like a mention — with the link's own
                // title after it when it has one.
                const bool titled = !LinkLabels::isUrlLabel(rawInner, e.data);
                html += "<a href='" + e.data.toHtmlEscaped() +
                        "' style='color:" + Th::qss(Th::c().message.mentionText) +
                        ";background:" + Th::qss(Th::c().message.mentionBg) +
                        ";border-radius:3px;padding:0 4px;text-decoration:none'><b>" +
                        QCoreApplication::translate("MsgRender", "GIF") + "</b>" +
                        (titled ? QStringLiteral(" · ") + inner : QString()) + "</a>";
                break;
            }
            // Slack's composer stores pasted-URL labels aggressively truncated
            // ("host/…/…"); rebuild a longer one from the full URL. Capped by
            // characters, not layout width — the HTML is built once per doc and
            // survives resizes, so it can't know the final line width.
            constexpr int kMaxLinkLabelChars = 96;
            const QString label =
                LinkLabels::isShortenedUrlLabel(rawInner, e.data)
                    ? LinkLabels::expandedLabel(e.data, kMaxLinkLabelChars).toHtmlEscaped()
                    : inner;
            html += "<a href='" + e.data.toHtmlEscaped() + "' style='color:" +
                    Th::qss(style.linkColor.isValid() ? style.linkColor : Th::c().text.link) +
                    (style.fontPx > 0 ? ";font-size:" + QString::number(style.fontPx) + "px"
                                      : QString()) +
                    ";text-decoration:none'>" + label + "</a>";
            break;
        }
        case EntityType::MessageLink:
            html += messageLinkChipHtml(SlackLinks::refFromToken(e.data), session);
            break;
        case EntityType::UserMention: {
            // Prefer the live cache; otherwise keep the parser's baked label
            // (the "<@W|Name>" display part, or "@W…" for a bare mention) rather
            // than forcing the raw id — an external collaborator's mention reads
            // as a name the moment users.info resolves it.
            const User   *u     = session ? session->findUser(UserId{e.data}) : nullptr;
            const QString label = u ? ("@" + u->displayLabel()) : rawInner;
            const bool    isMe  = session && UserId{e.data} == session->meUserId();
            // Anchor (not span) so the mention is hit-testable for the hover profile card.
            html += "<a href='" + (kUserAnchorPrefix + e.data).toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().message.mentionText) + ";background:" +
                    Th::qss(isMe ? Th::c().message.mentionSelfBg : Th::c().message.mentionBg) +
                    ";border-radius:3px;padding:0 2px;text-decoration:none'>" +
                    label.toHtmlEscaped() + "</a>";
            break;
        }
        case EntityType::ChannelMention: {
            const QString label = resolveChannelImpl(e.data, rawInner, session);
            // Anchor (not span) so the chip is clickable — the click handler
            // navigates to the channel.
            html += "<a href='" + (kChannelAnchorPrefix + e.data).toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().message.mentionText) +
                    ";background:" + Th::qss(Th::c().message.mentionBg) +
                    ";border-radius:3px;padding:0 2px;text-decoration:none'>" +
                    label.toHtmlEscaped() + "</a>";
            break;
        }
        case EntityType::HereCommand:
        case EntityType::ChannelCommand:
            html += "<span style='color:" + Th::qss(Th::c().message.mentionText) +
                    ";background:" + Th::qss(Th::c().message.mentionSelfBg) +
                    ";border-radius:3px;padding:0 2px'>" + inner + "</span>";
            break;
        case EntityType::UsergroupMention: {
            // The parser's text is the message's own label (or the bare id);
            // the live handle from usergroups.list wins when the group is known.
            const Usergroup *g    = session ? session->findUsergroup(e.data) : nullptr;
            const QString    text = g ? g->mentionLabel() : rawInner;
            const bool       mine = session && session->isMyUsergroup(e.data);
            html += "<span style='color:" + Th::qss(Th::c().message.mentionText) + ";background:" +
                    Th::qss(mine ? Th::c().message.mentionSelfBg : Th::c().message.mentionBg) +
                    ";border-radius:3px;padding:0 2px'>" + text.toHtmlEscaped() + "</span>";
            break;
        }
        case EntityType::Emoji:
            html += emojiHtml(resolveEmojiRich(e.data, session), inlineEmojiPx());
            break;
        }
        pos = e.offset + e.length;
        // The code-block table carries its own vertical margin — eat the newline
        // that followed the closing fence so it doesn't add a <br> on top.
        if (e.type == EntityType::Pre && pos < end && text[pos] == '\n')
            ++pos;
    }
    if (pos < end)
        html += escapeAndBr(text.mid(pos, end - pos));
    return html;
}

// Converts TextWithEntities to Qt-flavoured HTML for QTextDocument.
QString toHtml(const TextWithEntities &twe, const Session *session, const InlineStyle &style) {
    if (twe.entities.empty())
        return escapeAndBr(twe.text);

    // Parents before children: offset ascending, longer span first. The parser
    // pushes a wrapping entity before its nested ones, so a stable sort keeps
    // the parent first even for equal ranges (e.g. a link spanning all of a bold).
    auto sorted = twe.entities;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.offset != b.offset ? a.offset < b.offset : a.length > b.length;
    });

    // Containment tree — entity spans are nested-or-disjoint by construction.
    const int                     n = static_cast<int>(sorted.size());
    std::vector<std::vector<int>> kids(n);
    std::vector<int>              roots, stack;
    for (int i = 0; i < n; ++i) {
        const auto &e = sorted[i];
        while (!stack.empty()) {
            const auto &p = sorted[stack.back()];
            if (e.offset >= p.offset && e.offset + e.length <= p.offset + p.length)
                break;
            stack.pop_back();
        }
        (stack.empty() ? roots : kids[stack.back()]).push_back(i);
        stack.push_back(i);
    }
    return renderRange(twe.text, 0, twe.text.size(), roots, sorted, kids, session, style);
}

static void collectCodeTables(QTextFrame *frame, QVector<QTextTable *> &out) {
    for (auto it = frame->begin(); it != frame->end(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            // Code blocks are the only single-column percentage-width tables toHtml
            // emits with zero cell spacing (blockquotes are two-column, fixed
            // width; the bot-button container is full-width but carries the
            // kBotBtnCellSpacing marker).
            if (table->columns() == 1 &&
                table->format().width().type() == QTextLength::PercentageLength &&
                qRound(table->format().cellSpacing()) != kBotBtnCellSpacing)
                out.push_back(table);
        }
        collectCodeTables(child, out);
    }
}

QVector<QRectF> codeBlockRects(const QTextDocument *doc) {
    QVector<QTextTable *> tables;
    collectCodeTables(doc->rootFrame(), tables);
    QVector<QRectF> rects;
    rects.reserve(tables.size());
    auto *layout = doc->documentLayout();
    for (QTextTable *table : tables) {
        // QTextDocumentLayout::frameBoundingRect is unusable for tables (position
        // offset by the cell padding, size inflated by the margins) — rebuild the
        // outer rect from the single cell's accurate block geometry instead.
        const auto   cell  = table->cellAt(0, 0);
        const QRectF first = layout->blockBoundingRect(cell.firstCursorPosition().block());
        const QRectF last  = layout->blockBoundingRect(cell.lastCursorPosition().block());
        const auto   cf    = cell.format().toTableCellFormat();
        rects.push_back(QRectF(
            QPointF(first.left() - cf.leftPadding(), first.top() - cf.topPadding()),
            QPointF(first.right() + cf.rightPadding(), last.bottom() + cf.bottomPadding())
        ));
    }
    return rects;
}

void paintCodeBlockChrome(QPainter &p, const QTextDocument *doc) {
    const auto rects = codeBlockRects(doc);
    if (rects.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Th::c().message.codeBlockBorder, 1));
    p.setBrush(Th::c().message.codeBlockBg);
    for (const QRectF &r : rects)
        Paint::borderedRect(p, r, 4);
    p.restore();
}

// Wrap inline HTML in <p> (so the doc stylesheet's line-height applies), keeping any
// <table> elements (blockquotes) OUTSIDE the paragraphs. A table inside/after a <p>
// gets an implicit separator block that inherits the paragraph's line-height, which
// makes the gap above the table larger than the gap below (controlled only by the
// table's own bottom margin). Splitting at table boundaries keeps the gaps symmetric.
static QString wrapParagraph(const QString &inner, const QString &pStyle) {
    if (inner.isEmpty())
        return {};
    if (!inner.contains(QLatin1String("<table")))
        return "<p style='" + pStyle + "'>" + inner + "</p>";
    QString result;
    int     pos = 0;
    while (pos < inner.size()) {
        const int tableStart = inner.indexOf(QLatin1String("<table"), pos);
        if (tableStart < 0) {
            const QString tail = inner.mid(pos);
            // Qt SUMS the table's bottom margin with the next block's top margin
            // (no collapsing), so zero the top margin right after a table.
            if (!tail.isEmpty())
                result += "<p style='" + pStyle + ";margin-top:0'>" + tail + "</p>";
            break;
        }
        // Text segment before the table — strip trailing <br> (the \n the parser appends
        // after a blockquote turns into a leading <br> for the following segment), and
        // leave it UNwrapped (see above).
        if (tableStart > pos) {
            QString seg = inner.mid(pos, tableStart - pos);
            while (seg.endsWith(QLatin1String("<br>")))
                seg.chop(4);
            if (!seg.isEmpty())
                result += seg;
        }
        const int tableEnd = inner.indexOf(QLatin1String("</table>"), tableStart);
        if (tableEnd < 0)
            break;
        result += inner.mid(tableStart, tableEnd - tableStart + 8); // include </table>
        pos = tableEnd + 8;
        // Strip the leading <br> that follows the table.
        while (pos + 4 <= inner.size() && QStringView{inner}.mid(pos, 4) == u"<br>")
            pos += 4;
    }
    return result;
}

// HTML for one Block Kit image block (Slack GIF picker / Giphy / app images).
// With a GifRenderContext: a "GIF ▾" title line (collapse-toggle anchor, when the
// block has a title) followed by the real <img> sized to the kBlockImg cap; the
// doc owner registers the image resource and animates it by swapping in QMovie
// frames. Without one (preview dialogs): the alt text as italic placeholder.
static QString imageBlockHtml(
    const Block &blk, const Session *session, const GifRenderContext *gif, int blockIdx
) {
    if (!gif || blk.imageUrl.isEmpty()) {
        if (blk.altText.isEmpty())
            return {};
        return "<p style='color:" + Th::qss(Th::c().text.tertiary) +
               ";font-style:italic;margin:1px 0'>" + blk.altText.toHtmlEscaped() + "</p>";
    }
    const QString key       = gif->keyPrefix + "/b" + QString::number(blockIdx);
    const bool    collapsed = gif->collapsed && gif->collapsed->contains(key);

    QString html;
    if (!blk.text.text.isEmpty()) {
        html += "<p style='margin:2px 0;font-size:0.9em'><a href='" +
                (kGifToggleAnchorPrefix + key).toHtmlEscaped() +
                "' style='color:" + Th::qss(Th::c().text.secondary) + ";text-decoration:none'>" +
                toHtml(blk.text, session) + "&nbsp;<img src='" +
                (collapsed ? kGifChevronCollapsedRes : kGifChevronExpandedRes) +
                "' width='10' height='10'></a></p>";
    }
    if (!collapsed) {
        QString sizeAttrs;
        if (blk.imageWidth > 0 && blk.imageHeight > 0) {
            const double scale = std::min(
                1.0,
                std::min(
                    (double)kBlockImgMaxW / blk.imageWidth, (double)kBlockImgMaxH / blk.imageHeight
                )
            );
            sizeAttrs = " width='" + QString::number(qRound(blk.imageWidth * scale)) +
                        "' height='" + QString::number(qRound(blk.imageHeight * scale)) + "'";
        }
        html += "<p style='margin:2px 0 0'><img src='" + blk.imageUrl.toHtmlEscaped() + "'" +
                sizeAttrs + "></p>";
    }
    return html;
}

// Bot buttons as a flowing row of real-looking buttons. Each button is its own
// single-cell table floated left inside one full-width container cell: floats
// pack side by side and wrap to the next row when the message is narrow, so a
// row that doesn't fit breaks BETWEEN buttons instead of inside a label (a
// plain <tr> of cells can't wrap and squeezed the text instead). The container
// is what clears the floats — without it the following paragraph flows around
// them. The rounded border + background are painted underneath by
// paintBotButtonChrome() (Qt rich text can't do border-radius), which finds the
// container via the kBotBtnCellSpacing marker and the buttons as its floats.
// Every button is an anchor so it's hit-testable and gets the pointing cursor:
// URL buttons open their URL, interactive-only ones use the msga://botbtn/
// scheme — clicking shows why the action can't be delivered (see BotButton).
static QString buttonsHtml(const std::vector<BotButton> &buttons) {
    if (buttons.empty())
        return {};
    QString cells;
    for (size_t i = 0; i < buttons.size(); ++i) {
        const auto   &btn = buttons[i];
        const QColor  fg  = btn.style == QLatin1String("danger")    ? Th::c().danger.text
                            : btn.style == QLatin1String("primary") ? Th::c().accent.def
                                                                    : Th::c().text.primary;
        const QString href =
            btn.url.isEmpty() ? kBotBtnAnchorPrefix + QString::number(i)
                              : kBotBtnAnchorPrefix +
                                    "url:" + QString::fromLatin1(QUrl::toPercentEncoding(btn.url));
        cells += "<table cellspacing='0' cellpadding='0' style='float:left;margin:0 " +
                 QString::number(kBotBtnCellSpacing) + "px " + QString::number(kBotBtnCellSpacing) +
                 "px 0'><tr><td style='padding:4px 12px'><a href='" + href.toHtmlEscaped() +
                 "' style='color:" + Th::qss(fg) + ";font-weight:bold;text-decoration:none'>" +
                 btn.text.toHtmlEscaped() + "</a></td></tr></table>";
    }
    return "<table width='100%' cellspacing='" + QString::number(kBotBtnCellSpacing) +
           "' cellpadding='0' style='margin:4px 0 2px'><tr><td>" + cells + "</td></tr></table>";
}

static void collectButtonContainers(QTextFrame *frame, QVector<QTextTable *> &out) {
    for (auto it = frame->begin(); it != frame->end(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            // Button containers are the only tables emitted with this exact cell
            // spacing (code blocks, blockquotes and data tables use 0).
            if (qRound(table->format().cellSpacing()) == kBotBtnCellSpacing)
                out.push_back(table);
        }
        collectButtonContainers(child, out);
    }
}

QVector<QRectF> botButtonRects(const QTextDocument *doc) {
    QVector<QTextTable *> containers;
    collectButtonContainers(doc->rootFrame(), containers);
    QVector<QRectF> rects;
    auto           *layout = doc->documentLayout();
    for (QTextTable *container : containers) {
        // The buttons are the floating single-cell tables inside the container's
        // one cell; Qt's frame iterator visits floats in document order.
        const auto cell = container->cellAt(0, 0);
        if (!cell.isValid())
            continue;
        for (auto it = cell.begin(); it != cell.end(); ++it) {
            auto *button = qobject_cast<QTextTable *>(it.currentFrame());
            if (!button || button->format().position() == QTextFrameFormat::InFlow)
                continue;
            // Same approach as codeBlockRects: rebuild the cell's rect from its
            // block geometry + paddings (frameBoundingRect is unusable for tables).
            const auto   bc    = button->cellAt(0, 0);
            const QRectF first = layout->blockBoundingRect(bc.firstCursorPosition().block());
            const QRectF last  = layout->blockBoundingRect(bc.lastCursorPosition().block());
            const auto   cf    = bc.format().toTableCellFormat();
            rects.push_back(QRectF(
                QPointF(first.left() - cf.leftPadding(), first.top() - cf.topPadding()),
                QPointF(first.right() + cf.rightPadding(), last.bottom() + cf.bottomPadding())
            ));
        }
    }
    return rects;
}

void paintBotButtonChrome(QPainter &p, const QTextDocument *doc) {
    const auto rects = botButtonRects(doc);
    if (rects.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Th::c().message.fileChipBorder, 1));
    p.setBrush(Th::c().surface.raised);
    for (const QRectF &r : rects)
        Paint::borderedRect(p, r, 4);
    p.restore();
}

// Shared Block Kit block → HTML dispatch for buildMsgHtml/buildAttachHtml.
// Returns true when the block embedded a real image (caller skips text fallbacks).
static bool blockHtml(
    QString                &html,
    const Block            &blk,
    const Session          *session,
    const GifRenderContext *gif,
    int                     blockIdx
) {
    if (blk.typeStr == "divider") {
        html += "<hr style='border:0;border-top:1px solid " + Th::qss(Th::c().divider.def) +
                ";margin:4px 0'>";
    } else if (blk.typeStr == "header") {
        html += "<p style='font-size:1.1em;font-weight:bold;margin:2px 0'>" +
                toHtml(blk.text, session) + "</p>";
    } else if (blk.typeStr == "image") {
        html += imageBlockHtml(blk, session, gif, blockIdx);
        return gif && !blk.imageUrl.isEmpty();
    } else if (blk.typeStr == "table" && !blk.tableRows.empty()) {
        html += tableBlockHtml(blk, session, kMaxInlineTableRows);
    } else {
        if (!blk.text.text.isEmpty())
            html += wrapParagraph(toHtml(blk.text, session), "margin:2px 0");
        html += buttonsHtml(blk.buttons);
    }
    return false;
}

QString tableBlockHtml(const Block &blk, const Session *session, int maxRows) {
    // Data table (Slack's newer table messages). cellspacing 0 keeps it out of
    // the bot-button chrome (kBotBtnCellSpacing marker), and the fixed
    // (non-percentage) width keeps a one-column table out of the code-block
    // chrome. border-collapse makes Qt draw flat 1px grid lines instead of its
    // default ridged 3D border — and is what dataTableRects() keys on.
    const int total = (int)blk.tableRows.size();
    const int shown = maxRows > 0 ? std::min(total, maxRows) : total;
    QString   rows;
    for (int ri = 0; ri < shown; ++ri) {
        // When rows were cut, the last visible row is shaded — the official
        // client's "there's more below" cue.
        const bool    shaded  = shown < total && ri == shown - 1;
        const QString tdStyle = shaded ? "padding:3px 8px;color:" + Th::qss(Th::c().text.tertiary)
                                       : QStringLiteral("padding:3px 8px");
        rows += "<tr>";
        for (const auto &cell : blk.tableRows[ri])
            rows += "<td style='" + tdStyle + "'>" + toHtml(cell, session) + "</td>";
        rows += "</tr>";
    }
    return "<table cellspacing='0' cellpadding='0' style='margin:4px 0;"
           "border-collapse:collapse;border-width:1px;border-style:solid;border-color:" +
           Th::qss(Th::c().message.fileChipBorder) + "'>" + rows + "</table>";
}

Block csvToTableBlock(const QByteArray &bytes) {
    QByteArray raw = bytes;
    if (raw.startsWith("\xEF\xBB\xBF"))
        raw.remove(0, 3);
    const QString text = QString::fromUtf8(raw);

    // Sniff the delimiter from the first line: the most frequent of comma /
    // semicolon / tab outside quotes ("CSV" in the wild covers all three).
    int  commas = 0, semis = 0, tabs = 0;
    bool q = false;
    for (const QChar c : text) {
        if (c == '"')
            q = !q;
        else if (q)
            continue;
        else if (c == '\n' || c == '\r')
            break;
        else if (c == ',')
            ++commas;
        else if (c == ';')
            ++semis;
        else if (c == '\t')
            ++tabs;
    }
    const QChar delim = (tabs > commas && tabs > semis) ? QChar('\t')
                        : (semis > commas)              ? QChar(';')
                                                        : QChar(',');

    Block blk;
    blk.typeStr = "table";
    std::vector<TextWithEntities> row;
    QString                       cell;
    bool                          inQuotes = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { // "" = literal quote
                    cell += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cell += c;
            }
        } else if (c == '"' && cell.isEmpty()) {
            inQuotes = true;
        } else if (c == delim) {
            row.push_back({cell, {}});
            cell.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            if (!row.empty() || !cell.isEmpty()) { // skip blank lines
                row.push_back({cell, {}});
                cell.clear();
                blk.tableRows.push_back(std::move(row));
                row.clear();
            }
        } else {
            cell += c;
        }
    }
    // Last line without a trailing newline.
    if (!cell.isEmpty() || !row.empty()) {
        row.push_back({cell, {}});
        blk.tableRows.push_back(std::move(row));
    }
    return blk;
}

static void collectDataTables(QTextFrame *frame, QVector<QTextTable *> &out) {
    for (auto it = frame->begin(); it != frame->end(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            // Data tables are the only ones tableBlockHtml emits with
            // border-collapse (code blocks, blockquotes and button rows don't
            // set it).
            if (table->format().borderCollapse())
                out.push_back(table);
        }
        collectDataTables(child, out);
    }
}

QVector<QRectF> dataTableRects(const QTextDocument *doc) {
    QVector<QTextTable *> tables;
    collectDataTables(doc->rootFrame(), tables);
    QVector<QRectF> rects;
    rects.reserve(tables.size());
    auto *layout = doc->documentLayout();
    for (QTextTable *table : tables) {
        // frameBoundingRect is unusable for tables (see codeBlockRects) —
        // rebuild the outer rect from the corner cells' block geometry.
        const auto tl = table->cellAt(0, 0);
        const auto br = table->cellAt(table->rows() - 1, table->columns() - 1);
        if (!tl.isValid() || !br.isValid())
            continue;
        const QRectF first = layout->blockBoundingRect(tl.firstCursorPosition().block());
        const QRectF last  = layout->blockBoundingRect(br.lastCursorPosition().block());
        const auto   tlf   = tl.format().toTableCellFormat();
        const auto   brf   = br.format().toTableCellFormat();
        rects.push_back(QRectF(
            QPointF(first.left() - tlf.leftPadding(), first.top() - tlf.topPadding()),
            QPointF(last.right() + brf.rightPadding(), last.bottom() + brf.bottomPadding())
        ));
    }
    return rects;
}

// True for a code point that anchors an emoji grapheme (the pictographic blocks
// plus the symbol ranges that are predominantly emoji). Deliberately conservative
// so a lone CJK character or letter is never mistaken for an emoji.
static bool cpIsEmojiBase(char32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || // emoticons, pictographs, transport, symbols, flags
           (c >= 0x2600 && c <= 0x27BF) ||   // misc symbols + dingbats
           (c >= 0x2B00 && c <= 0x2BFF) ||   // misc symbols & arrows (⭐ ⬆ …)
           (c >= 0x2194 && c <= 0x21AA) ||   // arrows (↔ ↩ …)
           (c >= 0x231A && c <= 0x231B) ||   // ⌚ ⌛
           (c >= 0x23E9 && c <= 0x23FA) ||   // media controls (⏩ ⏰ …)
           (c >= 0x25AA && c <= 0x25FE) ||   // geometric shapes used as emoji
           c == 0x24C2 || c == 0x2934 || c == 0x2935 || c == 0x2122 || c == 0x2139 || c == 0x3030 ||
           c == 0x303D || c == 0x3297 || c == 0x3299;
}

// True for a code point that only ever continues an emoji grapheme (never starts
// one): joiners, variation selectors, skin-tone modifiers, keycap, flag tags.
static bool cpIsEmojiMod(char32_t c) {
    return c == 0x200D || c == 0xFE0F || c == 0xFE0E || c == 0x20E3 ||
           (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F);
}

// True when `s` is exactly one emoji — a single grapheme cluster (so flags,
// ZWJ families and keycaps each count as one) made only of emoji code points.
static bool isSingleEmoji(const QString &s) {
    if (s.isEmpty())
        return false;
    QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, s);
    bf.toStart();
    if (bf.toNextBoundary() != s.size()) // more than one grapheme cluster
        return false;

    const QList<uint> cps       = s.toUcs4();
    bool              hasKeycap = false, hasEmoji = false;
    for (uint c : cps) {
        if (c == 0x20E3)
            hasKeycap = true;
        if (cpIsEmojiBase(c))
            hasEmoji = true;
    }
    for (uint c : cps) {
        // Keycap emoji (#️⃣ *️⃣ 0️⃣–9️⃣) have an ASCII base — only valid alongside U+20E3.
        const bool keycapBase = hasKeycap && (c == '#' || c == '*' || (c >= '0' && c <= '9'));
        if (!cpIsEmojiBase(c) && !cpIsEmojiMod(c) && !keycapBase)
            return false;
    }
    return hasEmoji || hasKeycap;
}

// If `twe` is nothing but a single emoji (ignoring surrounding whitespace),
// returns it resolved; otherwise nullopt. Drives the jumbomoji size bump.
static std::optional<EmojiResolved> soleEmoji(const TextWithEntities &twe, const Session *session) {
    // A single :name: token parsed into one Emoji entity, nothing else around it.
    if (twe.entities.size() == 1 && twe.entities[0].type == EntityType::Emoji) {
        const auto &e = twe.entities[0];
        if (QStringView{twe.text}.left(e.offset).trimmed().isEmpty() &&
            QStringView{twe.text}.mid(e.offset + e.length).trimmed().isEmpty()) {
            const auto er = resolveEmojiRich(e.data, session);
            if (er.resolved)
                return er;
            return std::nullopt; // ":unknown:" is text — don't jumbo it
        }
    }
    // A raw unicode emoji typed directly (the mrkdwn parser leaves it as plain text).
    if (twe.entities.empty()) {
        const QString t = twe.text.trimmed();
        if (isSingleEmoji(t))
            return EmojiResolved{t, {}};
    }
    return std::nullopt;
}

// Build the full HTML for a message's main text doc (blocks preferred over text field).
// Offset where a reply's collapsible trailer (quoted history + a trailing
// signature) begins, or -1 if there's nothing to collapse. Heuristic, tuned on
// real mail: the trailer starts at the earliest of (a) a top-level blockquote
// that dominates the tail — longer than 40% of the text and ending past the 55%
// mark, i.e. quoted history rather than a short inline quote — and (b) an
// RFC-3676 "-- " signature delimiter on its own line in the second half.
static int quotedTrailerCut(const TextWithEntities &twe) {
    const QString &t = twe.text;
    const int      n = t.size();
    if (n < 200)
        return -1; // too short to be worth collapsing
    int cut = -1;

    // (a) dominant trailing blockquote (top-level only — skip nested ones)
    for (const auto &e : twe.entities) {
        if (e.type != EntityType::Blockquote)
            continue;
        bool nested = false;
        for (const auto &o : twe.entities)
            if (&o != &e && o.type == EntityType::Blockquote && o.offset <= e.offset &&
                o.offset + o.length >= e.offset + e.length && o.length > e.length) {
                nested = true;
                break;
            }
        if (nested)
            continue;
        if (e.length > 0.4 * n && (e.offset + e.length) > 0.55 * n && (cut < 0 || e.offset < cut))
            cut = e.offset;
    }

    // (b) signature delimiter on its own line: RFC-3676 "-- " plus the common
    // non-standard all-dashes variants ("---", "—"). Everything from there down is
    // the signature (and any quote beneath it). Take the earliest such line, but
    // never the very first line of the message. No latter-half restriction — a big
    // quote below the signature can push it into the first half of the text.
    {
        int ls = 0;
        for (int i = 0; i <= n; ++i)
            if (i == n || t[i] == '\n') {
                if (ls > 0 && (cut < 0 || ls < cut)) {
                    const QStringView line   = QStringView{t}.mid(ls, i - ls).trimmed();
                    bool              dashes = line.size() >= 2;
                    for (qsizetype k = 0; dashes && k < line.size(); ++k)
                        if (line[k] != QLatin1Char('-') && line[k] != QChar(0x2014))
                            dashes = false;
                    if (dashes)
                        cut = ls;
                }
                ls = i + 1;
            }
    }

    // (c) plain-text quoted block: a text/plain email body carries no Blockquote
    // entities — its quoted history is literal '>'-prefixed lines. Walk lines from
    // the end (skipping blanks) and take the topmost contiguous quoted line as the
    // cut; the unquoted "On … wrote:" attribution above it stays visible.
    {
        std::vector<int> starts{0};
        for (int i = 0; i < n; ++i)
            if (t[i] == '\n')
                starts.push_back(i + 1);
        auto end    = [&](int li) { return li + 1 < (int)starts.size() ? starts[li + 1] - 1 : n; };
        auto quoted = [&](int li) {
            int j = starts[li], e = end(li);
            while (j < e && (t[j] == ' ' || t[j] == '\t'))
                ++j;
            return j < e && t[j] == '>';
        };
        auto blank = [&](int li) {
            for (int j = starts[li], e = end(li); j < e; ++j)
                if (!t[j].isSpace())
                    return false;
            return true;
        };
        int topQuoted = -1;
        for (int li = (int)starts.size() - 1; li >= 0; --li) {
            if (quoted(li))
                topQuoted = li;
            else if (blank(li))
                continue; // blank lines inside/around the quote don't end it
            else
                break; // first unquoted line of real text → quote block starts below
        }
        if (topQuoted >= 0) {
            const int c = starts[topQuoted];
            if (c > 0 && (n - c) > 0.3 * n && (cut < 0 || c < cut))
                cut = c;
        }

        // (d) forwarded/replied header block (Outlook & localized clients): the
        // quoted original is introduced by a run of "From:/Sent:/To:/Subject:"
        // lines — there's no blockquote or '>' marking. Rather than match the
        // many localized labels ("From", "De", "Von", "发件人", "差出人", …), detect
        // it structurally and locale-independently: ≥3 consecutive "Label: value"
        // lines with at least one email address among them. Cut at the run start.
        auto headerLine = [&](int li) {
            int j = starts[li], e = end(li);
            while (j < e && (t[j] == ' ' || t[j] == '\t'))
                ++j;
            int colon = -1;
            for (int k = j; k < e && k < j + 31; ++k)
                if (t[k] == ':') {
                    colon = k;
                    break;
                }
            if (colon <= j)
                return false; // no (non-empty) label before a ':'
            int v = colon + 1;
            while (v < e && (t[v] == ' ' || t[v] == '\t'))
                ++v;
            return v < e; // a non-empty value follows
        };
        auto lineHasEmail = [&](int li) {
            for (int j = starts[li], e = end(li); j < e; ++j)
                if (t[j] == '@')
                    return true;
            return false;
        };
        int  runStart = -1, runLen = 0, dcut = -1;
        bool runHasEmail = false;
        for (int li = 0; li < (int)starts.size(); ++li) {
            if (headerLine(li)) {
                if (runLen == 0)
                    runStart = li;
                ++runLen;
                runHasEmail = runHasEmail || lineHasEmail(li);
            } else {
                if (runLen >= 3 && runHasEmail) {
                    dcut = starts[runStart];
                    break;
                }
                runLen      = 0;
                runHasEmail = false;
            }
        }
        if (dcut < 0 && runLen >= 3 && runHasEmail)
            dcut = starts[runStart];
        if (dcut > 0 && (cut < 0 || dcut < cut))
            cut = dcut;

        // Pull the cut up over the "On <date>, X <email> wrote:" attribution line
        // that introduces a quote (plus any blank line between it and the body),
        // so a stripped reply never ends on a dangling "… wrote:". Header-block
        // cuts (d) have no such line — the search simply finds none.
        if (cut > 0) {
            int li = 0;
            while (li < (int)starts.size() && starts[li] < cut)
                ++li;
            --li; // the line directly above the cut
            while (li > 0 && blank(li))
                --li;
            if (li > 0) {
                const QStringView ln =
                    QStringView{t}.mid(starts[li], end(li) - starts[li]).trimmed();
                const bool attribution =
                    lineHasEmail(li) ||
                    ((ln.endsWith(u':') || ln.endsWith(QChar(0xFF1A))) &&
                     ln.toString().contains(QStringLiteral("wrote"), Qt::CaseInsensitive));
                if (attribution)
                    cut = starts[li];
            }
        }
    }
    return cut;
}

// A sub-range of a TextWithEntities: substring [from,to) with each entity clipped
// to the window (offsets rebased to 0). Container entities that straddle the
// boundary are clipped too, so the body slice never carries a half-open table.
static TextWithEntities sliceEntities(const TextWithEntities &src, int from, int to) {
    const int n = src.text.size();
    from        = std::max(0, std::min(from, n));
    to          = std::max(from, std::min(to, n));
    TextWithEntities out;
    out.text = src.text.mid(from, to - from);
    for (const auto &e : src.entities) {
        const int s = std::max(e.offset, from);
        const int o = std::min(e.offset + e.length, to);
        if (o <= s)
            continue;
        TextEntity ne = e;
        ne.offset     = s - from;
        ne.length     = o - s;
        out.entities.push_back(ne);
    }
    return out;
}

static bool isLinkEntity(EntityType type) {
    return type == EntityType::Link || type == EntityType::MessageLink;
}

// Slack can omit a titled link from rich_text while retaining its URL in the
// fallback text. Only reuse spans when the text matches exactly; rich-text
// links, code and formatting remain authoritative. This also repairs cached
// messages without needing a refetch or showing their unfurl attachments.
static TextWithEntities
withFallbackLinks(const TextWithEntities &rich, const TextWithEntities &fallback) {
    auto merged = rich;
    if (rich.text != fallback.text)
        return merged;
    for (const auto &link : fallback.entities) {
        if (!isLinkEntity(link.type))
            continue;
        if (link.data.isEmpty() || link.offset < 0 || link.length <= 0 ||
            link.offset > rich.text.size() || link.length > rich.text.size() - link.offset)
            continue;
        // A bare permalink reaches rich_text as a plain `link` element, so the
        // block holds a URL anchor where the fallback parsed a message chip —
        // same span, same target. Take the chip (what the official client
        // shows); the block's URL carries nothing the chip lacks.
        if (link.type == EntityType::MessageLink) {
            const auto same = std::find_if(
                merged.entities.begin(), merged.entities.end(), [&](const TextEntity &entity) {
                    return entity.type == EntityType::Link && entity.offset == link.offset &&
                           entity.length == link.length &&
                           SlackLinks::refToToken(SlackLinks::parseMessageLink(entity.data)) ==
                               link.data;
                }
            );
            if (same != merged.entities.end()) {
                same->type = EntityType::MessageLink;
                same->data = link.data;
                continue;
            }
        }
        const int  end      = link.offset + link.length;
        const bool conflict = std::any_of(
            merged.entities.begin(), merged.entities.end(), [&](const TextEntity &entity) {
                const int otherEnd = entity.offset + entity.length;
                if (link.offset >= otherEnd || entity.offset >= end)
                    return false;
                // Never nest anchors or turn literal code into a link. Other
                // styles can nest, but crossing spans cannot form valid HTML.
                return entity.type == EntityType::Link || entity.type == EntityType::MessageLink ||
                       entity.type == EntityType::Code || entity.type == EntityType::Pre ||
                       !((link.offset <= entity.offset && end >= otherEnd) ||
                         (entity.offset <= link.offset && otherEnd >= end));
            }
        );
        if (!conflict)
            merged.entities.push_back(link);
    }
    return merged;
}

QString huddleDurationLabel(qint64 seconds) {
    // Slack rounds to whole minutes and never says "0m".
    const qint64 mins = std::max<qint64>(1, (seconds + 30) / 60);
    if (mins < 60)
        return QCoreApplication::translate("MsgRender", "%1m").arg(mins);
    const qint64 h = mins / 60, m = mins % 60;
    return m == 0 ? QCoreApplication::translate("MsgRender", "%1h").arg(h)
                  : QCoreApplication::translate("MsgRender", "%1h %2m").arg(h).arg(m);
}

QString huddleSummaryText(const Message &msg, const Session *session) {
    if (!msg.huddle)
        return {};
    const HuddleInfo &h  = *msg.huddle;
    const UserId      me = session ? session->meUserId() : UserId{};

    // "You" first, then the others in Slack's order; past three names the
    // rest collapse into "N others".
    bool        withMe = false;
    QStringList names;
    for (const auto &u : h.attendees) {
        if (!me.value.isEmpty() && u == me) {
            withMe = true;
            continue;
        }
        const User *usr = session ? session->findUser(u) : nullptr;
        names << (usr ? usr->displayLabel() : u.value);
    }
    if (withMe)
        names.prepend(QCoreApplication::translate("MsgRender", "You"));
    constexpr int kMaxNames = 3;
    if (names.size() > kMaxNames) {
        const int rest = int(names.size()) - (kMaxNames - 1);
        names          = names.mid(0, kMaxNames - 1);
        names << QCoreApplication::translate("MsgRender", "%n others", nullptr, rest);
    }
    QString who;
    if (names.size() == 1)
        who = names[0];
    else if (names.size() > 1)
        who = QCoreApplication::translate("MsgRender", "%1 and %2")
                  .arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")), names.back());

    // One person is grammatically singular — unless that person is "you".
    const bool plural = names.size() > 1 || withMe;
    if (!h.ended) {
        if (who.isEmpty())
            return QCoreApplication::translate(
                "MsgRender", "The huddle is waiting for people to join."
            );
        return plural ? QCoreApplication::translate("MsgRender", "%1 are in the huddle.").arg(who)
                      : QCoreApplication::translate("MsgRender", "%1 is in the huddle.").arg(who);
    }
    if (who.isEmpty())
        return QCoreApplication::translate("MsgRender", "Nobody joined the huddle.");
    if (h.startSec <= 0 || h.endSec < h.startSec)
        return plural ? QCoreApplication::translate("MsgRender", "%1 were in the huddle.").arg(who)
                      : QCoreApplication::translate("MsgRender", "%1 was in the huddle.").arg(who);
    const QString dur = huddleDurationLabel(h.endSec - h.startSec);
    return plural ? QCoreApplication::translate("MsgRender", "%1 were in the huddle for %2.")
                        .arg(who, dur)
                  : QCoreApplication::translate("MsgRender", "%1 was in the huddle for %2.")
                        .arg(who, dur);
}

QString buildMsgHtml(
    const Message          &msg,
    const Session          *session,
    const GifRenderContext *gif,
    bool                    collapseQuotedReplies
) {
    // A huddle row's body is a sentence about the call, not the stand-in text.
    if (isHuddleMessage(msg) && msg.huddle) {
        const QString line = huddleSummaryText(msg, session);
        return wrapParagraph(
            QStringLiteral("<span style='color:%1'>%2</span>")
                .arg(Th::qss(Th::c().text.secondary), line.toHtmlEscaped()),
            "margin:0"
        );
    }

    // Jumbomoji: a message that is nothing but a single emoji renders 50% larger,
    // matching the official client. The lone emoji arrives either as a one-block
    // rich_text payload or in the plain text field — check whichever applies.
    const TextWithEntities *solo = nullptr;
    if (msg.blocks.empty())
        solo = &msg.text;
    else if (msg.blocks.size() == 1 && msg.blocks[0].typeStr == "rich_text")
        solo = &msg.blocks[0].text;
    if (solo) {
        if (const auto er = soleEmoji(*solo, session))
            return "<p style='margin:0'>" + emojiHtml(*er, qRound(inlineEmojiPx() * 1.5)) + "</p>";
    }

    if (!msg.blocks.empty()) {
        QString    html;
        bool       anyImage      = false;
        // Most rich_text blocks mirror the fallback text; only copy one when the
        // fallback actually has a link to lend it.
        const bool fallbackLinks = std::any_of(
            msg.text.entities.begin(), msg.text.entities.end(), [](const TextEntity &e) {
                return isLinkEntity(e.type);
            }
        );
        for (int bi = 0; bi < (int)msg.blocks.size(); ++bi) {
            const auto &block = msg.blocks[bi];
            if (fallbackLinks && block.typeStr == "rich_text" && block.text.text == msg.text.text) {
                auto linked = block;
                linked.text = withFallbackLinks(block.text, msg.text);
                anyImage    = blockHtml(html, linked, session, gif, bi) || anyImage;
            } else {
                anyImage = blockHtml(html, block, session, gif, bi) || anyImage;
            }
        }
        // An embedded image block fully represents the message — never fall back
        // to the text field (it duplicates the alt text).
        if (!html.isEmpty() || anyImage)
            return html;
    }

    // Email: strip the trailing quoted history + signature. It's just the
    // previous message(s) already shown above in the conversation, and a chat
    // doesn't repeat them — what the sender actually added is the slice before
    // the cut. No "show quoted text" toggle: that isn't chat-like, and the quoted
    // original is the bubble right above. `collapseQuotedReplies` is the email
    // capability; chat services keep their (intentional) quotes whole.
    if (collapseQuotedReplies && msg.blocks.empty()) {
        int end = quotedTrailerCut(msg.text);
        if (end < 0)
            end = (int)msg.text.text.size();
        // Trim trailing blank lines/whitespace so the stripped body doesn't leave
        // a gap where the quote (or the email's trailing markup) used to be.
        while (end > 0 && msg.text.text[end - 1].isSpace())
            --end;
        if (end > 0 && end < (int)msg.text.text.size())
            return wrapParagraph(toHtml(sliceEntities(msg.text, 0, end), session), "margin:0");
    }

    return wrapParagraph(toHtml(msg.text, session), "margin:0");
}

// Escaped inline HTML with ONLY emoji entities substituted; every other entity
// renders as its plain text. For spots that can't take toHtml()'s full markup —
// a linked attachment title puts the whole run inside one <a>, where a nested
// anchor from a Link entity would be invalid.
static QString emojiOnlyHtml(const TextWithEntities &twe, const Session *session) {
    std::vector<const TextEntity *> emoji;
    for (const auto &e : twe.entities)
        if (e.type == EntityType::Emoji)
            emoji.push_back(&e);
    std::sort(emoji.begin(), emoji.end(), [](const TextEntity *a, const TextEntity *b) {
        return a->offset < b->offset;
    });
    QString out;
    int     pos = 0;
    for (const TextEntity *e : emoji) {
        if (e->offset < pos)
            continue; // defensive: skip any overlapping span
        out += QStringView{twe.text}.mid(pos, e->offset - pos).toString().toHtmlEscaped();
        out += emojiHtml(resolveEmojiRich(e->data, session), inlineEmojiPx());
        pos = e->offset + e->length;
    }
    out += QStringView{twe.text}.mid(pos).toString().toHtmlEscaped();
    return out;
}

// ── Shared-message unfurl card ────────────────────────────────────────────────

// Index to cut `text` at so it stays inside the char/line budget; -1 when it fits.
static int previewCut(const QString &text, int maxChars, int maxLines) {
    int lines = 1;
    for (int i = 0; i < text.size(); ++i) {
        if (i >= maxChars)
            return i;
        if (text[i] == QLatin1Char('\n') && ++lines > maxLines)
            return i;
    }
    return -1;
}

// The quoted message's body — the only part of a message-unfurl card that is a
// document. Its chrome (frame, author header, file chips) is painted by
// MessageListWidget with the same painters a real message row uses, so the card
// can't drift from the rest of the app; this only has to produce the text.
static QString
msgUnfurlHtml(const Attachment &att, const Session *session, const GifRenderContext *gif) {
    QString    html;
    const bool expanded = gif && gif->expanded && gif->expanded->contains(gif->keyPrefix);

    // The quoted message's blocks (its real content) or, lacking those, the
    // unfurl's flat text. Truncation runs across blocks on one shared budget.
    int  chars     = expanded ? std::numeric_limits<int>::max() : kUnfurlPreviewChars;
    int  lines     = expanded ? std::numeric_limits<int>::max() : kUnfurlPreviewLines;
    bool truncated = false;

    auto addText = [&](const TextWithEntities &twe) {
        const int cut = previewCut(twe.text, chars, lines);
        if (cut < 0) {
            html += wrapParagraph(toHtml(twe, session), "margin:2px 0");
            chars -= (int)twe.text.size();
            lines -= (int)twe.text.count(QLatin1Char('\n'));
            return;
        }
        truncated = true;
        if (cut > 0)
            html += wrapParagraph(
                toHtml(sliceEntities(twe, 0, cut), session) + "\xE2\x80\xA6", "margin:2px 0"
            );
        chars = 0;
        lines = 0;
    };

    if (!att.blocks.empty()) {
        for (int bi = 0; bi < (int)att.blocks.size() && !truncated; ++bi) {
            const Block &blk       = att.blocks[bi];
            // Section/rich_text/header text goes through the budget; images,
            // tables and button rows render whole (they're one visual unit).
            const bool   plainText = blk.typeStr != QLatin1String("image") &&
                                     blk.typeStr != QLatin1String("table") && blk.buttons.empty() &&
                                     !blk.text.text.isEmpty();
            if (plainText)
                addText(blk.text);
            else
                blockHtml(html, blk, session, gif, bi);
        }
    } else if (!att.text.text.isEmpty()) {
        addText(att.text);
    }

    // The toggle needs a host that owns the expand state; without one (preview
    // dialogs) the card just shows its preview.
    if ((truncated || expanded) && gif && gif->expanded)
        html += "<p style='margin:2px 0 0'><a href='" +
                (kUnfurlToggleAnchorPrefix + gif->keyPrefix).toHtmlEscaped() +
                "' style='color:" + Th::qss(Th::c().text.link) +
                ";font-weight:bold;text-decoration:none'>" +
                (expanded ? QCoreApplication::translate("MsgRender", "Show less")
                          : QCoreApplication::translate("MsgRender", "Show more")) +
                "</a></p>";
    return html;
}

// Attachment text HTML (used inside the colored bar area).
QString
buildAttachHtml(const Attachment &att, const Session *session, const GifRenderContext *gif) {
    // A quoted Slack message is a card of its own, not a link preview.
    if (att.isMsgUnfurl)
        return msgUnfurlHtml(att, session, gif);

    QString html;
    if (!att.pretext.isEmpty()) // pretext is mrkdwn, like text
        html += wrapParagraph(toHtml(MrkdwnParser::parse(att.pretext), session), "margin:0 0 2px");
    if (!att.authorName.isEmpty())
        html += "<p style='margin:0;font-size:0.85em;color:" + Th::qss(Th::c().text.tertiary) +
                "'>" + MrkdwnParser::decodeEntities(att.authorName).toHtmlEscaped() + "</p>";
    if (!att.title.isEmpty()) {
        // Titles don't support mrkdwn marks, but bots (e.g. Outlook Calendar)
        // embed <!date^…> and <url|label> tokens in them and Slack's clients
        // resolve those everywhere — so resolve tokens, not full mrkdwn.
        const TextWithEntities title =
            MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(att.title));
        if (!att.titleLink.isEmpty())
            // The whole title is one link; embedded tokens collapse to their
            // resolved plain text (anchors can't nest) — except emoji, which
            // substitute their glyph/img inline.
            html += "<p style='margin:0;font-weight:bold'><a href='" +
                    MrkdwnParser::decodeEntities(att.titleLink).toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().text.link) + ";text-decoration:none'>" +
                    emojiOnlyHtml(title, session) + "</a></p>";
        else
            html += "<p style='margin:0;font-weight:bold'>" + toHtml(title, session) + "</p>";
    }
    if (!att.text.text.isEmpty())
        html += wrapParagraph(toHtml(att.text, session), "margin:2px 0 0");

    // Key/value fields (classic bot format): bold title line, value below.
    for (const auto &f : att.fields) {
        if (!f.title.isEmpty())
            html += "<p style='margin:2px 0 0;font-weight:bold'>" +
                    MrkdwnParser::decodeEntities(f.title).toHtmlEscaped() + "</p>";
        if (!f.value.text.isEmpty())
            html += "<p style='margin:0'>" + toHtml(f.value, session) + "</p>";
    }

    // Render Block Kit blocks embedded in the attachment (modern bot format).
    bool anyImage = false;
    if (html.isEmpty() && !att.blocks.empty()) {
        for (int bi = 0; bi < (int)att.blocks.size(); ++bi)
            anyImage = blockHtml(html, att.blocks[bi], session, gif, bi) || anyImage;
    }

    // Last-resort fallback: parse as mrkdwn so any <url> links become clickable.
    // Skipped when an image block was embedded (it duplicates the alt text) or
    // when there are buttons to render (the fallback duplicates their purpose).
    if (html.isEmpty() && !anyImage && att.buttons.empty() && !att.fallback.isEmpty())
        html += "<p style='margin:2px 0 0'>" + toHtml(MrkdwnParser::parse(att.fallback), session) +
                "</p>";

    // Legacy attachment "actions" buttons render after text/fields, like Slack.
    html += buttonsHtml(att.buttons);

    // Footer always renders last, after whichever content variant was chosen.
    // Like Slack's: "[icon] footer | time", where the footer text carries the
    // same <url|label>/<!date> tokens a title does (GitHub links the repo name
    // there) and `ts` is the bot-supplied timestamp.
    // Sizes are absolute px, not em: Qt re-resolves an anchor's font from the
    // document default, so an em-sized paragraph rendered its link a size larger
    // than the plain text beside it.
    if (!att.footer.isEmpty() || att.msgDate > 0) {
        const QColor  fg   = Th::c().text.secondary; // Slack: #616061, links too
        const QString px   = QString::number(footerFontPx());
        const QString span = "<span style='font-size:" + px + "px;color:" + Th::qss(fg) + "'>";
        QString       inner;
        if (!att.footerIcon.isEmpty()) {
            const QString s = QString::number(footerIconPx());
            inner += "<img src='" + att.footerIcon.toHtmlEscaped() + "' width='" + s +
                     "' height='" + s + "' style='vertical-align:middle'>" + span + "&nbsp;</span>";
        }
        if (!att.footer.isEmpty())
            inner += span +
                     toHtml(
                         MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(att.footer)),
                         session,
                         InlineStyle{.linkColor = fg, .fontPx = footerFontPx()}
                     ) +
                     "</span>";
        if (att.msgDate > 0) {
            if (!att.footer.isEmpty())
                inner += "<span style='font-size:" + px +
                         "px;color:" + Th::qss(Th::c().text.tertiary) + "'>&nbsp;|&nbsp;</span>";
            inner += span + formatFooterTs(att.msgDate).toHtmlEscaped() + "</span>";
        }
        html += "<p style='margin:5px 0 0;font-size:" + px + "px;color:" + Th::qss(fg) + "'>" +
                inner + "</p>";
    }

    return html;
}

bool attachIsImageOnly(const Attachment &att) {
    const bool hasImageBlock =
        std::any_of(att.blocks.begin(), att.blocks.end(), [](const Block &b) {
            return b.typeStr == "image" && !b.imageUrl.isEmpty();
        });
    if (!hasImageBlock)
        return false;
    if (!att.pretext.isEmpty() || !att.authorName.isEmpty() || !att.title.isEmpty() ||
        !att.text.text.isEmpty() || !att.fields.empty() || !att.footer.isEmpty() ||
        !att.imageUrl.isEmpty() || !att.thumbUrl.isEmpty() || !att.buttons.empty())
        return false;
    for (const auto &b : att.blocks)
        if (b.typeStr != "image" && (b.typeStr == "divider" || !b.text.text.isEmpty() ||
                                     !b.buttons.empty() || !b.tableRows.empty()))
            return false;
    return true;
}

bool attachIsTableOnly(const Attachment &att) {
    const bool hasTableBlock =
        std::any_of(att.blocks.begin(), att.blocks.end(), [](const Block &b) {
            return !b.tableRows.empty();
        });
    if (!hasTableBlock)
        return false;
    if (!att.pretext.isEmpty() || !att.authorName.isEmpty() || !att.title.isEmpty() ||
        !att.text.text.isEmpty() || !att.fields.empty() || !att.footer.isEmpty() ||
        !att.imageUrl.isEmpty() || !att.thumbUrl.isEmpty() || !att.buttons.empty())
        return false;
    for (const auto &b : att.blocks)
        if (b.tableRows.empty() && (b.typeStr == "divider" || b.typeStr == "image" ||
                                    !b.text.text.isEmpty() || !b.buttons.empty()))
            return false;
    return true;
}

bool attachIsBarless(const Attachment &att) {
    return att.isMsgUnfurl || attachIsImageOnly(att) || attachIsTableOnly(att);
}

QColor fileTypeColor(const File &f) {
    const QString mt = f.mimeType.toLower();
    if (mt.contains("pdf"))
        return QColor("#E44D4D");
    if (mt.contains("word") || mt.contains("document"))
        return QColor("#2B579A");
    if (mt.contains("excel") || mt.contains("spreadsheet"))
        return QColor("#217346");
    if (mt.contains("powerpoint") || mt.contains("presentation"))
        return QColor("#D24726");
    if (mt.startsWith("video/"))
        return QColor("#7B2D8B");
    if (mt.startsWith("audio/"))
        return QColor("#1E7A6E");
    if (mt.contains("zip") || mt.contains("x-tar") || mt.contains("gzip") || mt.contains("x-7z") ||
        mt.contains("x-rar"))
        return QColor("#8B6914");
    if (mt.startsWith("text/") || mt.contains("json") || mt.contains("xml"))
        return QColor("#555555");
    return QColor("#888888");
}

QString fileIconLabel(const File &f) {
    const int dot = f.name.lastIndexOf('.');
    if (dot >= 0) {
        const QString ext = f.name.mid(dot + 1);
        if (ext.size() >= 1 && ext.size() <= 5)
            return ext.toUpper().left(4);
    }
    if (!f.prettyType.isEmpty())
        return f.prettyType.left(4).toUpper();
    return "FILE";
}

QString formatFileSize(qint64 bytes) {
    if (bytes <= 0)
        return {};
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(bytes / 1024);
    const double mb = bytes / (1024.0 * 1024.0);
    return QString("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
}

namespace {

constexpr int kChipIconW  = 48;
constexpr int kChipPadX   = 12;
constexpr int kChipRadius = 4;

// Audio card: round play button + title block on top, slider row underneath.
constexpr int kAudioPad      = 12;
constexpr int kAudioBtn      = 36; // play/pause circle
constexpr int kAudioKnob     = 12;
constexpr int kAudioBarH     = 4;
constexpr int kAudioRadius   = 8;
constexpr int kAudioAction   = 28; // "Transcribe" button (square hit area, round hover)
constexpr int kAudioLabelGap = 6;  // time label → action button

QRect clampChip(const QRect &rect, const File &f) {
    return QRect(rect.x(), rect.y(), std::min(rect.width(), kFileChipMaxW), fileChipHeight(f));
}
QRect clampChip(const QRect &rect, int h) {
    return QRect(rect.x(), rect.y(), std::min(rect.width(), kFileChipMaxW), h);
}

QFont chipNameFont() {
    QFont f = QApplication::font();
    f.setBold(true);
    return f;
}
QFont chipSubFont() {
    QFont f = QApplication::font();
    f.setPointSizeF(f.pointSizeF() * 0.82);
    return f;
}

// Text column layout of the plain (non-audio) chip.
struct ChipText {
    QRect chip; // clamped chip rect
    int   textX, textW;
    QFont nameFont, subFont;
    int   nameTop, subTop, subH;
};

ChipText chipText(const QRect &rect) {
    ChipText t;
    t.chip     = clampChip(rect, kFileChipH);
    t.textX    = t.chip.x() + kChipIconW + kChipPadX;
    t.textW    = t.chip.width() - kChipIconW - kChipPadX - 8;
    t.nameFont = chipNameFont();
    t.subFont  = chipSubFont();
    const QFontMetrics nameFm(t.nameFont), subFm(t.subFont);
    const int          totalTextH = nameFm.height() + 3 + subFm.height();
    t.nameTop                     = t.chip.y() + (kFileChipH - totalTextH) / 2;
    t.subTop                      = t.nameTop + nameFm.height() + 3;
    t.subH                        = subFm.height();
    return t;
}

// Width reserved for the slider row's time label: the clip's length, or the
// widest "m:ss" when it is unknown, so the track never resizes mid-play.
int audioTimeLabelW(const QFontMetrics &fm, qint64 durationMs) {
    const QString sample =
        durationMs > 0 ? formatDuration(durationMs, true) : QStringLiteral("0:00");
    return std::max(fm.horizontalAdvance(sample), fm.horizontalAdvance(QStringLiteral("0:00")));
}

void paintPlainChip(QPainter &p, const File &f, const QRect &rect) {
    const ChipText t        = chipText(rect);
    const QRect   &chipRect = t.chip;

    // Clipped fill: background + colored icon column
    QPainterPath clipPath;
    clipPath.addRoundedRect(QRectF(chipRect), kChipRadius, kChipRadius);
    p.save();
    p.setClipPath(clipPath);
    p.fillRect(chipRect, Th::c().message.fileChipBg);
    p.fillRect(QRect(chipRect.x(), chipRect.y(), kChipIconW, kFileChipH), fileTypeColor(f));
    p.restore();

    // Border
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Th::c().message.fileChipBorder);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(chipRect), kChipRadius, kChipRadius);
    p.restore();

    // Extension label centered in icon column
    {
        QFont iconFont = QApplication::font();
        iconFont.setBold(true);
        iconFont.setPointSizeF(iconFont.pointSizeF() * 0.72);
        p.save();
        p.setFont(iconFont);
        p.setPen(Qt::white);
        p.drawText(
            QRect(chipRect.x(), chipRect.y(), kChipIconW, kFileChipH),
            Qt::AlignCenter,
            fileIconLabel(f)
        );
        p.restore();
    }

    // Filename + subtitle (type · size) vertically centred in text column
    const QFontMetrics nameFm(t.nameFont);
    p.save();
    p.setFont(t.nameFont);
    p.setPen(Th::c().text.primary);
    p.drawText(
        QRect(t.textX, t.nameTop, t.textW, nameFm.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        nameFm.elidedText(f.name, Qt::ElideRight, t.textW)
    );
    QString       sub = f.prettyType;
    const QString sz  = formatFileSize(f.size);
    if (!sz.isEmpty())
        sub += (sub.isEmpty() ? "" : " · ") + sz;
    if (!sub.isEmpty()) {
        p.setFont(t.subFont);
        p.setPen(Th::c().text.secondary);
        p.drawText(
            QRect(t.textX, t.subTop, t.textW, t.subH), Qt::AlignLeft | Qt::AlignVCenter, sub
        );
    }
    p.restore();
}

void paintAudioCard(QPainter &p, const File &f, const QRect &rect, const AudioChipState *audio) {
    using Phase = AudioChipState::Phase;
    static const AudioChipState kIdle;
    if (!audio)
        audio = &kIdle;
    const QRect chip = clampChip(rect, kAudioChipH);
    const QRect btn  = audioChipButtonRect(chip);
    const QRect bar =
        audioChipBarRect(chip, audio->durationMs > 0 ? audio->durationMs : f.durationMs);

    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    // Card
    p.setPen(Th::c().message.fileChipBorder);
    p.setBrush(Th::c().message.fileChipBg);
    p.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), kAudioRadius, kAudioRadius);

    // Icons — re-baked on DPR/theme change (never a bare static — see .rules).
    static const QSize kGlyphSz(16, 16);
    static qreal       kDpr = 0;
    static QColor      kAccent, kMuted;
    static QPixmap     kPlay, kPause, kCaptionsAccent, kCaptionsMuted;
    const QColor       accent = Th::c().accent.def;
    const QColor       muted  = Th::c().text.secondary;
    if (const qreal d = p.device()->devicePixelRatioF();
        !qFuzzyCompare(d, kDpr) || accent != kAccent || muted != kMuted) {
        kDpr            = d;
        kAccent         = accent;
        kMuted          = muted;
        kPlay           = svgPixmapPhys(":/ui/play.svg", kGlyphSz, accent, d);
        kPause          = svgPixmapPhys(":/ui/pause.svg", kGlyphSz, accent, d);
        kCaptionsAccent = svgPixmapPhys(":/ui/captions.svg", kGlyphSz, accent, d);
        kCaptionsMuted  = svgPixmapPhys(":/ui/captions.svg", kGlyphSz, muted, d);
    }

    // Round play/pause button
    const bool playing = audio->phase == Phase::Playing;
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().accent.subtleBg);
    p.drawEllipse(QRectF(btn));
    {
        const QPixmap &glyph = playing ? kPause : kPlay;
        const int      gx = btn.left() + (btn.width() - kGlyphSz.width()) / 2 + (playing ? 0 : 1);
        const int      gy = btn.top() + (btn.height() - kGlyphSz.height()) / 2;
        p.drawPixmap(gx, gy, glyph);
    }

    // Title block: name, then "0:05 (79 KB)" / Loading… / error
    const QFont        nameFont = chipNameFont(), subFont = chipSubFont();
    const QFontMetrics nameFm(nameFont), subFm(subFont);
    const int          textX = btn.right() + 1 + kAudioPad;
    const int          textW = chip.right() - textX - kAudioPad + 1;
    p.setFont(nameFont);
    p.setPen(Th::c().text.primary);
    p.drawText(
        QRect(textX, btn.top() - 1, textW, nameFm.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        nameFm.elidedText(f.name, Qt::ElideRight, textW)
    );
    QString sub;
    QColor  subColor = Th::c().text.secondary;
    if (audio->phase == Phase::Error) {
        sub      = audio->error;
        subColor = Th::c().danger.text;
    } else if (audio->phase == Phase::Loading) {
        sub = QCoreApplication::translate("MsgRender", "Loading…");
    } else {
        const qint64  dur = audio->durationMs > 0 ? audio->durationMs : f.durationMs;
        const QString sz  = formatFileSize(f.size);
        if (dur > 0)
            sub = formatDuration(dur, true) +
                  (sz.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(sz));
        else
            sub = sz.isEmpty() ? f.prettyType : sz;
    }
    p.setFont(subFont);
    p.setPen(subColor);
    p.drawText(
        QRect(textX, btn.top() - 1 + nameFm.height() + 2, textW, subFm.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        subFm.elidedText(sub, Qt::ElideRight, textW)
    );

    // Slider: track, played part, knob
    const qint64 dur  = audio->durationMs > 0 ? audio->durationMs : f.durationMs;
    const bool   live = audio->phase == Phase::Playing || audio->phase == Phase::Paused ||
                        audio->phase == Phase::Ended || audio->scrubMs >= 0;
    qint64       pos  = audio->scrubMs >= 0 ? audio->scrubMs : audio->positionMs;
    if (audio->phase == Phase::Ended && audio->scrubMs < 0)
        pos = dur;
    if (!live)
        pos = 0;
    const qreal frac = dur > 0 ? std::clamp((qreal)pos / (qreal)dur, 0.0, 1.0) : 0.0;
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().message.fileChipBorder);
    p.drawRoundedRect(QRectF(bar), kAudioBarH / 2.0, kAudioBarH / 2.0);
    const qreal knobX = bar.left() + bar.width() * frac;
    if (frac > 0) {
        QRectF fill(bar);
        fill.setRight(knobX);
        p.setBrush(Th::c().accent.def);
        p.drawRoundedRect(fill, kAudioBarH / 2.0, kAudioBarH / 2.0);
    }
    p.setBrush(live ? Th::c().accent.def : Th::c().text.secondary);
    p.drawEllipse(QPointF(knobX, bar.center().y() + 0.5), kAudioKnob / 2.0, kAudioKnob / 2.0);

    // Time: elapsed while live, the clip's length otherwise
    QString label;
    if (audio->phase == Phase::Ended && audio->scrubMs < 0)
        label = dur > 0 ? formatDuration(dur, true) : QString();
    else if (live)
        label = formatDuration(pos);
    else if (dur > 0)
        label = formatDuration(dur, true);
    const QRect action = audioChipTranscribeRect(chip);
    p.setFont(subFont);
    p.setPen(Th::c().text.secondary);
    p.drawText(
        QRect(
            bar.right() + 1,
            bar.center().y() - kAudioBtn / 2,
            action.left() - kAudioLabelGap - bar.right() - 1,
            kAudioBtn
        ),
        Qt::AlignRight | Qt::AlignVCenter,
        label
    );

    // "Transcribe" button: muted glyph, accent on hover / while the AI works
    {
        const bool lit = audio->transcribeHovered || audio->transcribing;
        if (lit) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().accent.subtleBg);
            p.drawEllipse(QRectF(action));
        }
        const QPixmap &glyph = lit ? kCaptionsAccent : kCaptionsMuted;
        p.drawPixmap(
            action.left() + (action.width() - kGlyphSz.width()) / 2,
            action.top() + (action.height() - kGlyphSz.height()) / 2,
            glyph
        );
    }

    // Transcript line under the card
    if (f.hasTranscript()) {
        const AudioTranscriptLayout tl = audioChipTranscriptLayout(rect, f);
        const QFontMetrics          fm(QApplication::font());
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().divider.def);
        p.drawRoundedRect(QRectF(chip.x(), tl.textRect.y(), 3, tl.textRect.height()), 1.5, 1.5);
        p.setFont(QApplication::font());
        p.setPen(Th::c().text.secondary);
        p.drawText(
            tl.textRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            fm.elidedText(f.transcriptPreview.simplified(), Qt::ElideRight, tl.textRect.width())
        );
        p.setPen(Th::c().text.link);
        p.drawText(
            tl.linkRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            QCoreApplication::translate("MsgRender", "View transcript")
        );
    }
    p.restore();
}

} // namespace

QString formatDuration(qint64 ms, bool round) {
    const qint64  s   = std::max<qint64>(0, round ? (ms + 500) / 1000 : ms / 1000);
    const qint64  h   = s / 3600;
    const qint64  m   = (s / 60) % 60;
    const qint64  sec = s % 60;
    const QString ms2 = QStringLiteral("%1:%2")
                            .arg(m, h > 0 ? 2 : 1, 10, QLatin1Char('0'))
                            .arg(sec, 2, 10, QLatin1Char('0'));
    return h > 0 ? QStringLiteral("%1:%2").arg(h).arg(ms2) : ms2;
}

QRect audioChipButtonRect(const QRect &chipRect) {
    const QRect c = clampChip(chipRect, kAudioChipH);
    return QRect(c.x() + kAudioPad, c.y() + kAudioPad, kAudioBtn, kAudioBtn);
}

// Vertical centre of the slider row: centred in the band under the title block.
static int audioRowMid(const QRect &c) {
    return c.bottom() + 1 - kAudioPad - kAudioBtn / 2 + 4;
}

QRect audioChipBarRect(const QRect &chipRect, qint64 durationMs) {
    const QRect        c = clampChip(chipRect, kAudioChipH);
    const QFontMetrics subFm(chipSubFont());
    const int          labelW = audioTimeLabelW(subFm, durationMs);
    const int          x      = c.x() + kAudioPad + kAudioKnob / 2;
    const int          right  = audioChipTranscribeRect(c).left() - kAudioLabelGap - labelW - 12;
    const int          rowMid = audioRowMid(c);
    return QRect(x, rowMid - kAudioBarH / 2, std::max(20, right - x), kAudioBarH);
}

QRect audioChipTranscribeRect(const QRect &chipRect) {
    const QRect c = clampChip(chipRect, kAudioChipH);
    return QRect(
        c.right() + 1 - kAudioPad + 4 - kAudioAction,
        audioRowMid(c) - kAudioAction / 2,
        kAudioAction,
        kAudioAction
    );
}

AudioTranscriptLayout audioChipTranscriptLayout(const QRect &chipRect, const File &f) {
    if (!f.hasTranscript())
        return {};
    const QRect        c = clampChip(chipRect, kAudioChipH);
    const QFontMetrics fm(QApplication::font());
    const QString      link  = QCoreApplication::translate("MsgRender", "View transcript");
    const int          linkW = fm.horizontalAdvance(link);
    const int          top   = c.bottom() + 1 + (kTranscriptH - fm.height()) / 2;
    const int          textX = c.x() + 3 + kAudioPad;
    const int          avail = c.right() + 1 - textX - linkW - 6;
    const int textW = std::min(avail, fm.horizontalAdvance(f.transcriptPreview.simplified()));
    AudioTranscriptLayout tl;
    tl.textRect = QRect(textX, top, std::max(0, textW), fm.height());
    tl.linkRect = QRect(tl.textRect.right() + 1 + 6, top, linkW, fm.height());
    return tl;
}

std::vector<VttCue> parseVtt(const QByteArray &vtt) {
    // WEBVTT header, blank line, then cues: optional id line, "hh:mm:ss.mmm -->
    // hh:mm:ss.mmm", payload lines until a blank line.
    static const QRegularExpression kTiming(R"(^(?:(\d+):)?(\d{1,2}):(\d{2})\.(\d{3})\s*-->)");
    static const QRegularExpression kTag("<[^>]*>"); // <v Speaker>, <c>, <i>…
    static const QRegularExpression kEol("\\r?\\n");
    std::vector<VttCue>             cues;
    QString                         text = QString::fromUtf8(vtt);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    const QStringList lines = text.split(kEol);
    for (int i = 0; i < lines.size(); ++i) {
        const auto m = kTiming.match(lines[i]);
        if (!m.hasMatch())
            continue;
        VttCue cue;
        cue.startMs = ((m.captured(1).toLongLong() * 60 + m.captured(2).toLongLong()) * 60 +
                       m.captured(3).toLongLong()) *
                          1000 +
                      m.captured(4).toLongLong();
        QStringList payload;
        for (++i; i < lines.size() && !lines[i].trimmed().isEmpty(); ++i) {
            QString l = lines[i].trimmed();
            l.remove(kTag);
            if (l.startsWith(QLatin1String("- ")))
                l.remove(0, 2);
            payload << l;
        }
        cue.text = payload.join(' ').simplified();
        if (!cue.text.isEmpty())
            cues.push_back(cue);
    }
    return cues;
}

void paintFileChip(QPainter &p, const File &f, const QRect &rect, const AudioChipState *audio) {
    if (f.isAudio())
        paintAudioCard(p, f, rect, audio);
    else
        paintPlainChip(p, f, rect);
}

void configurePreviewBrowser(QTextBrowser *browser) {
    if (!browser)
        return;
    const auto &sp = Th::c().spacing;

    browser->setReadOnly(true);
    browser->setFrameShape(QFrame::NoFrame);
    browser->setOpenLinks(false);
    browser->setFocusPolicy(Qt::NoFocus); // read-only preview shouldn't grab focus
    // Thin rounded scrollbar matching the chats list thumb (4px / radius 2).
    browser->setStyleSheet(
        QStringLiteral("QTextBrowser { background: transparent; }") + Th::scrollBarQss(4, 2)
    );

    // Asymmetric text padding via the root frame (documentMargin is symmetric and
    // can't do this): left sp.lg so the text lines up with the card header, right 0
    // so the content reaches the edge with only the thin scrollbar beside it.
    browser->document()->setDocumentMargin(0);
    QTextFrameFormat fmt = browser->document()->rootFrame()->frameFormat();
    fmt.setLeftMargin(sp.lg);
    fmt.setRightMargin(0);
    fmt.setTopMargin(0);
    fmt.setBottomMargin(0);
    browser->document()->rootFrame()->setFrameFormat(fmt);
}

} // namespace MsgRender
