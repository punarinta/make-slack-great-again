// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_render.h"
#include "session/session.h"
#include "text/mrkdwn_parser.h"
#include "ui/theme.h"
#include "util/emoji.h"
#include "util/emoji_font.h"
#include "util/time_format.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFontInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QSet>

namespace MsgRender {

QString resolveEmoji(const QString &name) {
    return Emoji::fromName(name);
}

EmojiResolved resolveEmojiRich(const QString &name, const QHash<QString, QString> &customMap) {
    const QString unicode = Emoji::fromName(name);
    if (unicode != ":" + name + ":")
        return {unicode, {}};
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
    addFrom(msg.text);
    for (const auto &b : msg.blocks)
        addFrom(b.text);
    for (const auto &att : msg.attachments) {
        addFrom(att.text);
        for (const auto &f : att.fields)
            addFrom(f.value);
        for (const auto &b : att.blocks)
            addFrom(b.text);
    }
    return out;
}

QString formatTs(const Ts &ts) {
    bool   ok   = false;
    double secs = ts.toDouble(&ok);
    if (!ok)
        return ts;
    return TimeFmt::formatTime(static_cast<qint64>(secs));
}

QDate tsToDate(const Ts &ts) {
    bool   ok;
    double secs = ts.toDouble(&ok);
    if (!ok)
        return {};
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs)).date();
}

QString formatDateLabel(const Ts &ts) {
    const QDate date = tsToDate(ts);
    if (!date.isValid())
        return {};
    const QDate today = QDate::currentDate();
    if (date == today)
        return QCoreApplication::translate("MsgRender", "Today");
    if (date == today.addDays(-1))
        return QCoreApplication::translate("MsgRender", "Yesterday");
    return TimeFmt::formatDate(date);
}

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

// Converts TextWithEntities to Qt-flavoured HTML for QTextDocument.
QString toHtml(const TextWithEntities &twe, const Session *session) {
    if (twe.entities.empty()) {
        return twe.text.toHtmlEscaped().replace("\n", "<br>");
    }

    QString html;
    int     pos    = 0;
    auto    sorted = twe.entities;
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.offset < b.offset;
    });

    auto escapeAndBr = [](const QString &s) { return s.toHtmlEscaped().replace("\n", "<br>"); };

    for (const auto &e : sorted) {
        if (e.offset > pos)
            html += escapeAndBr(twe.text.mid(pos, e.offset - pos));
        auto rawInner = twe.text.mid(e.offset, e.length);
        auto inner    = rawInner.toHtmlEscaped();
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
        case EntityType::Pre:
            html += "<pre style='background:" + Th::qss(Th::c().message.codeBlockBg) +
                    ";padding:6px 10px;border-radius:4px;font-family:monospace;font-size:0.88em;"
                    "white-space:pre-wrap;margin:4px 0'>" +
                    inner + "</pre>";
            break;
        case EntityType::Blockquote:
            // Use a table so the gray left bar renders reliably in Qt's HTML subset.
            html += "<table cellspacing='0' cellpadding='0' style='border-spacing:0;margin:4px 0'>"
                    "<tr>"
                    "<td width='3' bgcolor='" +
                    Th::c().message.codeBlockBorder.name() +
                    "' style='padding:0;border-radius:2px'></td>"
                    "<td style='padding:2px 0 2px 10px;color:" +
                    Th::qss(Th::c().message.codeText) + "'>" + inner.replace("\n", "<br>") +
                    "</td></tr></table>";
            break;
        case EntityType::Link:
            html += "<a href='" + e.data.toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().text.link) + ";text-decoration:none'>" +
                    inner + "</a>";
            break;
        case EntityType::UserMention: {
            const QString label = session ? resolveMentionImpl(e.data, session) : rawInner;
            const bool    isMe  = session && UserId{e.data} == session->meUserId();
            // Anchor (not span) so the mention is hit-testable for the hover profile card.
            html += "<a href='" + (kUserAnchorPrefix + e.data).toHtmlEscaped() +
                    "' style='color:" + Th::qss(Th::c().message.mentionText) + ";background:" +
                    Th::qss(isMe ? Th::c().message.mentionSelfBg : Th::c().message.mentionBg) +
                    ";border-radius:3px;padding:0 2px;text-decoration:none'>" +
                    label.toHtmlEscaped() + "</a>";
            break;
        }
        case EntityType::ChannelMention:
            html += "<span style='color:" + Th::qss(Th::c().message.mentionText) +
                    ";background:" + Th::qss(Th::c().message.mentionBg) +
                    ";border-radius:3px;padding:0 2px'>" + inner + "</span>";
            break;
        case EntityType::HereCommand:
        case EntityType::ChannelCommand:
            html += "<span style='color:" + Th::qss(Th::c().message.mentionText) +
                    ";background:" + Th::qss(Th::c().message.mentionSelfBg) +
                    ";border-radius:3px;padding:0 2px'>" + inner + "</span>";
            break;
        case EntityType::Emoji: {
            const auto    er = resolveEmojiRich(e.data, session);
            const QString px = QString::number(inlineEmojiPx());
            if (!er.imageUrl.isEmpty()) {
                // Workspace custom emoji — the image resource is registered on the
                // QTextDocument by the caller (see ensureDocLayout).
                html += "<img src='" + er.imageUrl.toHtmlEscaped() + "' width='" + px +
                        "' height='" + px + "'>";
            } else {
                html += "<span style='font-family:" + emojiFontFamily() + ";font-size:" + px +
                        "px'>" + er.unicode.toHtmlEscaped() + "</span>";
            }
            break;
        }
        }
        pos = e.offset + e.length;
    }
    if (pos < twe.text.size())
        html += escapeAndBr(twe.text.mid(pos));
    return html;
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

