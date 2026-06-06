// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "message_render.h"
#include "session/session.h"

#include <QCoreApplication>
#include <QHash>
#include <QDateTime>

namespace MsgRender {

// Maps common Slack emoji short-names to their Unicode characters.
// Falls back to ":name:" for unknown names.
QString resolveEmoji(const QString &name) {
    static const QHash<QString,QString> kTable = {
        // ── Aliases & Slack-specific names ────────────────────────────
        {"+1","👍"},{"thumbsup","👍"},{"-1","👎"},{"thumbsdown","👎"},
        {"simple_smile","🙂"},{"slightly_smiling_face","🙂"},
        {"blush","😊"},{"hearts","❤️"},{"heavy_exclamation_mark","❗"},
        {"white_check_mark","✅"},{"raised_hands","🙌"},{"zany_face","🤪"},
        {"exploding_head","🤯"},{"face_palm","🤦"},{"shrug","🤷"},
        {"v","✌️"},{"speech_balloon","💬"},{"loudspeaker","📢"},
        {"no_bell","🔕"},{"mag","🔍"},{"iphone","📱"},
        {"chart_with_upwards_trend","📈"},{"chart_with_downwards_trend","📉"},
        {"dollar","💵"},{"rotating_light","🚨"},
        {"white_circle","⚪"},{"red_circle","🔴"},{"green_circle","🟢"},
        {"zzz","💤"},{"clock1","🕐"},{"hourglass","⌛"},{"moneybag","💰"},
        {"mailbox","📫"},{"unlock","🔓"},{"link","🔗"},
        // ── Faces & emotions ──────────────────────────────────────────
        {"smile","😊"},{"grin","😁"},{"laughing","😆"},
        {"joy","😂"},{"rofl","🤣"},{"sweat_smile","😅"},
        {"wink","😉"},{"heart_eyes","😍"},{"kissing_heart","😘"},
        {"stuck_out_tongue","😛"},{"thinking_face","🤔"},{"raised_eyebrow","🤨"},
        {"neutral_face","😐"},{"expressionless","😑"},{"zipper_mouth","🤐"},
        {"grimacing","😬"},{"sob","😭"},{"tired_face","😫"},
        {"sleepy","😪"},{"mask","😷"},{"sunglasses","😎"},
        {"nerd_face","🤓"},{"monocle_face","🧐"},{"confused","😕"},
        {"worried","😟"},{"angry","😠"},{"rage","😡"},
        {"skull","💀"},{"ghost","👻"},{"alien","👽"},
        {"poop","💩"},{"clown_face","🤡"},{"partying_face","🥳"},
        // ── Hands & people ────────────────────────────────────────────
        {"wave","👋"},{"raised_hand","✋"},{"ok_hand","👌"},
        {"thumbsup","👍"},{"thumbsdown","👎"},{"clap","👏"},
        {"pray","🙏"},{"point_right","👉"},{"point_left","👈"},
        {"point_up","☝️"},{"point_down","👇"},{"muscle","💪"},
        {"handshake","🤝"},{"writing_hand","✍️"},{"selfie","🤳"},
        // ── Hearts & symbols ──────────────────────────────────────────
        {"heart","❤️"},{"orange_heart","🧡"},{"yellow_heart","💛"},
        {"green_heart","💚"},{"blue_heart","💙"},{"purple_heart","💜"},
        {"broken_heart","💔"},{"sparkling_heart","💖"},{"two_hearts","💕"},
        {"100","💯"},{"tada","🎉"},{"fire","🔥"},
        {"star","⭐"},{"star2","🌟"},{"sparkles","✨"},
        {"zap","⚡"},{"boom","💥"},{"eyes","👀"},
        {"warning","⚠️"},
        // ── Animals ───────────────────────────────────────────────────
        {"dog","🐶"},{"cat","🐱"},{"mouse","🐭"},
        {"hamster","🐹"},{"rabbit","🐰"},{"fox_face","🦊"},
        {"bear","🐻"},{"panda_face","🐼"},{"koala","🐨"},
        {"tiger","🐯"},{"lion","🦁"},{"cow","🐮"},
        {"pig","🐷"},{"frog","🐸"},{"monkey_face","🐵"},
        {"chicken","🐔"},{"penguin","🐧"},{"bird","🐦"},
        {"hatching_chick","🐣"},{"eagle","🦅"},{"owl","🦉"},
        {"snake","🐍"},{"turtle","🐢"},{"bee","🐝"},{"butterfly","🦋"},
        // ── Food ──────────────────────────────────────────────────────
        {"apple","🍎"},{"watermelon","🍉"},{"grapes","🍇"},
        {"strawberry","🍓"},{"pizza","🍕"},{"hamburger","🍔"},
        {"hot_dog","🌭"},{"taco","🌮"},{"sushi","🍣"},
        {"ramen","🍜"},{"cake","🎂"},{"coffee","☕"},{"beer","🍺"},
        // ── Travel & places ───────────────────────────────────────────
        {"rocket","🚀"},{"airplane","✈️"},{"car","🚗"},
        {"bus","🚌"},{"train","🚂"},{"bicycle","🚲"},
        {"boat","⛵"},{"house","🏠"},
        {"earth_americas","🌎"},{"earth_africa","🌍"},{"earth_asia","🌏"},
        // ── Objects & misc ────────────────────────────────────────────
        {"computer","💻"},{"keyboard","⌨️"},
        {"email","📧"},{"memo","📝"},{"pencil","✏️"},{"paperclip","📎"},
        {"scissors","✂️"},{"lock","🔒"},{"key","🔑"},
        {"hammer","🔨"},{"wrench","🔧"},{"gear","⚙️"},
        {"bulb","💡"},{"books","📚"},
        {"trophy","🏆"},{"medal","🏅"},{"gift","🎁"},
        {"balloon","🎈"},{"musical_note","🎵"},
        {"headphones","🎧"},{"microphone","🎤"},{"camera","📷"},
        {"calendar","📅"},
        {"x","❌"},{"question","❓"},{"bell","🔔"},
    };
    const auto it = kTable.constFind(name);
    return it != kTable.constEnd() ? *it : (":" + name + ":");
}

QString formatTs(const Ts &ts) {
    bool ok = false;
    double secs = ts.toDouble(&ok);
    if (!ok) return ts;
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs))
               .toString("h:mm AP");
}

