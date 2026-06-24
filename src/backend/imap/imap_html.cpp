// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_html.h"

#include <QHash>
#include <QRegularExpression>

#include <vector>

namespace imap {

namespace {

bool isWs(QChar c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Collapsible whitespace for body text: the ASCII set above plus every Unicode
// space separator — non-breaking space (U+00A0), narrow/figure spaces, the
// en/em quad family, ideographic space, line/paragraph separators, etc. Marketing
// mail fills spacer cells with a *numeric* or literal NBSP (&#160; / U+00A0),
// which decodeHtmlEntities leaves as U+00A0; without this it reads as real text,
// flushes, and resets the blank-line cap so each spacer line opens a fresh gap.
// (QChar::isSpace already excludes zero-width formatting chars — those are dropped
// separately as invisible.)
bool isCollapsibleSpace(QChar c) {
    return isWs(c) || c.isSpace();
}

// Zero-width / invisible formatting chars. Marketing mail sprinkles these as
// filler (often one per line between <br>s) — kept as text they read as content
// and defeat blank-line collapsing, opening huge gaps. We drop them outright;
// being zero-width, removing them never changes the visible text.
bool isInvisible(QChar c) {
    const char16_t u = c.unicode();
    return u == 0x200B     // zero-width space
           || u == 0x200C  // zero-width non-joiner (&zwnj;)
           || u == 0x200D  // zero-width joiner (&zwj;)
           || u == 0x2060  // word joiner
           || u == 0xFEFF  // zero-width no-break space / BOM
           || u == 0x00AD; // soft hyphen
}

// Value of an attribute (e.g. href) within a tag's inner text. Quoted or bare.
QString attrValue(const QString &inner, const QString &name) {
    static const QString pat = QStringLiteral("\\b%1\\s*=\\s*(\"([^\"]*)\"|'([^']*)'|([^\\s>]+))");
    QRegularExpression   re(pat.arg(name), QRegularExpression::CaseInsensitiveOption);
    const auto           m = re.match(inner);
    if (!m.hasMatch())
        return {};
    if (!m.captured(2).isNull())
        return m.captured(2);
    if (!m.captured(3).isNull())
        return m.captured(3);
    return m.captured(4);
}

struct Open {
    EntityType type;
    int        start;
    QString    data;
};

// Void elements never have a closing tag — they must not start a hidden-subtree
// skip (there'd be no matching close, swallowing the rest of the document).
bool isVoidElement(const QByteArray &name) {
    return name == "br" || name == "hr" || name == "img" || name == "input" || name == "meta" ||
           name == "link" || name == "area" || name == "base" || name == "col" || name == "embed" ||
           name == "source" || name == "track" || name == "wbr" || name == "param";
}

// An element hidden from the reader: inline display:none / visibility:hidden, or
// the `hidden` attribute. Marketing mail ships hidden preheaders + mobile/desktop
// duplicate blocks this way; Gmail hides them, so we do too (skip the subtree).
bool isHiddenTag(const QString &inner) {
    const QString style = attrValue(inner, QStringLiteral("style")).remove(' ').toLower();
    if (style.contains(QLatin1String("display:none")) ||
        style.contains(QLatin1String("visibility:hidden")) ||
        style.contains(QLatin1String("visibility:collapse")))
        return true;
    static const QRegularExpression hiddenAttr(
        QStringLiteral("(^|\\s)hidden(\\s|=|/|$)"), QRegularExpression::CaseInsensitiveOption
    );
    return hiddenAttr.match(inner).hasMatch();
}

} // namespace

QString decodeHtmlEntities(const QString &s) {
    static const QHash<QString, QChar> named = {
        {"amp", '&'},
        {"lt", '<'},
        {"gt", '>'},
        {"quot", '"'},
        {"apos", '\''},
        {"nbsp", QChar(' ')},
        {"mdash", QChar(0x2014)},
        {"ndash", QChar(0x2013)},
        {"hellip", QChar(0x2026)},
        {"copy", QChar(0xA9)},
        {"reg", QChar(0xAE)},
        {"trade", QChar(0x2122)},
        {"rsquo", QChar(0x2019)},
        {"lsquo", QChar(0x2018)},
        {"rdquo", QChar(0x201D)},
        {"ldquo", QChar(0x201C)},
        {"bull", QChar(0x2022)},
        {"middot", QChar(0xB7)},
        {"sbquo", QChar(0x201A)},
        {"bdquo", QChar(0x201E)},
        {"dagger", QChar(0x2020)},
        {"Dagger", QChar(0x2021)},
        {"permil", QChar(0x2030)},
        {"prime", QChar(0x2032)},
        {"Prime", QChar(0x2033)},
        {"lsaquo", QChar(0x2039)},
        {"rsaquo", QChar(0x203A)},
        {"euro", QChar(0x20AC)},
        // Zero-width / spacing chars — common invisible filler in marketing mail.
        {"zwnj", QChar(0x200C)},
        {"zwj", QChar(0x200D)},
        {"shy", QChar(0xAD)},
        {"ensp", QChar(0x2002)},
        {"emsp", QChar(0x2003)},
        {"thinsp", QChar(0x2009)},
        // Latin-1 named entities — accented letters etc. used across Western
        // European languages (Swedish å/ä/ö, German ü/ß, French é/è, …).
        {"iexcl", QChar(0xA1)},
        {"cent", QChar(0xA2)},
        {"pound", QChar(0xA3)},
        {"curren", QChar(0xA4)},
        {"yen", QChar(0xA5)},
        {"brvbar", QChar(0xA6)},
        {"sect", QChar(0xA7)},
        {"uml", QChar(0xA8)},
        {"ordf", QChar(0xAA)},
        {"laquo", QChar(0xAB)},
        {"not", QChar(0xAC)},
        {"macr", QChar(0xAF)},
        {"deg", QChar(0xB0)},
        {"plusmn", QChar(0xB1)},
        {"sup2", QChar(0xB2)},
        {"sup3", QChar(0xB3)},
        {"acute", QChar(0xB4)},
        {"micro", QChar(0xB5)},
        {"para", QChar(0xB6)},
        {"cedil", QChar(0xB8)},
        {"sup1", QChar(0xB9)},
        {"ordm", QChar(0xBA)},
        {"raquo", QChar(0xBB)},
        {"frac14", QChar(0xBC)},
        {"frac12", QChar(0xBD)},
        {"frac34", QChar(0xBE)},
        {"iquest", QChar(0xBF)},
        {"Agrave", QChar(0xC0)},
        {"Aacute", QChar(0xC1)},
        {"Acirc", QChar(0xC2)},
        {"Atilde", QChar(0xC3)},
        {"Auml", QChar(0xC4)},
        {"Aring", QChar(0xC5)},
        {"AElig", QChar(0xC6)},
        {"Ccedil", QChar(0xC7)},
        {"Egrave", QChar(0xC8)},
        {"Eacute", QChar(0xC9)},
        {"Ecirc", QChar(0xCA)},
        {"Euml", QChar(0xCB)},
        {"Igrave", QChar(0xCC)},
        {"Iacute", QChar(0xCD)},
        {"Icirc", QChar(0xCE)},
        {"Iuml", QChar(0xCF)},
        {"ETH", QChar(0xD0)},
        {"Ntilde", QChar(0xD1)},
        {"Ograve", QChar(0xD2)},
        {"Oacute", QChar(0xD3)},
        {"Ocirc", QChar(0xD4)},
        {"Otilde", QChar(0xD5)},
        {"Ouml", QChar(0xD6)},
        {"times", QChar(0xD7)},
        {"Oslash", QChar(0xD8)},
        {"Ugrave", QChar(0xD9)},
        {"Uacute", QChar(0xDA)},
        {"Ucirc", QChar(0xDB)},
        {"Uuml", QChar(0xDC)},
        {"Yacute", QChar(0xDD)},
        {"THORN", QChar(0xDE)},
        {"szlig", QChar(0xDF)},
        {"agrave", QChar(0xE0)},
        {"aacute", QChar(0xE1)},
        {"acirc", QChar(0xE2)},
        {"atilde", QChar(0xE3)},
        {"auml", QChar(0xE4)},
        {"aring", QChar(0xE5)},
        {"aelig", QChar(0xE6)},
        {"ccedil", QChar(0xE7)},
        {"egrave", QChar(0xE8)},
        {"eacute", QChar(0xE9)},
        {"ecirc", QChar(0xEA)},
        {"euml", QChar(0xEB)},
        {"igrave", QChar(0xEC)},
        {"iacute", QChar(0xED)},
        {"icirc", QChar(0xEE)},
        {"iuml", QChar(0xEF)},
        {"eth", QChar(0xF0)},
        {"ntilde", QChar(0xF1)},
        {"ograve", QChar(0xF2)},
        {"oacute", QChar(0xF3)},
        {"ocirc", QChar(0xF4)},
        {"otilde", QChar(0xF5)},
        {"ouml", QChar(0xF6)},
        {"divide", QChar(0xF7)},
        {"oslash", QChar(0xF8)},
        {"ugrave", QChar(0xF9)},
        {"uacute", QChar(0xFA)},
        {"ucirc", QChar(0xFB)},
        {"uuml", QChar(0xFC)},
        {"yacute", QChar(0xFD)},
        {"thorn", QChar(0xFE)},
        {"yuml", QChar(0xFF)},
    };
    QString out;
    out.reserve(s.size());
    const int n = s.size();
    for (int i = 0; i < n;) {
        const QChar c = s[i];
        if (c == '&') {
            const int semi = s.indexOf(';', i + 1);
            if (semi > 0 && semi - i <= 12) {
                const QString ent = s.mid(i + 1, semi - i - 1);
                if (ent.startsWith('#')) {
                    bool      ok   = false;
                    const int code = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                         ? ent.mid(2).toInt(&ok, 16)
                                         : ent.mid(1).toInt(&ok, 10);
                    if (ok && code > 0) {
                        if (code <= 0xFFFF) {
                            out += QChar(static_cast<ushort>(code));
                        } else {
                            const char32_t u = static_cast<char32_t>(code);
                            out += QString::fromUcs4(&u, 1);
                        }
                        i = semi + 1;
                        continue;
                    }
                } else if (named.contains(ent)) {
                    out += named.value(ent);
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

TextWithEntities htmlToEntities(const QString &html) {
    TextWithEntities  out;
    QString          &text = out.text;
    std::vector<Open> stack;
    constexpr int     kMaxNL       = 3; // cap consecutive newlines (≤2 blank lines) — no giant gaps
    bool              pendingSpace = false;
    int               pendingNL = 0; // line breaks buffered before the next text (capped at kMaxNL)
    int               preDepth  = 0;
    int               skipDepth = 0;
    QByteArray        skipTag;
    int               hiddenDepth = 0; // inside a display:none / hidden subtree
    QByteArray        hiddenTag;       // the tag name that opened the hidden subtree

    // Newlines/spaces are buffered, not written immediately, so we can collapse
    // runs and cap blank lines. flush() commits them just before real text — and
    // suppresses leading whitespace so the message never starts with blank lines.
    auto flush = [&] {
        if (pendingNL > 0) {
            if (!text.isEmpty()) // suppress leading newlines
                for (int k = 0; k < pendingNL; ++k)
                    text += '\n';
            pendingNL    = 0;
            pendingSpace = false;
        } else if (pendingSpace) {
            if (!text.isEmpty() && !text.endsWith('\n'))
                text += ' ';
            pendingSpace = false;
        }
    };
    // A single hard break (<br>); two in a row make a blank line. Capped at 2 so a
    // run of breaks can't open up a big gap.
    auto breakLine = [&] {
        pendingNL    = qMin(kMaxNL, pendingNL + 1);
        pendingSpace = false;
    };
    // A block boundary: want=1 puts following content on its own line, want=2
    // leaves a blank line between blocks (paragraphs). Merges with pending breaks.
    auto blockBreak = [&](int want) {
        pendingNL    = qMin(kMaxNL, qMax(pendingNL, want));
        pendingSpace = false;
    };
    auto appendText = [&](const QString &raw) {
        const QString dec = decodeHtmlEntities(raw);
        if (preDepth > 0) { // <pre>: keep newlines/spacing verbatim
            flush();
            text += dec;
            return;
        }
        for (QChar c : dec) {
            if (isInvisible(c)) {
                continue; // drop zero-width filler so blank-looking lines collapse
            } else if (isCollapsibleSpace(c)) {
                if (pendingNL == 0) // a pending break already implies the space
                    pendingSpace = true;
            } else {
                flush();
                text += c;
            }
        }
    };
    auto inlineType = [](const QByteArray &nm, bool &ok) -> EntityType {
        ok = true;
        if (nm == "b" || nm == "strong")
            return EntityType::Bold;
        if (nm == "i" || nm == "em")
            return EntityType::Italic;
        if (nm == "u" || nm == "ins")
            return EntityType::Underline;
        if (nm == "s" || nm == "strike" || nm == "del")
            return EntityType::Strike;
        if (nm == "code" || nm == "tt" || nm == "kbd" || nm == "samp")
            return EntityType::Code;
        if (nm == "pre")
            return EntityType::Pre;
        if (nm == "blockquote")
            return EntityType::Blockquote;
        if (nm == "a")
            return EntityType::Link;
        // Headings have no dedicated entity in the message model; render their
        // text bold (they're also in the `block` set, so they get their own line).
        if (nm == "h1" || nm == "h2" || nm == "h3" || nm == "h4" || nm == "h5" || nm == "h6")
            return EntityType::Bold;
        ok = false;
        return EntityType::Bold;
    };

    const int n = html.size();
    for (int i = 0; i < n;) {
        if (html[i] == '<') {
            if (html.mid(i, 4) == QLatin1String("<!--")) {
                const int e = html.indexOf("-->", i + 4);
                i           = e < 0 ? n : e + 3;
                continue;
            }
            const int gt = html.indexOf('>', i + 1);
            if (gt < 0)
                break;
            QString t          = html.mid(i + 1, gt - i - 1).trimmed();
            i                  = gt + 1;
            const bool closing = t.startsWith('/');
            if (closing)
                t = t.mid(1).trimmed();
            int sp = 0;
            while (sp < t.size() && !isWs(t[sp]) && t[sp] != '/')
                ++sp;
            const QByteArray name        = t.left(sp).toLower().toUtf8();
            const QString    inner       = t.mid(sp);
            const bool       selfClosing = t.endsWith('/');

            // Inside a hidden subtree: drop everything until its matching close,
            // counting nested same-name tags so we stop at the right one.
            if (hiddenDepth > 0) {
                if (!closing && name == hiddenTag && !selfClosing)
                    ++hiddenDepth;
                else if (closing && name == hiddenTag)
                    --hiddenDepth;
                continue;
            }
            // A non-void element marked display:none / hidden starts a skip.
            if (!closing && !selfClosing && !isVoidElement(name) && isHiddenTag(inner)) {
                hiddenDepth = 1;
                hiddenTag   = name;
                continue;
            }

            if (skipDepth > 0) { // inside <script>/<style>/<head>/<title>
                if (closing && name == skipTag)
                    skipDepth = 0;
                continue;
            }
            if (!closing &&
                (name == "script" || name == "style" || name == "head" || name == "title")) {
                skipDepth = 1;
                skipTag   = name;
                continue;
            }
            if (name == "br" || name == "hr") {
                breakLine();
                continue;
            }
            if (name == "img")
                continue; // dropped (inline-image resolution is a later step)

            // Paragraph-level blocks get a blank line around them; line-level
            // blocks just start a new line. (0 = inline, not a block.)
            const int blockWant =
                (name == "p" || name == "blockquote" || name == "ul" || name == "ol" ||
                 name == "table" || name == "h1" || name == "h2" || name == "h3" || name == "h4" ||
                 name == "h5" || name == "h6")
                    ? 2
                : (name == "div" || name == "li" || name == "tr" || name == "header" ||
                   name == "footer" || name == "section" || name == "article" || name == "figure" ||
                   name == "figcaption" || name == "dl" || name == "dd" || name == "dt" ||
                   name == "pre")
                    ? 1
                    : 0;

            bool             isInline = false;
            const EntityType et       = inlineType(name, isInline);

            if (!closing) {
                if (name == "pre")
                    ++preDepth;
                if (blockWant)
                    blockBreak(blockWant);
                if (name == "li") {
                    flush(); // commit the line break so the bullet starts the line
                    text += QStringLiteral("• ");
                }
                if (isInline) {
                    flush(); // so the span starts at the first real character
                    QString data;
                    if (et == EntityType::Link)
                        data = decodeHtmlEntities(attrValue(inner, QStringLiteral("href")));
                    stack.push_back({et, int(text.size()), data});
                }
            } else {
                if (name == "pre" && preDepth > 0)
                    --preDepth;
                if (isInline) { // pop before the block break so the span excludes it
                    for (int k = int(stack.size()) - 1; k >= 0; --k)
                        if (stack[k].type == et) {
                            const Open o = stack[k];
                            stack.erase(stack.begin() + k);
                            const int len = int(text.size()) - o.start;
                            if (len > 0)
                                out.entities.push_back({o.type, o.start, len, o.data});
                            break;
                        }
                }
                if (blockWant)
                    blockBreak(blockWant);
            }
            continue;
        }
        int lt = html.indexOf('<', i);
        if (lt < 0)
            lt = n;
        if (skipDepth == 0 && hiddenDepth == 0) // not inside skipped/hidden content
            appendText(html.mid(i, lt - i));
        i = lt;
    }

    // Close any still-open inline entities.
    for (const Open &o : stack) {
        const int len = int(text.size()) - o.start;
        if (len > 0)
            out.entities.push_back({o.type, o.start, len, o.data});
    }
    // Trim leading/trailing blank lines, fixing entity offsets for a leading trim.
    while (!text.isEmpty() && (text.endsWith('\n') || text.endsWith(' ')))
        text.chop(1);
    int lead = 0;
    while (lead < text.size() && (text[lead] == '\n' || text[lead] == ' '))
        ++lead;
    if (lead > 0) {
        text = text.mid(lead);
        for (auto &e : out.entities) {
            e.offset = qMax(0, e.offset - lead);
            if (e.offset + e.length > text.size())
                e.length = qMax(0, text.size() - e.offset);
        }
    }
    return out;
}

} // namespace imap