// Build the full HTML for a message's main text doc (blocks preferred over text field).
QString buildMsgHtml(const Message &msg, const Session *session) {
    if (!msg.blocks.empty()) {
        QString html;
        for (const auto &blk : msg.blocks) {
            if (blk.typeStr == "divider") {
                html += "<hr style='border:0;border-top:1px solid " + Th::qss(Th::c().divider.def) +
                        ";margin:4px 0'>";
            } else if (blk.typeStr == "header") {
                html += "<p style='font-size:1.1em;font-weight:bold;margin:2px 0'>" +
                        toHtml(blk.text, session) + "</p>";
            } else if (blk.typeStr == "image") {
                // Painted separately; show alt text as italic placeholder
                if (!blk.altText.isEmpty())
                    html += "<p style='color:" + Th::qss(Th::c().text.tertiary) +
                            ";font-style:italic;margin:1px 0'>" + blk.altText.toHtmlEscaped() +
                            "</p>";
            } else if (!blk.text.text.isEmpty()) {
                html += wrapParagraph(toHtml(blk.text, session), "margin:2px 0");
            }
        }
        if (!html.isEmpty())
            return html;
    }
    return wrapParagraph(toHtml(msg.text, session), "margin:0");
}

// Attachment text HTML (used inside the colored bar area).
QString buildAttachHtml(const Attachment &att, const Session *session) {
    QString html;
    if (!att.pretext.isEmpty())
        html += "<p style='margin:0 0 2px'>" + att.pretext.toHtmlEscaped() + "</p>";
    if (!att.authorName.isEmpty())
        html += "<p style='margin:0;font-size:0.85em;color:" + Th::qss(Th::c().text.tertiary) +
                "'>" + att.authorName.toHtmlEscaped() + "</p>";
    if (!att.title.isEmpty()) {
        if (!att.titleLink.isEmpty())
            html += "<p style='margin:0;font-weight:bold'><a href='" +
                    att.titleLink.toHtmlEscaped() + "' style='color:" + Th::qss(Th::c().text.link) +
                    ";text-decoration:none'>" + att.title.toHtmlEscaped() + "</a></p>";
        else
            html += "<p style='margin:0;font-weight:bold'>" + att.title.toHtmlEscaped() + "</p>";
    }
    if (!att.text.text.isEmpty())
        html += wrapParagraph(toHtml(att.text, session), "margin:2px 0 0");

    // Key/value fields (classic bot format): bold title line, value below.
    for (const auto &f : att.fields) {
        if (!f.title.isEmpty())
            html +=
                "<p style='margin:2px 0 0;font-weight:bold'>" + f.title.toHtmlEscaped() + "</p>";
        if (!f.value.text.isEmpty())
            html += "<p style='margin:0'>" + toHtml(f.value, session) + "</p>";
    }

    // Render Block Kit blocks embedded in the attachment (modern bot format).
    if (html.isEmpty() && !att.blocks.empty()) {
        for (const auto &blk : att.blocks) {
            if (blk.typeStr == "divider") {
                html += "<hr style='border:0;border-top:1px solid " + Th::qss(Th::c().divider.def) +
                        ";margin:4px 0'>";
            } else if (blk.typeStr == "header") {
                html += "<p style='font-size:1.1em;font-weight:bold;margin:2px 0'>" +
                        toHtml(blk.text, session) + "</p>";
            } else if (blk.typeStr == "image") {
                if (!blk.altText.isEmpty())
                    html += "<p style='color:" + Th::qss(Th::c().text.tertiary) +
                            ";font-style:italic;margin:1px 0'>" + blk.altText.toHtmlEscaped() +
                            "</p>";
            } else if (!blk.text.text.isEmpty()) {
                html += wrapParagraph(toHtml(blk.text, session), "margin:2px 0");
            }
        }
    }

    // Last-resort fallback: parse as mrkdwn so any <url> links become clickable.
    if (html.isEmpty() && !att.fallback.isEmpty())
        html += "<p style='margin:2px 0 0'>" + toHtml(MrkdwnParser::parse(att.fallback), session) +
                "</p>";

    // Footer always renders last, after whichever content variant was chosen.
    if (!att.footer.isEmpty())
        html += "<p style='margin:4px 0 0;font-size:0.8em;color:" + Th::qss(Th::c().text.tertiary) +
                "'>" + att.footer.toHtmlEscaped() + "</p>";

    return html;
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

} // namespace MsgRender