QDate tsToDate(const Ts &ts) {
    bool ok;
    double secs = ts.toDouble(&ok);
    if (!ok) return {};
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs)).date();
}

QString formatDateLabel(const Ts &ts) {
    const QDate date = tsToDate(ts);
    if (!date.isValid()) return {};
    const QDate today = QDate::currentDate();
    if (date == today) return QCoreApplication::translate("MsgRender", "Today");
    if (date == today.addDays(-1)) return QCoreApplication::translate("MsgRender", "Yesterday");
    if (date.year() == today.year()) return date.toString("MMMM d");
    return date.toString("MMMM d, yyyy");
}

// Resolve a UserMention entity's display name via entity.data (the user ID).
static QString resolveMentionImpl(const QString &userId, const Session *session) {
    if (!session) return "@" + userId;
    const auto *u = session->findUser(UserId{userId});
    return u ? ("@" + u->displayName) : ("@" + userId);
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
    int pos = 0;
    auto sorted = twe.entities;
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.offset < b.offset;
    });

    auto escapeAndBr = [](const QString &s) {
        return s.toHtmlEscaped().replace("\n", "<br>");
    };

    for (const auto &e : sorted) {
        if (e.offset > pos)
            html += escapeAndBr(twe.text.mid(pos, e.offset - pos));
        auto rawInner = twe.text.mid(e.offset, e.length);
        auto inner    = rawInner.toHtmlEscaped();
        switch (e.type) {
        case EntityType::Bold:
            html += "<b>" + inner + "</b>"; break;
        case EntityType::Italic:
            html += "<i>" + inner + "</i>"; break;
        case EntityType::Underline:
            html += "<u>" + inner + "</u>"; break;
        case EntityType::Strike:
            html += "<s>" + inner + "</s>"; break;
        case EntityType::Code:
            html += "<span style='background:#FDF0F0;color:#C0392B;font-family:monospace;"
                    "font-size:0.88em;padding:1px 3px;border-radius:3px'>"
                    + inner + "</span>"; break;
        case EntityType::Pre:
            html += "<pre style='background:#F4F4F4;padding:6px 10px;border-radius:4px;"
                    "font-family:monospace;font-size:0.88em;white-space:pre-wrap;margin:4px 0'>"
                    + inner + "</pre>"; break;
        case EntityType::Blockquote:
            // Use a table so the gray left bar renders reliably in Qt's HTML subset.
            html += "<table cellspacing='0' cellpadding='0' style='border-spacing:0;margin:0 0 8px 0'>"
                    "<tr>"
                    "<td width='3' bgcolor='#CCCCCC' style='padding:0;border-radius:2px'></td>"
                    "<td style='padding:2px 0 2px 10px;color:#555555'>"
                    + inner.replace("\n", "<br>") +
                    "</td></tr></table>"; break;
        case EntityType::Link:
            html += "<a href='" + e.data.toHtmlEscaped() + "' style='color:#1264A3'>"
                    + inner + "</a>"; break;
        case EntityType::UserMention: {
            const QString label = session
                ? resolveMentionImpl(e.data, session)
                : rawInner;
            html += "<span style='color:#1164A3;background:#E8F5FA;"
                    "border-radius:3px;padding:0 2px'>"
                    + label.toHtmlEscaped() + "</span>"; break;
        }
        case EntityType::ChannelMention:
            html += "<span style='color:#1164A3;background:#E8F5FA;"
                    "border-radius:3px;padding:0 2px'>" + inner + "</span>"; break;
        case EntityType::HereCommand:
        case EntityType::ChannelCommand:
            html += "<span style='color:#E01E5A;font-weight:bold'>" + inner + "</span>"; break;
        case EntityType::Emoji:
            html += resolveEmoji(e.data).toHtmlEscaped(); break;
        }
        pos = e.offset + e.length;
    }
    if (pos < twe.text.size())
        html += escapeAndBr(twe.text.mid(pos));
    return html;
}

