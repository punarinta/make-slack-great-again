// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "markdown_compose.h"
#include "text/link_labels.h"
#include "text/mrkdwn_parser.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

namespace MarkdownCompose {
namespace {

// ── Inline ────────────────────────────────────────────────────────────────────

// Same notion as the parser's: something we would linkify.
bool looksLikeUrl(QStringView s) {
    return s.contains(QLatin1String("://")) || s.startsWith(QLatin1String("mailto:")) ||
           s.startsWith(QLatin1String("tel:"));
}

// <…> the way Slack reads it: a mention, a command or a link. Anything else in
// angle brackets is just text and stays open to the rewrites.
bool isSlackToken(QStringView inner) {
    if (inner.isEmpty())
        return false;
    const QChar c = inner[0];
    return c == '@' || c == '#' || c == '!' || looksLikeUrl(inner);
}

int backtickRun(const QString &s, int i) {
    int n = 0;
    while (i + n < s.size() && s[i + n] == '`')
        ++n;
    return n;
}

// Position just past a code span opened by `run` backticks at `open`, or -1.
// CommonMark: the closer is a run of exactly the same length.
int codeSpanEnd(const QString &s, int open, int run) {
    int j = open + run;
    while (j < s.size()) {
        if (s[j] != '`') {
            ++j;
            continue;
        }
        const int r = backtickRun(s, j);
        if (r == run)
            return j + r;
        j += r;
    }
    return -1;
}

// The closing "dd" (d repeated `width` times) for a double/triple delimiter
// opened at `open`: the first one that follows non-space text. Returns its
// index or -1.
int delimiterClose(const QString &s, int open, QChar d, int width) {
    for (int j = open + width; j + width <= s.size(); ++j) {
        if (s[j - 1].isSpace() || s[j - 1] == d)
            continue;
        bool all = true;
        for (int k = 0; k < width && all; ++k)
            all = s[j + k] == d;
        if (all)
            return j;
    }
    return -1;
}

// [label](url), ![alt](url), with an optional "title"; URLs may hold one
// level of parentheses (Wikipedia). Anchored at the match offset by the caller.
const QRegularExpression &linkRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(!?\[([^\[\]]+)\]\(((?:[^\s()<>]|\([^\s()<>]*\))+)(?:\s+"[^"]*")?\))")
    );
    return re;
}

QString slackLink(const QString &url, QString label) {
    // Slack's token has no escape for '|' in the URL — the label would be
    // split off at the wrong place, so a pipe-bearing URL goes bare.
    if (url.contains('|') || label.trimmed().isEmpty() || label == url)
        return '<' + url + '>';
    label.replace('|', '/');
    label.replace('>', QLatin1String("&gt;"));
    return '<' + url + '|' + label + '>';
}

} // namespace

QString convertInline(const QString &s) {
    QString   out;
    const int n = s.size();
    int       i = 0;
    out.reserve(n);
    while (i < n) {
        const QChar c = s[i];

        // Code span: verbatim, delimiters included.
        if (c == '`') {
            const int run = backtickRun(s, i);
            const int end = codeSpanEnd(s, i, run);
            if (end > 0) {
                out += s.mid(i, end - i);
                i = end;
            } else {
                out += s.mid(i, run);
                i += run;
            }
            continue;
        }

        // Slack token: opaque.
        if (c == '<') {
            const int close = s.indexOf('>', i + 1);
            if (close > 0 && isSlackToken(QStringView{s}.mid(i + 1, close - i - 1))) {
                out += s.mid(i, close + 1 - i);
                i = close + 1;
                continue;
            }
        }

        // ***x*** → *_x_*, **x** → *x*, ~~x~~ → ~x~. The opener must touch text
        // (not a space, not another delimiter) and the closer must follow text,
        // the same flanking rule that keeps "5 ** 2" and "a ** b ** c" literal.
        if ((c == '*' || c == '~') && i + 1 < n && s[i + 1] == c) {
            const int width = (c == '*' && i + 2 < n && s[i + 2] == '*') ? 3 : 2;
            const int first = i + width;
            if (first < n && !s[first].isSpace() && s[first] != c) {
                const int close = delimiterClose(s, i, c, width);
                if (close > 0) {
                    const QString inner = convertInline(s.mid(first, close - first));
                    if (width == 3)
                        out += "*_" + inner + "_*";
                    else
                        out += c + inner + c;
                    i = close + width;
                    continue;
                }
            }
        }

        // [label](url) → <url|label>.
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            const auto m = linkRe().match(
                s, i, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption
            );
            if (m.hasMatch() && looksLikeUrl(m.capturedView(2))) {
                out += slackLink(m.captured(2), m.captured(1));
                i = m.capturedEnd();
                continue;
            }
        }

        out += c;
        ++i;
    }
    return out;
}

