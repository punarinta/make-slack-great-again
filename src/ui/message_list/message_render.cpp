// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_render.h"
#include "session/session.h"
#include "text/mrkdwn_parser.h"
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
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextBrowser>
#include <QTextFrame>
#include <QTextTable>
#include <optional>

namespace MsgRender {

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
        const bool    baseIsGlyph = !base.unicode.isEmpty() && base.unicode != ":" + baseName + ":";
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
    return {unicode, {}};
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
    const QString s = QString::number(px);
    if (!er.imageUrl.isEmpty())
        return "<img src='" + er.imageUrl.toHtmlEscaped() + "' width='" + s + "' height='" + s +
               "'>";
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

QStringList collectEmojiImageUrls(const Message &msg, const Session *session) {
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
        }
    };
    addFrom(msg.text);
    for (const auto &b : msg.blocks) {
        addFrom(b.text);
        addBlockImage(b);
    }
    for (const auto &att : msg.attachments) {
        if (!att.pretext.isEmpty()) // pretext is parsed as mrkdwn at render time
            addFrom(MrkdwnParser::parse(att.pretext));
        if (!att.title.isEmpty()) // title is token-resolved at render time
            addFrom(MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(att.title)));
        addFrom(att.text);
        for (const auto &f : att.fields)
            addFrom(f.value);
        for (const auto &b : att.blocks) {
            addFrom(b.text);
            addBlockImage(b);
        }
    }
    return out;
}