// Build the full HTML for a message's main text doc (blocks preferred over text field).
QString buildMsgHtml(const Message &msg, const Session *session) {
    if (!msg.blocks.empty()) {
        QString html;
        for (const auto &blk : msg.blocks) {
            if (blk.typeStr == "divider") {
                html += "<hr style='border:0;border-top:1px solid #DDD;margin:4px 0'>";
            } else if (blk.typeStr == "header") {
                html += "<p style='font-size:1.1em;font-weight:bold;margin:2px 0'>"
                     + toHtml(blk.text, session) + "</p>";
            } else if (blk.typeStr == "image") {
                // Painted separately; show alt text as italic placeholder
                if (!blk.altText.isEmpty())
                    html += "<p style='color:#888;font-style:italic;margin:1px 0'>"
                         + blk.altText.toHtmlEscaped() + "</p>";
            } else if (!blk.text.text.isEmpty()) {
                html += "<p style='margin:2px 0'>"
                     + toHtml(blk.text, session) + "</p>";
            }
        }
        return html.isEmpty() ? toHtml(msg.text, session) : html;
    }
    return toHtml(msg.text, session);
}

// Attachment text HTML (used inside the colored bar area).
QString buildAttachHtml(const Attachment &att, const Session *session) {
    QString html;
    if (!att.pretext.isEmpty())
        html += "<p style='margin:0 0 2px'>" + att.pretext.toHtmlEscaped() + "</p>";
    if (!att.authorName.isEmpty())
        html += "<p style='margin:0;font-size:0.85em;color:#888'>"
             + att.authorName.toHtmlEscaped() + "</p>";
    if (!att.title.isEmpty()) {
        if (!att.titleLink.isEmpty())
            html += "<p style='margin:0;font-weight:bold'><a href='"
                 + att.titleLink.toHtmlEscaped() + "' style='color:#1264A3'>"
                 + att.title.toHtmlEscaped() + "</a></p>";
        else
            html += "<p style='margin:0;font-weight:bold'>"
                 + att.title.toHtmlEscaped() + "</p>";
    }
    if (!att.text.text.isEmpty())
        html += "<p style='margin:0'>" + toHtml(att.text, session) + "</p>";
    if (!att.footer.isEmpty())
        html += "<p style='margin:2px 0 0;font-size:0.8em;color:#888'>"
             + att.footer.toHtmlEscaped() + "</p>";
    return html;
}

QColor fileTypeColor(const File &f) {
    const QString mt = f.mimeType.toLower();
    if (mt.contains("pdf"))                                       return QColor("#E44D4D");
    if (mt.contains("word") || mt.contains("document"))           return QColor("#2B579A");
    if (mt.contains("excel") || mt.contains("spreadsheet"))       return QColor("#217346");
    if (mt.contains("powerpoint") || mt.contains("presentation")) return QColor("#D24726");
    if (mt.startsWith("video/"))                                  return QColor("#7B2D8B");
    if (mt.startsWith("audio/"))                                  return QColor("#1E7A6E");
    if (mt.contains("zip") || mt.contains("x-tar")
     || mt.contains("gzip") || mt.contains("x-7z")
     || mt.contains("x-rar"))                                     return QColor("#8B6914");
    if (mt.startsWith("text/") || mt.contains("json")
     || mt.contains("xml"))                                       return QColor("#555555");
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
    if (bytes <= 0)        return {};
    if (bytes < 1024)      return QString("%1 B").arg(bytes);
    if (bytes < 1024*1024) return QString("%1 KB").arg(bytes / 1024);
    const double mb = bytes / (1024.0 * 1024.0);
    return QString("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
}

} // namespace MsgRender