// ── rich_text elements ────────────────────────────────────────────────────────

namespace {

struct Style {
    bool bold = false, italic = false, strike = false, code = false;

    bool        any() const { return bold || italic || strike || code; }
    QJsonObject json(bool withCode = true) const {
        QJsonObject o;
        if (bold)
            o["bold"] = true;
        if (italic)
            o["italic"] = true;
        if (strike)
            o["strike"] = true;
        if (code && withCode)
            o["code"] = true;
        return o;
    }
};

void emitText(QJsonArray &out, const QString &t, const Style &st) {
    if (t.isEmpty())
        return;
    const QJsonObject style = st.json();
    if (!out.isEmpty()) {
        QJsonObject last = out.last().toObject();
        if (last.value("type").toString() == QLatin1String("text") &&
            last.value("style").toObject() == style) {
            last["text"]        = last.value("text").toString() + t;
            out[out.size() - 1] = last;
            return;
        }
    }
    QJsonObject o{{"type", "text"}, {"text", t}};
    if (!style.isEmpty())
        o["style"] = style;
    out.append(o);
}

// Attach a style object when the element type takes one. Mentions accept
// bold/italic/strike only; a code flag there is invalid_blocks.
void withStyle(QJsonObject &o, const Style &st, bool allowCode) {
    const QJsonObject style = st.json(allowCode);
    if (!style.isEmpty())
        o["style"] = style;
}

void emitEmoji(QJsonArray &out, const QString &name) {
    // ":+1::skin-tone-3:" arrives as two Emoji spans; the official client sends
    // one element with skin_tone, and a lone skin-tone element shows a swatch.
    static const QRegularExpression toneRe(QStringLiteral("^skin-tone-([2-6])$"));
    if (const auto m = toneRe.match(name); m.hasMatch() && !out.isEmpty()) {
        QJsonObject last = out.last().toObject();
        if (last.value("type").toString() == QLatin1String("emoji") &&
            !last.contains("skin_tone")) {
            last["skin_tone"]   = m.captured(1).toInt();
            out[out.size() - 1] = last;
            return;
        }
    }
    out.append(QJsonObject{{"type", "emoji"}, {"name", name}});
}

void emitLink(QJsonArray &out, const QString &url, const QString &label, const Style &st) {
    if (!looksLikeUrl(url)) {
        // The parser linkifies any <word>; Slack would refuse a link element
        // without a scheme, so give the text back as typed.
        emitText(out, '<' + (label == url ? url : url + '|' + label) + '>', st);
        return;
    }
    QJsonObject o{{"type", "link"}, {"url", url}};
    if (label != url)
        o["text"] = label;
    withStyle(o, st, /*allowCode=*/true);
    out.append(o);
}

void emitCommand(QJsonArray &out, const QString &data, const QString &label, const Style &st) {
    // <!here> and <!channel> come with empty data; <!everyone> arrives as an
    // unknown command with the raw command as data.
    QString range;
    if (data.isEmpty() || data == QLatin1String("here"))
        range = QStringLiteral("here");
    else if (data == QLatin1String("channel") || data == QLatin1String("everyone"))
        range = data;
    if (!range.isEmpty()) {
        out.append(QJsonObject{{"type", "broadcast"}, {"range", range}});
        return;
    }
    emitText(out, label, st); // an unknown command shows as its label
}

// Walk [from, to) of `text`, consuming entities from `k` in (offset asc,
// length desc) order — children follow their parent, so a parent's walk owns
// everything up to its end.
void walk(
    const QString                 &text,
    const std::vector<TextEntity> &ents,
    size_t                        &k,
    int                            from,
    int                            to,
    Style                          st,
    QJsonArray                    &out
) {
    int pos = from;
    while (k < ents.size() && ents[k].offset < to) {
        const TextEntity e = ents[k];
        ++k;
        if (e.offset < pos || e.length <= 0)
            continue; // overlapping or empty: nothing to wrap
        const int end = std::min(e.offset + e.length, to);
        emitText(out, text.mid(pos, e.offset - pos), st);
        pos = end;

        auto skipChildren = [&] {
            while (k < ents.size() && ents[k].offset < end)
                ++k;
        };
        const QString span = text.mid(e.offset, end - e.offset);
        switch (e.type) {
        case EntityType::Bold:
        case EntityType::Italic:
        case EntityType::Strike:
        case EntityType::Code:
        case EntityType::Pre: {
            Style s2 = st;
            if (e.type == EntityType::Bold)
                s2.bold = true;
            else if (e.type == EntityType::Italic)
                s2.italic = true;
            else if (e.type == EntityType::Strike)
                s2.strike = true;
            else
                s2.code = true; // a mid-line ``` run can only be inline code here
            walk(text, ents, k, e.offset, end, s2, out);
            break;
        }
        case EntityType::Underline: // Slack has no underline style
        case EntityType::Blockquote:
            walk(text, ents, k, e.offset, end, st, out);
            break;
        case EntityType::Link:
            emitLink(out, e.data, span, st);
            skipChildren();
            break;
        case EntityType::MessageLink:
            emitLink(out, span, span, st); // the span IS the permalink
            skipChildren();
            break;
        case EntityType::UserMention: {
            QJsonObject o{{"type", "user"}, {"user_id", e.data}};
            withStyle(o, st, /*allowCode=*/false);
            out.append(o);
            skipChildren();
            break;
        }
        case EntityType::ChannelMention: {
            QJsonObject o{{"type", "channel"}, {"channel_id", e.data}};
            withStyle(o, st, /*allowCode=*/false);
            out.append(o);
            skipChildren();
            break;
        }
        case EntityType::Emoji:
            emitEmoji(out, e.data);
            skipChildren();
            break;
        case EntityType::HereCommand:
            emitCommand(out, e.data, span, st);
            skipChildren();
            break;
        case EntityType::UsergroupMention: {
            QJsonObject o{{"type", "usergroup"}, {"usergroup_id", e.data}};
            withStyle(o, st, /*allowCode=*/false);
            out.append(o);
            skipChildren();
            break;
        }
        case EntityType::ChannelCommand:
            out.append(QJsonObject{{"type", "broadcast"}, {"range", "channel"}});
            skipChildren();
            break;
        }
    }
    emitText(out, text.mid(pos, to - pos), st);
}

} // namespace