// These take Message::date (epoch microseconds) — the single orderable/display
// time field — so the UI never parses a ts string as a clock.
QString formatTs(qint64 dateMicros) {
    return TimeFmt::formatTime(dateMicros / 1000000);
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
// recovers the real "#general" the official client shows. `fallback` is the
// parser's baked text, used when the channel isn't in the cache.
static QString
resolveChannelImpl(const QString &channelId, const QString &fallback, const Session *session) {
    if (session) {
        const auto *c = session->findConversation(ConversationId{channelId});
        if (c && !c->name.isEmpty())
            return "#" + c->name;
    }
    return fallback;
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
        case EntityType::Emoji: {
            const auto    er    = resolveEmojiRich(e.data, session);
            const QString glyph = er.unicode.isEmpty() ? (":" + e.data + ":") : er.unicode;
            repls.push_back({e.offset, e.length, glyph});
            break;
        }
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
    int                                  quoteDepth = 0
) {
    QString html;
    int     pos = start;
    for (int idx : nodes) {
        const auto &e = ents[idx];
        if (e.offset > pos)
            html += escapeAndBr(text.mid(pos, e.offset - pos));
        const auto rawInner  = text.mid(e.offset, e.length);
        const bool container = e.type == EntityType::Bold || e.type == EntityType::Italic ||
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
        case EntityType::Link:
            html += "<a href='" + e.data.toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().text.link) + ";text-decoration:none'>" +
                    inner + "</a>";
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
QString toHtml(const TextWithEntities &twe, const Session *session) {
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
    return renderRange(twe.text, 0, twe.text.size(), roots, sorted, kids, session);
}

static void collectCodeTables(QTextFrame *frame, QVector<QTextTable *> &out) {
    for (auto it = frame->begin(); it != frame->end(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            // Code blocks are the only single-column percentage-width tables toHtml
            // emits (blockquotes are two-column, fixed width).
            if (table->columns() == 1 &&
                table->format().width().type() == QTextLength::PercentageLength)
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

// Bot buttons as a row of real-looking buttons. Each button is a table cell;
// the rounded border + background are painted underneath by
// paintBotButtonChrome() (Qt rich text can't do border-radius), which finds
// these tables via the kBotBtnCellSpacing marker. Every button is an anchor so
// it's hit-testable and gets the pointing cursor: URL buttons open their URL,
// interactive-only ones use the msga://botbtn/ scheme — clicking shows why the
// action can't be delivered (see BotButton).
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
        cells += "<td style='padding:4px 12px'><a href='" + href.toHtmlEscaped() +
                 "' style='color:" + Th::qss(fg) + ";font-weight:bold;text-decoration:none'>" +
                 btn.text.toHtmlEscaped() + "</a></td>";
    }
    return "<table cellspacing='" + QString::number(kBotBtnCellSpacing) +
           "' cellpadding='0' style='margin:4px 0 2px'><tr>" + cells + "</tr></table>";
}

static void collectButtonTables(QTextFrame *frame, QVector<QTextTable *> &out) {
    for (auto it = frame->begin(); it != frame->end(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            // Button rows are the only tables buttonsHtml emits with this exact
            // cell spacing (code blocks and blockquotes use 0).
            if (qRound(table->format().cellSpacing()) == kBotBtnCellSpacing)
                out.push_back(table);
        }
        collectButtonTables(child, out);
    }
}

QVector<QRectF> botButtonRects(const QTextDocument *doc) {
    QVector<QTextTable *> tables;
    collectButtonTables(doc->rootFrame(), tables);
    QVector<QRectF> rects;
    auto           *layout = doc->documentLayout();
    for (QTextTable *table : tables) {
        for (int col = 0; col < table->columns(); ++col) {
            // Same approach as codeBlockRects: rebuild each cell's rect from its
            // block geometry + paddings (frameBoundingRect is unusable for tables).
            const auto   cell  = table->cellAt(0, col);
            const QRectF first = layout->blockBoundingRect(cell.firstCursorPosition().block());
            const QRectF last  = layout->blockBoundingRect(cell.lastCursorPosition().block());
            const auto   cf    = cell.format().toTableCellFormat();
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
    } else {
        if (!blk.text.text.isEmpty())
            html += wrapParagraph(toHtml(blk.text, session), "margin:2px 0");
        html += buttonsHtml(blk.buttons);
    }
    return false;
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
            QStringView{twe.text}.mid(e.offset + e.length).trimmed().isEmpty())
            return resolveEmojiRich(e.data, session);
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

QString buildMsgHtml(
    const Message          &msg,
    const Session          *session,
    const GifRenderContext *gif,
    bool                    collapseQuotedReplies
) {
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
        QString html;
        bool    anyImage = false;
        for (int bi = 0; bi < (int)msg.blocks.size(); ++bi)
            anyImage = blockHtml(html, msg.blocks[bi], session, gif, bi) || anyImage;
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

// Attachment text HTML (used inside the colored bar area).
QString
buildAttachHtml(const Attachment &att, const Session *session, const GifRenderContext *gif) {
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
            // resolved plain text (anchors can't nest).
            html += "<p style='margin:0;font-weight:bold'><a href='" +
                    MrkdwnParser::decodeEntities(att.titleLink).toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().text.link) + ";text-decoration:none'>" +
                    title.text.toHtmlEscaped() + "</a></p>";
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
    if (!att.footer.isEmpty())
        html += "<p style='margin:4px 0 0;font-size:0.8em;color:" + Th::qss(Th::c().text.tertiary) +
                "'>" + MrkdwnParser::decodeEntities(att.footer).toHtmlEscaped() + "</p>";

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
        if (b.typeStr != "image" &&
            (b.typeStr == "divider" || !b.text.text.isEmpty() || !b.buttons.empty()))
            return false;
    return true;
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

void paintFileChip(QPainter &p, const File &f, const QRect &rect) {
    static constexpr int kIconW  = 48;
    static constexpr int kPadX   = 12;
    static constexpr int kRadius = 4;

    const int   chipW = std::min(rect.width(), kFileChipMaxW);
    const QRect chipRect(rect.x(), rect.y(), chipW, kFileChipH);

    // Clipped fill: background + colored icon column
    QPainterPath clipPath;
    clipPath.addRoundedRect(QRectF(chipRect), kRadius, kRadius);
    p.save();
    p.setClipPath(clipPath);
    p.fillRect(chipRect, Th::c().message.fileChipBg);
    p.fillRect(QRect(chipRect.x(), chipRect.y(), kIconW, kFileChipH), fileTypeColor(f));
    p.restore();

    // Border
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Th::c().message.fileChipBorder);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(chipRect), kRadius, kRadius);
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
            QRect(chipRect.x(), chipRect.y(), kIconW, kFileChipH), Qt::AlignCenter, fileIconLabel(f)
        );
        p.restore();
    }

    // Filename + subtitle (type · size) vertically centred in text column
    const int textX = chipRect.x() + kIconW + kPadX;
    const int textW = chipW - kIconW - kPadX - 8;

    QFont nameFont = QApplication::font();
    nameFont.setBold(true);
    const QFontMetrics nameFm(nameFont);

    QFont subFont = QApplication::font();
    subFont.setPointSizeF(subFont.pointSizeF() * 0.82);
    const QFontMetrics subFm(subFont);

    const int totalTextH = nameFm.height() + 3 + subFm.height();
    const int textTop    = chipRect.y() + (kFileChipH - totalTextH) / 2;

    p.save();
    p.setFont(nameFont);
    p.setPen(Th::c().text.primary);
    p.drawText(
        QRect(textX, textTop, textW, nameFm.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        nameFm.elidedText(f.name, Qt::ElideRight, textW)
    );

    QString       sub = f.prettyType;
    const QString sz  = formatFileSize(f.size);
    if (!sz.isEmpty())
        sub += (sub.isEmpty() ? "" : " · ") + sz;
    if (!sub.isEmpty()) {
        p.setFont(subFont);
        p.setPen(Th::c().text.secondary);
        p.drawText(
            QRect(textX, textTop + nameFm.height() + 3, textW, subFm.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            sub
        );
    }
    p.restore();
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
