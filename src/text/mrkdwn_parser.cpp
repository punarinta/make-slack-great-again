// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mrkdwn_parser.h"
#include "util/relative_time.h"
#include "util/time_format.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QRegularExpression>

namespace MrkdwnParser {

// Single-pass scanner. We build plain text while recording entity spans.
// Offsets in TextEntity refer to positions in the *output* plain text, not the mrkdwn.
// Paired marks (*_~`__) recurse into their content, so nested constructs
// (links inside bold, emoji inside quotes…) produce contained entity spans.

QString decodeEntities(QString s) {
    // Slack escapes exactly these three in every text field. &amp; last, so
    // "&amp;lt;" correctly decodes to the literal "&lt;".
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

struct Builder {
    QString                 text;
    std::vector<TextEntity> entities;

    void appendPlain(QChar c) { text.append(c); }
    void appendPlain(const QString &s) { text.append(s); }

    void addSpan(EntityType type, int start, const QString &data = {}) {
        entities.push_back(TextEntity{type, start, static_cast<int>(text.size()) - start, data});
    }

    // Append a recursively-parsed fragment, then wrap it in an outer span.
    // The outer entity is pushed BEFORE the shifted inner ones so that a
    // stable offset/length sort keeps the parent first for equal ranges.
    void appendNested(EntityType type, const TextWithEntities &sub, const QString &data = {}) {
        const int start = static_cast<int>(text.size());
        text += sub.text;
        entities.push_back(TextEntity{type, start, static_cast<int>(sub.text.size()), data});
        for (auto e : sub.entities) {
            e.offset += start;
            entities.push_back(e);
        }
    }
};

// Try to consume a paired delimiter (*,_,~,`) starting at pos.
// Returns the closing pos+1 if matched, -1 otherwise.
static int findClose(const QString &src, int start, QChar delim) {
    for (int i = start; i < src.size(); ++i) {
        if (src[i] == delim && (i == 0 || src[i - 1] != '\\'))
            return i + 1;
        // Don't cross newlines for inline marks
        if (src[i] == '\n')
            return -1;
    }
    return -1;
}

// Find position after a doubled closing delimiter (e.g. __ for underline).
static int findDoubleClose(const QString &src, int start, QChar delim) {
    for (int i = start; i + 1 < src.size(); ++i) {
        if (src[i] == delim && src[i + 1] == delim)
            return i + 2;
        if (src[i] == '\n')
            return -1;
    }
    return -1;
}

// Try to consume a fenced code block starting at pos (pos points just after the opening ```).
// Returns the closing position after the closing ```, or -1.
static int findCodeFenceClose(const QString &src, int pos) {
    while (pos < src.size() - 2) {
        if (src[pos] == '`' && src[pos + 1] == '`' && src[pos + 2] == '`')
            return pos + 3;
        ++pos;
    }
    return -1;
}

// "Today"/"Yesterday"/"Tomorrow" when date is adjacent to now, else absolute.
static QString prettyDay(const QDate &date, const QString &absolute) {
    const QDate today = QDate::currentDate();
    if (date == today)
        return QCoreApplication::translate("MrkdwnParser", "Today");
    if (date == today.addDays(-1))
        return QCoreApplication::translate("MrkdwnParser", "Yesterday");
    if (date == today.addDays(1))
        return QCoreApplication::translate("MrkdwnParser", "Tomorrow");
    return absolute;
}

// Render a <!date^ts^format…> token's format string ("{date_short} at {time}").
// Returns an empty string when the format contains a token we don't know —
// the caller then falls back to the fallback text, as the Slack docs mandate.
static QString formatDateToken(qint64 secs, const QString &fmt) {
    const QDateTime dt   = QDateTime::fromSecsSinceEpoch(secs);
    const QDate     date = dt.date();

    QString                         out;
    int                             pos = 0;
    static const QRegularExpression tokenRe(QStringLiteral("\\{([a-z_]+)\\}"));
    QRegularExpressionMatchIterator it = tokenRe.globalMatch(fmt);
    while (it.hasNext()) {
        const auto m = it.next();
        out += fmt.mid(pos, m.capturedStart() - pos);
        pos = m.capturedEnd();

        const QString name = m.captured(1);
        QString       val;
        if (name == QLatin1String("date_num")) {
            val = date.toString(Qt::ISODate);
        } else if (name == QLatin1String("date") || name == QLatin1String("date_short")) {
            val = TimeFmt::formatDate(date);
        } else if (name == QLatin1String("date_pretty") ||
                   name == QLatin1String("date_short_pretty")) {
            val = prettyDay(date, TimeFmt::formatDate(date));
        } else if (name == QLatin1String("date_long")) {
            val = TimeFmt::locale().dayName(date.dayOfWeek()) + ", " + TimeFmt::formatDate(date);
        } else if (name == QLatin1String("date_long_pretty")) {
            val = prettyDay(
                date, TimeFmt::locale().dayName(date.dayOfWeek()) + ", " + TimeFmt::formatDate(date)
            );
        } else if (name == QLatin1String("time")) {
            val = TimeFmt::formatTime(dt);
        } else if (name == QLatin1String("time_secs")) {
            val = TimeFmt::locale().toString(
                dt.time(),
                TimeFmt::use24h() ? QStringLiteral("HH:mm:ss") : QStringLiteral("h:mm:ss AP")
            );
        } else if (name == QLatin1String("ago")) {
            val = relativeTime(secs);
        } else {
            return {}; // unknown token → use the sender's fallback text
        }
        out += val;
    }
    out += fmt.mid(pos);
    return out;
}

// A URL carries a scheme we recognise as linkable. Used to tell a real
// <url|label> token from a literal "<word>" that happens to sit in a
// rich_text run (see resolveTokens), where '<' is not escaped.
static bool looksLikeUrl(const QString &s) {
    return s.contains(QLatin1String("://")) || s.startsWith(QLatin1String("mailto:")) ||
           s.startsWith(QLatin1String("tel:"));
}

// Append `s`, adding an Emoji span per :name: shortcode. Link labels carry no
// mrkdwn marks, but Slack's clients do render emoji in them (CI bots title
// their notifications ":white_check_mark: …" inside a <url|label> token).
static void appendPlainWithEmoji(Builder &b, const QString &s) {
    static const QRegularExpression shortcode(QStringLiteral(":([a-zA-Z0-9_+\\-]+):"));
    int  pos = 0;
    auto it  = shortcode.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        b.appendPlain(s.mid(pos, m.capturedStart() - pos));
        const int start = b.text.size();
        b.appendPlain(m.captured(0));
        b.addSpan(EntityType::Emoji, start, m.captured(1));
        pos = m.capturedEnd();
    }
    b.appendPlain(s.mid(pos));
}

// Append the resolved content of a single <…> construct: a user/channel
// mention, an <!command> (incl. <!date^…>), or a <url|label> link. Mutates b.
// When requireScheme is true a plain link is only linkified if its URL has a
// scheme — so a bare "<word>" stays literal; parse() passes false because Slack
// escapes real '<' to &lt; in the mrkdwn fields it handles.
static void appendAngleConstruct(Builder &b, const QString &inner, bool requireScheme) {
    // User mention: <@UXXXXX> or <@UXXXXX|name>
    if (inner.startsWith('@')) {
        auto parts       = inner.mid(1).split('|');
        auto uid         = parts[0];
        auto label       = parts.size() > 1 ? decodeEntities(parts[1]) : ("@" + uid);
        int  entityStart = b.text.size();
        b.appendPlain(label);
        b.addSpan(EntityType::UserMention, entityStart, uid);
        return;
    }

    // Channel mention: <#CXXXXX|name>
    if (inner.startsWith('#')) {
        auto parts       = inner.mid(1).split('|');
        auto cid         = parts[0];
        auto name        = parts.size() > 1 ? decodeEntities(parts[1]) : cid;
        int  entityStart = b.text.size();
        b.appendPlain("#" + name);
        b.addSpan(EntityType::ChannelMention, entityStart, cid);
        return;
    }

    // Special commands: <!here>, <!channel>, <!date^…>, <!subteam^S|name>
    if (inner.startsWith('!')) {
        auto cmd = inner.mid(1);
        if (cmd == "here") {
            int s = b.text.size();
            b.appendPlain("@here");
            b.addSpan(EntityType::HereCommand, s);
        } else if (cmd == "channel") {
            int s = b.text.size();
            b.appendPlain("@channel");
            b.addSpan(EntityType::ChannelCommand, s);
        } else if (cmd.startsWith(QLatin1String("date^"))) {
            // <!date^unix-ts^format-string[^link]|fallback>
            const int     pipe     = cmd.indexOf('|');
            const QString fallback = pipe >= 0 ? decodeEntities(cmd.mid(pipe + 1)) : QString();
            const auto    parts    = (pipe >= 0 ? cmd.left(pipe) : cmd).split('^');
            bool          tsOk     = false;
            const qint64  secs     = parts.value(1).toLongLong(&tsOk);
            QString       rendered =
                tsOk ? formatDateToken(secs, decodeEntities(parts.value(2))) : QString();
            if (rendered.isEmpty())
                rendered = fallback;
            const QString link = parts.size() > 3 ? decodeEntities(parts[3]) : QString();
            const int     s    = b.text.size();
            b.appendPlain(rendered);
            if (!link.isEmpty())
                b.addSpan(EntityType::Link, s, link);
        } else {
            // subteam or unknown: show as @name
            auto parts = cmd.split('|');
            auto label = parts.size() > 1 ? decodeEntities(parts.last()) : cmd;
            int  s     = b.text.size();
            b.appendPlain("@" + label);
            b.addSpan(EntityType::HereCommand, s, cmd);
        }
        return;
    }

    // Link: <url|label> or <url>
    auto parts = inner.split('|');
    if (requireScheme && !looksLikeUrl(parts[0])) {
        // Not a Slack token — emit the brackets and content verbatim.
        b.appendPlain('<' + inner + '>');
        return;
    }
    auto url   = decodeEntities(parts[0]);
    auto label = parts.size() > 1 ? decodeEntities(parts[1]) : url;
    // Emoji entities nest inside the Link span (the Link is pushed first so
    // the parent-before-child order holds even when the label is one emoji).
    // Bare <url> displays the URL itself — never emoji-scan that ("/a:b:c"
    // path segments would false-match).
    Builder sub;
    if (parts.size() > 1)
        appendPlainWithEmoji(sub, label);
    else
        sub.appendPlain(label);
    b.appendNested(EntityType::Link, TextWithEntities{sub.text, sub.entities}, url);
}

// Recursion is bounded two ways. kMaxParseDepth is a hard backstop so crafted,
// deeply-nested inline marks can neither blow the stack nor the layout. inQuote
// caps blockquotes at a single level: once inside a quote, a further leading '>'
// is literal text, not another nested quote. This matches Slack (which renders
// only one quote level) and, crucially, avoids the layout pathology — every
// blockquote becomes a nested <table> in the rendered HTML (message_render.cpp),
// and QTextDocumentLayout's recursive frame layout gets catastrophically slow on
// deeply nested tables. A message of many stacked '>' marks once froze the whole
// UI here, deep inside QTextDocument::size() (caught by the hang watchdog).
static constexpr int kMaxParseDepth = 32;

static TextWithEntities parseImpl(const QString &mrkdwn, int depth, bool inQuote) {
    Builder   b;
    int       i = 0;
    const int n = mrkdwn.size();

    if (depth > kMaxParseDepth) {
        b.appendPlain(decodeEntities(mrkdwn));
        return TextWithEntities{b.text, b.entities};
    }

    while (i < n) {
        QChar c = mrkdwn[i];

        // ── Code fence ``` ──
        if (c == '`' && i + 2 < n && mrkdwn[i + 1] == '`' && mrkdwn[i + 2] == '`') {
            int contentStart = i + 3;
            // Skip optional language hint on same line
            int lineEnd      = mrkdwn.indexOf('\n', contentStart);
            if (lineEnd != -1 && lineEnd < n)
                contentStart = lineEnd + 1;
            int closePos = findCodeFenceClose(mrkdwn, contentStart);
            if (closePos != -1) {
                int entityStart = b.text.size();
                b.appendPlain(
                    decodeEntities(mrkdwn.mid(contentStart, closePos - 3 - contentStart))
                );
                b.addSpan(EntityType::Pre, entityStart);
                i = closePos;
                continue;
            }
        }

        // ── Inline code ` ──
        if (c == '`') {
            int close = findClose(mrkdwn, i + 1, '`');
            if (close != -1) {
                int entityStart = b.text.size();
                b.appendPlain(decodeEntities(mrkdwn.mid(i + 1, close - 1 - (i + 1))));
                b.addSpan(EntityType::Code, entityStart);
                i = close;
                continue;
            }
        }

        // ── Bold *text* ──
        if (c == '*') {
            int close = findClose(mrkdwn, i + 1, '*');
            if (close != -1) {
                b.appendNested(
                    EntityType::Bold,
                    parseImpl(mrkdwn.mid(i + 1, close - 1 - (i + 1)), depth + 1, inQuote)
                );
                i = close;
                continue;
            }
        }

        // ── Underline __text__ (must be checked before single _ italic) ──
        if (c == '_' && i + 1 < n && mrkdwn[i + 1] == '_') {
            int close = findDoubleClose(mrkdwn, i + 2, '_');
            if (close != -1) {
                b.appendNested(
                    EntityType::Underline,
                    parseImpl(mrkdwn.mid(i + 2, close - 2 - (i + 2)), depth + 1, inQuote)
                );
                i = close;
                continue;
            }
        }

        // ── Italic _text_ ──
        if (c == '_') {
            int close = findClose(mrkdwn, i + 1, '_');
            if (close != -1) {
                b.appendNested(
                    EntityType::Italic,
                    parseImpl(mrkdwn.mid(i + 1, close - 1 - (i + 1)), depth + 1, inQuote)
                );
                i = close;
                continue;
            }
        }

        // ── Strikethrough ~text~ ──
        if (c == '~') {
            int close = findClose(mrkdwn, i + 1, '~');
            if (close != -1) {
                b.appendNested(
                    EntityType::Strike,
                    parseImpl(mrkdwn.mid(i + 1, close - 1 - (i + 1)), depth + 1, inQuote)
                );
                i = close;
                continue;
            }
        }

        // ── Angular bracket constructs <…> ──
        if (c == '<') {
            int close = mrkdwn.indexOf('>', i + 1);
            if (close != -1) {
                auto inner = mrkdwn.mid(i + 1, close - i - 1);
                i          = close + 1;
                appendAngleConstruct(b, inner, /*requireScheme=*/false);
                continue;
            }
        }

        // ── Emoji :name: ──
        if (c == ':') {
            int close = mrkdwn.indexOf(':', i + 1);
            if (close != -1 && close > i + 1) {
                auto                      name = mrkdwn.mid(i + 1, close - i - 1);
                // Validate: emoji names are [a-z0-9_+-]+
                static QRegularExpression validEmoji("^[a-zA-Z0-9_+\\-]+$");
                if (validEmoji.match(name).hasMatch()) {
                    int entityStart = b.text.size();
                    b.appendPlain(":" + name + ":");
                    b.addSpan(EntityType::Emoji, entityStart, name);
                    i = close + 1;
                    continue;
                }
            }
        }

        // ── Blockquote (> at line start; the API escapes it to &gt;) ──
        // Consume all consecutive >-prefixed lines as a single Blockquote entity,
        // then recursively parse the collected content for inline constructs.
        auto quoteMarkLen = [&](int pos) -> int {
            if (pos >= n)
                return 0;
            if (mrkdwn[pos] == '>')
                return 1;
            return QStringView{mrkdwn}.mid(pos, 4) == u"&gt;" ? 4 : 0;
        };
        if (!inQuote && (i == 0 || mrkdwn[i - 1] == '\n') && quoteMarkLen(i) > 0) {
            // Drop all preceding \n: the block-level table element provides its own line break.
            while (!b.text.isEmpty() && b.text.back() == '\n')
                b.text.chop(1);
            QString quoted;
            bool    first = true;
            while (int markLen = quoteMarkLen(i)) {
                if (!first)
                    quoted += '\n';
                first = false;
                i += markLen; // skip '>' / '&gt;'
                if (i < n && mrkdwn[i] == ' ')
                    ++i; // skip optional space
                // Collect content until end of line
                while (i < n && mrkdwn[i] != '\n')
                    quoted += mrkdwn[i++];
                if (i < n)
                    ++i; // skip '\n'
                // Check if next non-empty line continues the quote
                if (quoteMarkLen(i) == 0)
                    break;
            }
            b.appendNested(EntityType::Blockquote, parseImpl(quoted, depth + 1, true));
            b.appendPlain('\n'); // ensure visual line break after blockquote
            continue;
        }

        // ── HTML entities (&lt; &gt; &amp;) — Slack escapes these in all text ──
        if (c == '&') {
            if (QStringView{mrkdwn}.mid(i, 4) == u"&lt;") {
                b.appendPlain('<');
                i += 4;
                continue;
            }
            if (QStringView{mrkdwn}.mid(i, 4) == u"&gt;") {
                b.appendPlain('>');
                i += 4;
                continue;
            }
            if (QStringView{mrkdwn}.mid(i, 5) == u"&amp;") {
                b.appendPlain('&');
                i += 5;
                continue;
            }
        }

        b.appendPlain(c);
        ++i;
    }

    return TextWithEntities{b.text, b.entities};
}

TextWithEntities parse(const QString &mrkdwn) {
    return parseImpl(mrkdwn, /*depth=*/0, /*inQuote=*/false);
}

TextWithEntities resolveTokens(const QString &src) {
    // Resolve ONLY Slack's angle-bracket tokens (<@U>, <#C>, <!cmd>, <url|label>)
    // and :emoji: shortcodes. Unlike parse(), *, _, ~, ` are left literal: this
    // runs on already-structured runs (a rich_text "text" element) whose emphasis
    // comes from a style object, not mrkdwn marks. Slack's own text→rich_text
    // conversion (and some bots) leave these tokens unexpanded inside a plain text
    // element — which is why an Outlook Calendar reminder shows a raw
    // "<!date^…|2:00 PM>" — and Slack's clients resolve them everywhere.
    Builder   b;
    const int n = src.size();
    int       i = 0;
    while (i < n) {
        const QChar c = src[i];

        // ── Angular-bracket token <…> ──
        if (c == '<') {
            const int close = src.indexOf('>', i + 1);
            if (close != -1) {
                // requireScheme: '<' is not escaped in rich_text, so only linkify
                // a real URL — leave a literal "<word>" as typed.
                appendAngleConstruct(b, src.mid(i + 1, close - i - 1), /*requireScheme=*/true);
                i = close + 1;
                continue;
            }
        }

        // ── Emoji :name: ──
        if (c == ':') {
            const int close = src.indexOf(':', i + 1);
            if (close != -1 && close > i + 1) {
                const auto                name = src.mid(i + 1, close - i - 1);
                static QRegularExpression validEmoji("^[a-zA-Z0-9_+\\-]+$");
                if (validEmoji.match(name).hasMatch()) {
                    const int entityStart = b.text.size();
                    b.appendPlain(":" + name + ":");
                    b.addSpan(EntityType::Emoji, entityStart, name);
                    i = close + 1;
                    continue;
                }
            }
        }

        b.appendPlain(c);
        ++i;
    }
    return TextWithEntities{b.text, b.entities};
}

} // namespace MrkdwnParser