QJsonArray richTextElements(const TextWithEntities &twe) {
    std::vector<TextEntity> ents = twe.entities;
    std::stable_sort(ents.begin(), ents.end(), TextEntityNestingOrder{});
    QJsonArray out;
    size_t     k = 0;
    walk(twe.text, ents, k, 0, twe.text.size(), Style{}, out);
    return out;
}

// ── Blocks ────────────────────────────────────────────────────────────────────

namespace {

enum class Kind { Section, Quote, Pre, List };

struct Item {
    int     indent  = 0; // columns of leading whitespace, as typed
    int     level   = 0; // rank of `indent` among the list's indents
    bool    ordered = false;
    int     number  = 1;
    QString text; // inline-converted; continuation lines joined with '\n'
};

struct Chunk {
    Kind              kind;
    QString           text; // Section/Quote: converted mrkdwn; Pre: raw code
    std::vector<Item> items;
};

bool isBlank(const QString &line) {
    return line.trimmed().isEmpty();
}

int indentCols(const QString &line) {
    int cols = 0;
    for (const QChar c : line) {
        if (c == ' ')
            ++cols;
        else if (c == '\t')
            cols += 4;
        else
            break;
    }
    return cols;
}

// "```" opening a fence, whatever follows it on the line goes to `rest`.
bool fenceOpen(const QString &line, QString *rest) {
    const QString t = line.trimmed();
    if (!t.startsWith(QLatin1String("```")))
        return false;
    *rest = t.mid(3);
    return true;
}

// "```js": a language hint Slack would print as the first code line. One word
// of identifier characters — "```ls -la" is mrkdwn habit and stays code.
bool isLanguageHint(const QString &info) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9_+#.-]{0,29}$"));
    return re.match(info).hasMatch();
}

// "- a", "* a", "+ a", "1. a", "1) a" — a marker, a space, then something.
bool listItem(const QString &line, Item *item, QString *content) {
    static const QRegularExpression re(
        QStringLiteral(R"(^([ \t]*)(?:([-*+])|(\d{1,9})[.)])[ \t]+(\S.*)$)")
    );
    const auto m = re.match(line);
    if (!m.hasMatch())
        return false;
    item->indent  = indentCols(line);
    item->ordered = !m.capturedView(3).isEmpty();
    item->number  = item->ordered ? m.captured(3).toInt() : 1;
    *content      = m.captured(4);
    return true;
}

bool quoteLine(const QString &line, QString *prefix, QString *content) {
    static const QRegularExpression re(QStringLiteral("^( {0,3}> ?)(.*)$"));
    const auto                      m = re.match(line);
    if (!m.hasMatch())
        return false;
    *prefix  = m.captured(1);
    *content = m.captured(2);
    return true;
}

QJsonObject section(const char *type, const QString &mrkdwn) {
    QJsonArray els = richTextElements(MrkdwnParser::parse(mrkdwn));
    if (els.isEmpty()) // never an empty section — Slack refuses it
        els.append(QJsonObject{{"type", "text"}, {"text", mrkdwn.isEmpty() ? " " : mrkdwn}});
    return QJsonObject{{"type", type}, {"elements", els}};
}

QJsonArray listElements(const std::vector<Item> &items) {
    QJsonArray out;
    size_t     i = 0;
    while (i < items.size()) {
        // One rich_text_list per run of items sharing level and style; a nested
        // list is a sibling element with a deeper indent, the way Slack shapes it.
        const Item &head = items[i];
        QJsonArray  els;
        size_t      j = i;
        for (; j < items.size() && items[j].level == head.level && items[j].ordered == head.ordered;
             ++j)
            els.append(section("rich_text_section", items[j].text));
        QJsonObject list{
            {"type", "rich_text_list"},
            {"style", head.ordered ? "ordered" : "bullet"},
            {"elements", els},
        };
        if (head.level > 0)
            list["indent"] = head.level;
        if (head.ordered && head.number > 1)
            list["offset"] = head.number - 1;
        out.append(list);
        i = j;
    }
    return out;
}

} // namespace

Composed convert(const QString &composerText) {
    QString text = composerText;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace('\r', '\n');

    QStringList        lines = text.split('\n');
    QStringList        out;
    std::vector<Chunk> chunks;
    bool               sectionOpen   = false; // the last chunk is a Section still taking lines
    int                pendingBlanks = 0;     // blank lines since that Section's last line
    bool               anyList       = false;

    auto closeSection = [&] {
        sectionOpen   = false;
        pendingBlanks = 0;
    };
    auto paragraphLine = [&](const QString &line) {
        const QString conv = convertInline(line);
        out << conv;
        if (sectionOpen) {
            chunks.back().text += QString(pendingBlanks + 1, '\n') + conv;
        } else {
            chunks.push_back({Kind::Section, conv, {}});
            sectionOpen = true;
        }
        pendingBlanks = 0;
    };

    int i = 0;
    while (i < lines.size()) {
        const QString line = lines[i];

        // ── Fenced code ──
        if (QString rest; fenceOpen(line, &rest)) {
            const int sameLine = rest.indexOf(QLatin1String("```"));
            if (sameLine > 0 && sameLine == rest.size() - 3) {
                // mrkdwn one-liner "```code```": already on the wire form.
                closeSection();
                out << line;
                chunks.push_back({Kind::Pre, rest.left(sameLine), {}});
                ++i;
                continue;
            }
            if (sameLine < 0) {
                int j = i + 1;
                while (j < lines.size() && !lines[j].contains(QLatin1String("```")))
                    ++j;
                if (j < lines.size()) {
                    QStringList   code;
                    const QString info = rest.trimmed();
                    if (!info.isEmpty() && !isLanguageHint(info))
                        code << rest;
                    for (int k = i + 1; k < j; ++k)
                        code << lines[k];
                    const int     at     = lines[j].indexOf(QLatin1String("```"));
                    const QString before = lines[j].left(at);
                    const QString after  = lines[j].mid(at + 3);
                    if (!isBlank(before))
                        code << before; // "last line```" — mrkdwn habit
                    closeSection();
                    out << QStringLiteral("```") << code << QStringLiteral("```");
                    chunks.push_back({Kind::Pre, code.join('\n'), {}});
                    i = j;
                    if (isBlank(after)) {
                        ++i;
                    } else {
                        lines[j] = after.trimmed(); // whatever trailed the fence: its own line
                    }
                    continue;
                }
            }
            // Unterminated, or "```a``` more": plain text, the parser copes.
        }

        // ── List ──
        Item    item;
        QString content;
        if (listItem(line, &item, &content)) {
            std::vector<Item> items;
            for (;;) {
                item.text = convertInline(content);
                items.push_back(item);
                ++i;
                // Continuation lines: indented, not a marker.
                while (i < lines.size() && !isBlank(lines[i]) && indentCols(lines[i]) >= 2 &&
                       !listItem(lines[i], &item, &content)) {
                    QString rest;
                    if (fenceOpen(lines[i], &rest))
                        break;
                    items.back().text += '\n' + convertInline(lines[i].trimmed());
                    ++i;
                }
                // A blank line only ends the list when no item follows it.
                int k = i;
                while (k < lines.size() && isBlank(lines[k]))
                    ++k;
                if (k < lines.size() && listItem(lines[k], &item, &content)) {
                    i = k;
                    continue;
                }
                break;
            }
            // Nesting level = rank of the indent among this list's indents, so
            // two-space and four-space habits nest the same. Slack caps sub-lists.
            std::vector<int> indents;
            for (const auto &it : items)
                indents.push_back(it.indent);
            std::sort(indents.begin(), indents.end());
            indents.erase(std::unique(indents.begin(), indents.end()), indents.end());
            for (auto &it : items) {
                const auto at = std::lower_bound(indents.begin(), indents.end(), it.indent);
                it.level      = std::min<int>(int(at - indents.begin()), 8);
            }
            // Fallback text like the official client's: "• a" / "1. a", numbered
            // consecutively from the first item of each run.
            int  number      = 0;
            int  prevLevel   = -1;
            bool prevOrdered = false;
            for (auto &it : items) {
                if (it.level != prevLevel || it.ordered != prevOrdered)
                    number = it.number;
                else
                    ++number;
                it.number            = number;
                prevLevel            = it.level;
                prevOrdered          = it.ordered;
                const QString marker = it.ordered ? QString::number(number) + QStringLiteral(". ")
                                                  : QStringLiteral("• ");
                out << QString(it.level * 4, ' ') + marker + it.text;
            }
            closeSection();
            chunks.push_back({Kind::List, {}, std::move(items)});
            anyList = true;
            continue;
        }

        // ── Blockquote ──
        if (QString prefix, content; quoteLine(line, &prefix, &content)) {
            QStringList quoted;
            while (i < lines.size() && quoteLine(lines[i], &prefix, &content)) {
                const QString conv = convertInline(content);
                out << prefix + conv;
                quoted << conv;
                ++i;
            }
            closeSection();
            chunks.push_back({Kind::Quote, quoted.join('\n'), {}});
            continue;
        }

        if (isBlank(line)) {
            out << line;
            ++pendingBlanks;
            ++i;
            continue;
        }

        paragraphLine(line);
        ++i;
    }

    Composed c;
    c.mrkdwn = out.join('\n');
    if (!anyList)
        return c;

    QJsonArray elements;
    for (const auto &ch : chunks) {
        switch (ch.kind) {
        case Kind::Section:
            elements.append(section("rich_text_section", ch.text));
            break;
        case Kind::Quote:
            elements.append(section("rich_text_quote", ch.text));
            break;
        case Kind::Pre:
            elements.append(
                QJsonObject{
                    {"type", "rich_text_preformatted"},
                    {"elements",
                     QJsonArray{QJsonObject{
                         {"type", "text"}, {"text", MrkdwnParser::decodeEntities(ch.text)}
                     }}},
                }
            );
            break;
        case Kind::List:
            for (const auto &v : listElements(ch.items))
                elements.append(v);
            break;
        }
    }
    c.blocks.append(QJsonObject{{"type", "rich_text"}, {"elements", elements}});
    return c;
}

std::vector<OutgoingGif> takeGifLinks(QString &composerText) {
    static const QRegularExpression kLink(QStringLiteral(R"(<(https?://[^|>\s]+)(?:\|([^>]*))?>)"));

    std::vector<OutgoingGif> gifs;
    QString                  rest;
    qsizetype                pos = 0;
    for (auto it = kLink.globalMatch(composerText); it.hasNext();) {
        const auto m = it.next();
        if (!LinkLabels::isGiphyMediaUrl(m.captured(1)))
            continue;
        // The composer puts a space on either side of the badge; drop one so
        // the text around it closes up.
        qsizetype start = m.capturedStart();
        qsizetype end   = m.capturedEnd();
        if (end < composerText.size() && composerText.at(end) == QLatin1Char(' '))
            ++end;
        else if (start > pos && composerText.at(start - 1) == QLatin1Char(' '))
            --start;
        rest += QStringView(composerText).mid(pos, start - pos);
        pos = end;
        gifs.push_back({m.captured(1), m.captured(2).trimmed()});
    }
    if (gifs.empty())
        return gifs;
    rest += QStringView(composerText).mid(pos);
    composerText = rest.trimmed();
    return gifs;
}

} // namespace MarkdownCompose
