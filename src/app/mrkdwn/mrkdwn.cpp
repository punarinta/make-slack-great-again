// A single-pass scanner over UTF-8, with byte offsets. Every special
// character of the grammar is ASCII and UTF-8 continuation bytes never equal
// an ASCII byte, so the scanner walks bytes; only the word-character test
// ('_' inside words) decodes code points.
#include "app/mrkdwn/mrkdwn.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"

#include <algorithm>
#include <charconv>

namespace mrkdwn {

namespace {

using str::endsWith;
using str::startsWith;

struct Builder {
    std::string         text;
    std::vector<Entity> entities;

    void addSpan(Kind kind, uint32_t start, std::string data = {}) {
        entities.push_back(Entity{kind, start, uint32_t(text.size()) - start, std::move(data)});
    }

    // Append a recursively parsed fragment wrapped in an outer span. The outer
    // entity goes BEFORE the shifted inner ones so parents precede children.
    void appendNested(Kind kind, Rich sub, std::string data = {}) {
        const uint32_t start = uint32_t(text.size());
        text += sub.text;
        entities.push_back(Entity{kind, start, uint32_t(sub.text.size()), std::move(data)});
        for (auto &e : sub.entities) {
            e.start += start;
            entities.push_back(std::move(e));
        }
    }

    Rich take() { return Rich{std::move(text), std::move(entities)}; }
};

// Code point that ends just before byte `pos` / starts at `pos`.
uint32_t cpBefore(std::string_view s, size_t pos) {
    size_t p = utf8::prevBoundary(s, pos);
    return utf8::decode(s, p);
}

uint32_t cpAt(std::string_view s, size_t pos) {
    return utf8::decode(s, pos);
}

// Try to consume a paired delimiter (*, ~, `) starting at `start`: the
// position after the closer, or npos. Inline marks never cross a newline.
size_t findClose(std::string_view src, size_t start, char delim) {
    for (size_t i = start; i < src.size(); ++i) {
        if (src[i] == delim && (i == 0 || src[i - 1] != '\\'))
            return i + 1;
        if (src[i] == '\n')
            return std::string_view::npos;
    }
    return std::string_view::npos;
}

// Slack reads '_' inside a word as a literal, so snake_case names and
// FILE_NAME_LIKE_THIS stay intact: an opener can't follow a letter or digit,
// and a closer can't precede one. A run of underscores counts as one mark.
bool underscoreOpens(std::string_view src, size_t pos) {
    while (pos > 0 && src[pos - 1] == '_')
        --pos;
    return pos == 0 || !utf8::isWordChar(cpBefore(src, pos));
}

// Position after the `width` closing underscores (_ italic, __ underline), or npos.
size_t findUnderscoreClose(std::string_view src, size_t start, size_t width) {
    for (size_t i = start; i + width <= src.size(); ++i) {
        if (src[i] == '\n')
            return std::string_view::npos;
        bool all = i == 0 || src[i - 1] != '\\';
        for (size_t k = 0; k < width && all; ++k)
            all = src[i + k] == '_';
        if (!all)
            continue;
        const size_t after = i + width;
        if (after == src.size() || !utf8::isWordChar(cpAt(src, after)))
            return after;
    }
    return std::string_view::npos;
}

size_t findCodeFenceClose(std::string_view src, size_t pos) {
    while (pos + 2 < src.size()) {
        if (src[pos] == '`' && src[pos + 1] == '`' && src[pos + 2] == '`')
            return pos + 3;
        ++pos;
    }
    return std::string_view::npos;
}

bool isEmojiNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '+' || c == '-';
}

bool validEmojiName(std::string_view name) {
    if (name.empty())
        return false;
    for (char c : name)
        if (!isEmojiNameChar(c))
            return false;
    return true;
}

// "Today"/"Yesterday"/"Tomorrow" when the date is adjacent to now.
std::string prettyDay(int64_t secs, std::string absolute) {
    const int64_t d = base::localDay(secs) - base::localDay(base::nowSecs());
    if (d == 0)
        return i18n::tr("Today");
    if (d == -1)
        return i18n::tr("Yesterday");
    if (d == 1)
        return i18n::tr("Tomorrow");
    return absolute;
}

// Render a <!date^ts^format…> format string ("{date_short} at {time}").
// Empty when it holds a token we don't know — the caller then shows the
// sender's fallback text, as Slack's docs mandate.
std::string formatDateToken(int64_t secs, std::string_view fmt) {
    const int64_t now = base::nowSecs();
    std::string   out;
    size_t        i = 0;
    while (i < fmt.size()) {
        // \{([a-z_]+)\}
        if (fmt[i] == '{') {
            size_t j = i + 1;
            while (j < fmt.size() && ((fmt[j] >= 'a' && fmt[j] <= 'z') || fmt[j] == '_'))
                ++j;
            if (j > i + 1 && j < fmt.size() && fmt[j] == '}') {
                const std::string_view name = fmt.substr(i + 1, j - i - 1);
                if (name == "date_num") {
                    out += base::isoDate(secs);
                } else if (name == "date" || name == "date_short") {
                    out += base::formatDate(secs, now);
                } else if (name == "date_pretty" || name == "date_short_pretty") {
                    out += prettyDay(secs, base::formatDate(secs, now));
                } else if (name == "date_long" || name == "date_long_pretty") {
                    // "Friday, March 15" (", 2025" in another year): the
                    // day name + ", " + the date, in the date language.
                    const std::string longDate = str::concat(
                        {base::weekdayName(base::localTime(secs).weekday),
                         ", ",
                         base::formatDate(secs, now)}
                    );
                    out += name == "date_long" ? longDate : prettyDay(secs, longDate);
                } else if (name == "time") {
                    out += base::formatTime(secs);
                } else if (name == "time_secs") {
                    out += base::formatTimeSecs(secs);
                } else if (name == "ago") {
                    out += base::relativeTime(secs, now);
                } else {
                    return {}; // unknown token → fallback text
                }
                i = j + 1;
                continue;
            }
        }
        out += fmt[i++];
    }
    return out;
}

bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

// True if the colon pair [open, close] is part of a number rather than a
// shortcode: an all-digit "name" touching a digit on either side, i.e.
// "14:43:34" (a time), "1:2:3", scores and ratios. A real digit-only
// shortcode (":100:") is unaffected as long as it isn't glued to a number.
bool numericColonRun(std::string_view s, size_t open, size_t close) {
    for (size_t k = open + 1; k < close; ++k)
        if (!isAsciiDigit(s[k]))
            return false;
    return (open > 0 && isAsciiDigit(s[open - 1])) ||
           (close + 1 < s.size() && isAsciiDigit(s[close + 1]));
}

// Which bytes sit inside a URL-looking word: a path or query can carry colon
// pairs ("…/a:b:c") that are not emoji. Judged per whitespace-delimited word,
// marked in one pass (testing each match's word on demand would be quadratic
// on colon-heavy input).
std::string urlWordMask(std::string_view s) {
    std::string mask(s.size(), 0); // a byte per input byte: 1 = inside a URL word
    size_t      i       = 0;
    auto        spaceAt = [&](size_t p, size_t *next) {
        size_t     q  = p;
        const bool sp = utf8::isSpace(utf8::decode(s, q));
        *next         = q;
        return sp;
    };
    while (i < s.size()) {
        size_t next;
        while (i < s.size() && spaceAt(i, &next))
            i = next;
        const size_t start = i;
        while (i < s.size() && !spaceAt(i, &next))
            i = next;
        if (i > start && looksLikeUrl(s.substr(start, i - start)))
            for (size_t k = start; k < i; ++k)
                mask[k] = 1;
    }
    return mask;
}

// Append `s`, adding an Emoji span per :name: shortcode (link labels carry
// no marks, but Slack's clients do render emoji in them — CI bots title
// notifications ":white_check_mark: …" inside a <url|label>).
void appendPlainWithEmoji(Builder &b, std::string_view s) {
    const auto inUrl = urlWordMask(s);
    size_t     pos = 0, search = 0;
    while (true) {
        // Next match of :([a-zA-Z0-9_+-]+): at or after `search`.
        size_t open = s.find(':', search);
        if (open == std::string_view::npos)
            break;
        size_t q = open + 1;
        while (q < s.size() && isEmojiNameChar(s[q]))
            ++q;
        if (q == open + 1 || q >= s.size() || s[q] != ':') {
            search = open + 1;
            continue;
        }
        search = q + 1; // the regex iterator resumes after the match, used or not
        if (inUrl[open] || numericColonRun(s, open, q))
            continue;
        b.text.append(s.substr(pos, open - pos));
        const uint32_t start = uint32_t(b.text.size());
        b.text.append(s.substr(open, q + 1 - open));
        b.addSpan(Kind::Emoji, start, std::string(s.substr(open + 1, q - open - 1)));
        pos = q + 1;
    }
    b.text.append(s.substr(pos));
}

// Append the resolved content of a single <…> construct: a mention, an
// <!command> (incl. <!date^…>), or a <url|label> link. With requireScheme a
// plain link is only linkified if its URL has a scheme, so a bare "<word>"
// stays literal; parse() passes false because Slack escapes real '<' to &lt;.
void appendAngleConstruct(Builder &b, std::string_view inner, bool requireScheme) {
    if (startsWith(inner, "@")) { // <@U…> or <@U…|name>
        const auto        parts = str::split(inner.substr(1), '|');
        const std::string uid(parts[0]);
        const std::string label =
            parts.size() > 1 ? decodeEntities(parts[1]) : str::concat({"@", uid});
        const uint32_t start = uint32_t(b.text.size());
        b.text += label;
        b.addSpan(Kind::User, start, uid);
        return;
    }
    if (startsWith(inner, "#")) { // <#C…|name>
        const auto        parts = str::split(inner.substr(1), '|');
        const std::string cid(parts[0]);
        const std::string name  = parts.size() > 1 ? decodeEntities(parts[1]) : cid;
        const uint32_t    start = uint32_t(b.text.size());
        b.text += '#';
        b.text += name;
        b.addSpan(Kind::Channel, start, cid);
        return;
    }
    if (startsWith(inner, "!")) {
        const std::string_view cmd   = inner.substr(1);
        const uint32_t         start = uint32_t(b.text.size());
        if (cmd == "here") {
            b.text += "@here";
            b.addSpan(Kind::Here, start);
        } else if (cmd == "channel") {
            b.text += "@channel";
            b.addSpan(Kind::ChannelCmd, start);
        } else if (startsWith(cmd, "date^")) {
            // <!date^unix-ts^format-string[^link]|fallback>
            const size_t      pipe     = cmd.find('|');
            const std::string fallback = pipe != std::string_view::npos
                                             ? decodeEntities(cmd.substr(pipe + 1))
                                             : std::string();
            std::string_view  head     = pipe != std::string_view::npos ? cmd.substr(0, pipe) : cmd;
            const std::vector<std::string_view> parts = str::split(head, '^');
            int64_t                             secs  = 0;
            bool                                tsOk  = false;
            if (parts.size() > 1 && !parts[1].empty()) {
                const auto r =
                    std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), secs);
                tsOk = r.ec == std::errc() && r.ptr == parts[1].data() + parts[1].size();
            }
            std::string rendered =
                tsOk ? formatDateToken(secs, decodeEntities(parts.size() > 2 ? parts[2] : "")) : "";
            if (rendered.empty())
                rendered = fallback;
            const std::string link = parts.size() > 3 ? decodeEntities(parts[3]) : std::string();
            b.text += rendered;
            if (!link.empty())
                b.addSpan(Kind::Link, start, link);
        } else if (startsWith(cmd, "subteam^")) {
            // <!subteam^S…|@handle>: a user-group mention. The label is
            // optional (Grid member workspaces and bots omit it) and, when
            // present, already carries the '@'. The span carries the id so the
            // renderer can swap in the live handle.
            const size_t      pipe = cmd.find('|');
            const std::string id(
                pipe != std::string_view::npos ? cmd.substr(8, pipe - 8) : cmd.substr(8)
            );
            std::string label =
                pipe != std::string_view::npos ? decodeEntities(cmd.substr(pipe + 1)) : id;
            if (startsWith(label, "@"))
                label.erase(0, 1);
            b.text += '@';
            b.text += label;
            b.addSpan(Kind::Usergroup, start, id);
        } else {
            // <!everyone> and unknown commands: show as @name.
            const auto        parts = str::split(cmd, '|');
            const std::string label =
                parts.size() > 1 ? decodeEntities(parts.back()) : std::string(cmd);
            b.text += '@';
            b.text += label;
            b.addSpan(Kind::Here, start, std::string(cmd));
        }
        return;
    }

    // <url|label> or <url>
    const auto parts = str::split(inner, '|');
    if (requireScheme && !looksLikeUrl(parts[0])) {
        b.text += '<';
        b.text += inner;
        b.text += '>';
        return;
    }
    const std::string url   = decodeEntities(parts[0]);
    const std::string label = parts.size() > 1 ? decodeEntities(parts[1]) : url;
    // msga's own link to a thread: a chip of its label.
    if (const MessageRef ref = parseThreadLink(url); ref.valid()) {
        const uint32_t start = uint32_t(b.text.size());
        b.text += label;
        b.addSpan(Kind::MessageLink, start, refToToken(ref));
        return;
    }
    // A link to another message becomes a chip — only when the URL is all the
    // author wrote. Slack echoes a pasted permalink as "<url|url>", so an
    // equal label still counts as bare; real link text stays a plain link.
    if (label == url) {
        const MessageRef ref = parseMessageLink(url);
        if (ref.valid()) {
            const uint32_t start = uint32_t(b.text.size());
            b.text += url;
            b.addSpan(Kind::MessageLink, start, refToToken(ref));
            return;
        }
    }
    // Emoji nest inside the Link (pushed first, so parent-before-child holds
    // even for a one-emoji label). A bare <url> shows the URL itself — never
    // emoji-scanned ("/a:b:c" path segments would false-match).
    Builder sub;
    if (parts.size() > 1)
        appendPlainWithEmoji(sub, label);
    else
        sub.text = label;
    b.appendNested(Kind::Link, sub.take(), url);
}

// msga's own escape for a mark character meant literally ("&#42;" for '*'):
// a printable ASCII character as a decimal reference. Slack never sends a
// bare '&' (it's always "&amp;"), so no Slack text reads differently. The
// length of the reference at `pos` (its character in *out), or 0.
size_t asciiRef(std::string_view s, size_t pos, char *out) {
    if (s.substr(pos, 2) != "&#")
        return 0;
    int    v = 0;
    size_t i = pos + 2;
    for (; i < s.size() && i < pos + 5 && s[i] >= '0' && s[i] <= '9'; ++i)
        v = v * 10 + (s[i] - '0');
    if (i == pos + 2 || i >= s.size() || s[i] != ';' || v < 32 || v > 126)
        return 0;
    *out = char(v);
    return i + 1 - pos;
}

// The entity at `pos` — Slack's &lt; &gt; &amp;, or an asciiRef — as its
// character in *out; its length, or 0 for none.
size_t entityAt(std::string_view s, size_t pos, char *out) {
    if (s[pos] != '&')
        return 0;
    if (s.substr(pos, 4) == "&lt;") {
        *out = '<';
        return 4;
    }
    if (s.substr(pos, 4) == "&gt;") {
        *out = '>';
        return 4;
    }
    if (s.substr(pos, 5) == "&amp;") {
        *out = '&';
        return 5;
    }
    return asciiRef(s, pos, out);
}

// ":name:" at `pos` (not part of a time like 10:30:00) appended as an Emoji
// span; the index after it, or 0 when there is none.
size_t emojiAt(Builder &b, std::string_view src, size_t pos) {
    const size_t close = src.find(':', pos + 1);
    if (close == std::string_view::npos || close <= pos + 1)
        return 0;
    const std::string_view name = src.substr(pos + 1, close - pos - 1);
    if (!validEmojiName(name) || numericColonRun(src, pos, close))
        return 0;
    const uint32_t start = uint32_t(b.text.size());
    b.text += ':';
    b.text += name;
    b.text += ':';
    b.addSpan(Kind::Emoji, start, std::string(name));
    return close + 1;
}

// A leading "&gt;" is what the API sends for a typed '>'.
size_t quoteMarkLen(std::string_view s, size_t pos) {
    if (pos >= s.size())
        return 0;
    if (s[pos] == '>')
        return 1;
    return s.substr(pos, 4) == "&gt;" ? 4 : 0;
}

// "No closer from `from` to the end of its line" — a closer search that
// failed answers every later opener on that line too (a line of " _a_b"
// tokens or lone '*'s is then scanned once, not once per opener).
struct NoCloser {
    size_t from = std::string_view::npos, to = 0; // [from, to): to = the line's end
    bool   covers(size_t start) const { return start >= from && start < to; }
    void   note(std::string_view src, size_t start) {
        from = start;
        to   = std::min(src.find('\n', start), src.size());
    }
};

Rich parseImpl(std::string_view src, int depth, bool inQuote) {
    Builder          b;
    size_t           i    = 0;
    const size_t     n    = src.size();
    constexpr size_t npos = std::string_view::npos;
    // Per mark: '`', '*', '~', "__", '_'.
    NoCloser         none[5];
    bool             noGt  = false; // no '>' left after a '<': none for any later one
    const auto       close = [&](NoCloser &nc, size_t start, size_t found) {
        if (found == npos)
            nc.note(src, start);
        return found;
    };

    if (depth > kMaxParseDepth) {
        b.text = decodeEntities(src);
        return b.take();
    }

    while (i < n) {
        const char c = src[i];

        // ── Code fence ``` ──
        if (c == '`' && i + 2 < n && src[i + 1] == '`' && src[i + 2] == '`') {
            size_t       contentStart = i + 3;
            // Skip a language hint ("```js\n") on the fence line; any other
            // text there ("```Can you…\n") is the first code line.
            const size_t lineEnd      = src.find('\n', contentStart);
            if (lineEnd != npos && isLanguageHint(src.substr(contentStart, lineEnd - contentStart)))
                contentStart = lineEnd + 1;
            const size_t closePos = findCodeFenceClose(src, contentStart);
            if (closePos != npos) {
                const uint32_t start = uint32_t(b.text.size());
                b.text += decodeEntities(src.substr(contentStart, closePos - 3 - contentStart));
                b.addSpan(Kind::Pre, start);
                i = closePos;
                continue;
            }
        }

        // ── Inline code ` ──
        if (c == '`' && !none[0].covers(i + 1)) {
            const size_t end = close(none[0], i + 1, findClose(src, i + 1, '`'));
            if (end != npos) {
                const uint32_t start = uint32_t(b.text.size());
                b.text += decodeEntities(src.substr(i + 1, end - 1 - (i + 1)));
                b.addSpan(Kind::Code, start);
                i = end;
                continue;
            }
        }

        // ── Bold *text* ──
        if (c == '*' && !none[1].covers(i + 1)) {
            const size_t end = close(none[1], i + 1, findClose(src, i + 1, '*'));
            if (end != npos) {
                b.appendNested(
                    Kind::Bold, parseImpl(src.substr(i + 1, end - 1 - (i + 1)), depth + 1, inQuote)
                );
                i = end;
                continue;
            }
        }

        // ── Underline __text__ (before single-_ italic) ──
        if (c == '_' && i + 1 < n && src[i + 1] == '_' && !none[3].covers(i + 2) &&
            underscoreOpens(src, i)) {
            const size_t end = close(none[3], i + 2, findUnderscoreClose(src, i + 2, 2));
            if (end != npos) {
                b.appendNested(
                    Kind::Underline,
                    parseImpl(src.substr(i + 2, end - 2 - (i + 2)), depth + 1, inQuote)
                );
                i = end;
                continue;
            }
        }

        // ── Italic _text_ ──
        if (c == '_' && !none[4].covers(i + 1) && underscoreOpens(src, i)) {
            const size_t end = close(none[4], i + 1, findUnderscoreClose(src, i + 1, 1));
            if (end != npos) {
                b.appendNested(
                    Kind::Italic,
                    parseImpl(src.substr(i + 1, end - 1 - (i + 1)), depth + 1, inQuote)
                );
                i = end;
                continue;
            }
        }

        // ── Strikethrough ~text~ ──
        if (c == '~' && !none[2].covers(i + 1)) {
            const size_t end = close(none[2], i + 1, findClose(src, i + 1, '~'));
            if (end != npos) {
                b.appendNested(
                    Kind::Strike,
                    parseImpl(src.substr(i + 1, end - 1 - (i + 1)), depth + 1, inQuote)
                );
                i = end;
                continue;
            }
        }

        // ── Angle-bracket constructs <…> ──
        if (c == '<' && !noGt) {
            const size_t end = src.find('>', i + 1);
            if (end != npos) {
                const std::string_view inner = src.substr(i + 1, end - i - 1);
                i                            = end + 1;
                appendAngleConstruct(b, inner, /*requireScheme=*/false);
                continue;
            }
            noGt = true;
        }

        // ── Emoji :name: ──
        if (c == ':') {
            if (const size_t next = emojiAt(b, src, i)) {
                i = next;
                continue;
            }
        }

        // ── Blockquote (> at line start; the API escapes it to &gt;) ──
        // All consecutive >-prefixed lines become one Quote entity whose
        // content is parsed for inline constructs. Inside a quote a further
        // '>' is literal (one level, like Slack).
        if (!inQuote && (i == 0 || src[i - 1] == '\n') && quoteMarkLen(src, i) > 0) {
            // Drop preceding newlines: the quote is its own block.
            while (!b.text.empty() && b.text.back() == '\n')
                b.text.pop_back();
            // Clamped: a span ending in a dropped newline (```code\n```) would
            // point past the text.
            for (auto &e : b.entities)
                if (e.end() > b.text.size())
                    e.length = e.start >= b.text.size() ? 0 : uint32_t(b.text.size()) - e.start;
            std::string quoted;
            bool        first = true;
            while (size_t markLen = quoteMarkLen(src, i)) {
                if (!first)
                    quoted += '\n';
                first = false;
                i += markLen;
                if (i < n && src[i] == ' ')
                    ++i; // optional space
                while (i < n && src[i] != '\n')
                    quoted += src[i++];
                if (i < n)
                    ++i; // '\n'
                if (quoteMarkLen(src, i) == 0)
                    break;
            }
            b.appendNested(Kind::Quote, parseImpl(quoted, depth + 1, true));
            b.text += '\n'; // the line break after the quote
            continue;
        }

        // ── HTML entities (&lt; &gt; &amp;) — Slack escapes these in all text ──
        char lit = 0;
        if (const size_t len = entityAt(src, i, &lit)) {
            b.text += lit;
            i += len;
            continue;
        }

        b.text += c;
        ++i;
    }
    return b.take();
}

bool isUrlChar(char c) {
    const unsigned char u = uint8_t(c);
    return u > ' ' && c != '<' && c != '>' && c != '"' && c != '\'' && c != '`';
}

// The bare http(s) URLs of `text`, stopping at a byte `covered` marks (the
// ones a token of the text already owns; empty: nothing stops them).
std::vector<BareUrl> scanBareUrls(std::string_view text, const std::string &covered) {
    const auto           at = [&covered](size_t i) { return !covered.empty() && covered[i]; };
    std::vector<BareUrl> out;
    const size_t         n = text.size();
    for (size_t i = 0; i + 8 < n; ++i) {
        size_t scheme = 0;
        if (text.compare(i, 8, "https://") == 0)
            scheme = 8;
        else if (text.compare(i, 7, "http://") == 0)
            scheme = 7;
        if (!scheme || at(i))
            continue;
        size_t j = i + scheme;
        while (j < n && isUrlChar(text[j]) && !at(j))
            ++j;
        // Sentence punctuation after a URL isn't part of it, nor is a ")"
        // closing a parenthesis the URL sits in.
        std::string_view url(text.data() + i, j - i);
        while (!url.empty()) {
            const char c = url.back();
            if (std::string_view(".,;:!?*_~").find(c) != std::string_view::npos ||
                (c == ')' &&
                 std::count(url.begin(), url.end(), '(') < std::count(url.begin(), url.end(), ')')))
                url.remove_suffix(1);
            else
                break;
        }
        if (url.size() <= scheme) // the scheme alone
            continue;
        out.push_back({uint32_t(i), uint32_t(i + url.size())});
        i += url.size() - 1;
    }
    return out;
}

// Bare http(s) URLs in the output text become Link entities — where the text
// is not already a token (links, mentions, emoji) or a code block. Inside
// inline `code` they are linked too.
void linkifyBareUrls(Rich &r) {
    if (r.text.find("://") == std::string::npos)
        return;
    std::string covered(r.text.size(), 0); // 1 = inside a token
    for (const auto &e : r.entities) {
        switch (e.kind) {
        case Kind::Bold:
        case Kind::Italic:
        case Kind::Underline:
        case Kind::Strike:
        case Kind::Code:
        case Kind::Quote:
            break;
        default:
            for (uint32_t k = e.start; k < e.end() && k < covered.size(); ++k)
                covered[k] = 1;
        }
    }
    bool added = false;
    for (const BareUrl &u : scanBareUrls(r.text, covered)) {
        r.entities.push_back(
            Entity{
                Kind::Link,
                u.from,
                u.to - u.from,
                std::string(std::string_view(r.text).substr(u.from, u.to - u.from))
            }
        );
        added = true;
    }
    if (!added)
        return;
    // Stable insertion sort into parent-first order (offset asc, longer
    // first): entity lists are short and std::stable_sort costs ~2 KB of code.
    auto before = [](const Entity &a, const Entity &b) {
        return a.start != b.start ? a.start < b.start : a.length > b.length;
    };
    for (size_t i = 1; i < r.entities.size(); ++i) {
        for (size_t j = i; j > 0 && before(r.entities[j], r.entities[j - 1]); --j)
            std::swap(r.entities[j], r.entities[j - 1]);
    }
}

} // namespace

// A URL with a scheme we linkify. Tells a real <url|label> token from a
// literal "<word>" in a rich_text run, where '<' is not escaped.
bool looksLikeUrl(std::string_view s) {
    return s.find("://") != std::string_view::npos || startsWith(s, "mailto:") ||
           startsWith(s, "tel:");
}

std::vector<BareUrl> bareUrls(std::string_view text) {
    return scanBareUrls(text, {});
}

// "js", "c++", "objective-c": one identifier-ish word, as the fence info
// string. ^[A-Za-z][A-Za-z0-9_+#.-]{0,29}$
bool isLanguageHint(std::string_view info) {
    if (info.empty() || info.size() > 30)
        return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!alpha(info[0]))
        return false;
    for (char c : info.substr(1))
        if (!alpha(c) && !(c >= '0' && c <= '9') && c != '_' && c != '+' && c != '#' && c != '.' &&
            c != '-')
            return false;
    return true;
}

std::string escapeEntities(std::string_view s) {
    return str::escapeHtml(s);
}

std::string decodeEntities(std::string_view s) {
    // Slack escapes exactly these three. One left-to-right pass is equivalent
    // to replacing &lt;, then &gt;, then &amp;: "&amp;lt;"
    // decodes to the literal "&lt;" because the "lt;" after an &amp; is never
    // re-scanned. Plus msga's "&#42;" for a mark meant literally (asciiRef).
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        char lit = 0;
        if (const size_t len = entityAt(s, i, &lit)) {
            out += lit;
            i += len;
        } else {
            out += s[i++];
        }
    }
    return out;
}

Rich parse(std::string_view src) {
    Rich r = parseImpl(src, 0, false);
    linkifyBareUrls(r);
    return r;
}

Rich resolveTokens(std::string_view src) {
    // Only Slack's angle-bracket tokens and :emoji:. Slack's own
    // text→rich_text conversion (and some bots, e.g. an Outlook Calendar
    // reminder with a raw "<!date^…|2:00 PM>") leaves these unexpanded in a
    // plain text element, and Slack's clients resolve them everywhere.
    Builder      b;
    const auto   inUrl = urlWordMask(src);
    const size_t n     = src.size();
    size_t       i     = 0;
    while (i < n) {
        const char c = src[i];
        if (c == '<') {
            const size_t close = src.find('>', i + 1);
            if (close != std::string_view::npos) {
                // requireScheme: '<' is not escaped in rich_text.
                appendAngleConstruct(b, src.substr(i + 1, close - i - 1), /*requireScheme=*/true);
                i = close + 1;
                continue;
            }
        }
        // Same URL guard as the link-label scanner: an unbracketed URL can
        // carry a ":b:" path segment.
        if (c == ':' && !inUrl[i]) {
            if (const size_t next = emojiAt(b, src, i)) {
                i = next;
                continue;
            }
        }
        b.text += c;
        ++i;
    }
    return b.take();
}

// ── Permalinks ──────────────────────────────────────────────────────────────

MessageRef parseMessageLink(std::string_view url) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos)
        return {};
    const std::string_view scheme = url.substr(0, schemeEnd);
    if (!str::iequals(scheme, "https") && !str::iequals(scheme, "http"))
        return {};
    std::string_view rest    = url.substr(schemeEnd + 3);
    const size_t     authEnd = rest.find_first_of("/?#");
    std::string_view auth    = rest.substr(0, authEnd);
    rest = authEnd == std::string_view::npos ? std::string_view() : rest.substr(authEnd);
    if (const size_t at = auth.rfind('@'); at != std::string_view::npos)
        auth.remove_prefix(at + 1);
    if (const size_t colon = auth.find(':'); colon != std::string_view::npos)
        auth = auth.substr(0, colon);
    const std::string host = str::asciiLower(auth);
    if (host != "slack.com" && !endsWith(host, ".slack.com"))
        return {};

    const size_t     q    = rest.find_first_of("?#");
    std::string_view path = rest.substr(0, q);
    std::string_view query =
        q != std::string_view::npos && rest[q] == '?' ? rest.substr(q + 1) : std::string_view();
    if (const size_t hash = query.find('#'); hash != std::string_view::npos)
        query = query.substr(0, hash);

    std::vector<std::string_view> seg;
    for (size_t s = 0; s <= path.size();) {
        const size_t slash = path.find('/', s);
        const auto   part  = path.substr(s, slash == std::string_view::npos ? slash : slash - s);
        if (!part.empty())
            seg.push_back(part);
        if (slash == std::string_view::npos)
            break;
        s = slash + 1;
    }
    if (seg.size() != 3 || seg[0] != "archives")
        return {};
    // Conversation ids are uppercase alphanumeric ("C6HQE8G0Z"), so a
    // lookalike path (…/archives/search/pdf) can't make a dead chip.
    const std::string_view conv = seg[1];
    if (conv.size() < 2 || !(conv[0] >= 'A' && conv[0] <= 'Z'))
        return {};
    for (char ch : conv)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
            return {};
    // "p1786008939071009" → "1786008939.071009": the fraction is the last 6 digits.
    const std::string_view tok = seg[2];
    if (tok.size() < 8 || tok[0] != 'p')
        return {};
    for (char ch : tok.substr(1))
        if (!isAsciiDigit(ch))
            return {};
    const std::string_view digits = tok.substr(1);
    MessageRef             ref;
    ref.host = host;
    ref.conv = std::string(conv);
    ref.ts =
        str::concat({digits.substr(0, digits.size() - 6), ".", digits.substr(digits.size() - 6)});
    // A reply's permalink carries its root in thread_ts; when the message *is*
    // the root it repeats its own ts — only a genuine reply gets a target.
    for (size_t s = 0; s < query.size();) {
        const size_t     amp  = query.find('&', s);
        std::string_view pair = query.substr(s, amp == std::string_view::npos ? amp : amp - s);
        if (startsWith(pair, "thread_ts=")) {
            const std::string thread = str::percentDecode(pair.substr(10));
            if (!thread.empty() && thread != ref.ts)
                ref.threadTs = thread;
            break;
        }
        if (amp == std::string_view::npos)
            break;
        s = amp + 1;
    }
    return ref;
}

namespace {
constexpr std::string_view kThreadLinkPrefix = "msga://thread/";
}

std::string threadLink(std::string_view conv, std::string_view rootTs) {
    return str::concat({kThreadLinkPrefix, conv, "/", rootTs});
}

MessageRef parseThreadLink(std::string_view url) {
    if (!startsWith(url, kThreadLinkPrefix))
        return {};
    const std::string_view rest  = url.substr(kThreadLinkPrefix.size());
    const size_t           slash = rest.find('/');
    if (slash == std::string_view::npos || rest.find('/', slash + 1) != std::string_view::npos)
        return {};
    MessageRef ref;
    ref.conv     = std::string(rest.substr(0, slash));
    ref.ts       = std::string(rest.substr(slash + 1));
    ref.threadTs = ref.ts;
    return ref;
}

std::string refToToken(const MessageRef &ref) {
    return str::concat({ref.host, "/", ref.conv, "/", ref.ts, "/", ref.threadTs, "/", ref.author});
}

std::string messagePermalink(const MessageRef &ref) {
    if (ref.host.empty() || !ref.valid())
        return {};
    std::string ts = ref.ts;
    std::erase(ts, '.');
    std::string url = str::concat({"https://", ref.host, "/archives/", ref.conv, "/p", ts});
    if (!ref.threadTs.empty())
        url += str::concat({"?thread_ts=", ref.threadTs, "&cid=", ref.conv});
    return url;
}

MessageRef refFromToken(std::string_view token) {
    std::string_view p[5];
    int              k = 0;
    size_t           s = 0;
    while (k < 5) {
        const size_t slash = token.find('/', s);
        p[k++]             = token.substr(s, slash == std::string_view::npos ? slash : slash - s);
        if (slash == std::string_view::npos)
            break;
        s = slash + 1;
        if (k == 5)
            return {}; // more than five fields
    }
    if (k != 5)
        return {};
    return MessageRef{
        std::string(p[0]),
        std::string(p[1]),
        std::string(p[2]),
        std::string(p[3]),
        std::string(p[4])
    };
}

// ── Layout view ─────────────────────────────────────────────────────────────

namespace {

// "• ", "◦ ", "▪ ", "- " or "12. " / "12) " after up to 6 spaces.
bool listMarker(std::string_view line, uint32_t *markerLen, uint16_t *ordinal, uint8_t *indent) {
    size_t sp = 0;
    while (sp < line.size() && sp < 6 && line[sp] == ' ')
        ++sp;
    std::string_view rest = line.substr(sp);
    size_t           mark = 0;
    uint16_t         ord  = 0;
    if (startsWith(rest, "\xE2\x80\xA2 ") || startsWith(rest, "\xE2\x97\xA6 ") ||
        startsWith(rest, "\xE2\x96\xAA ")) // • ◦ ▪
        mark = 4;
    else if (startsWith(rest, "- "))
        mark = 2;
    else {
        size_t d = 0;
        while (d < rest.size() && d < 3 && isAsciiDigit(rest[d]))
            ord = uint16_t(ord * 10 + (rest[d++] - '0'));
        if (d > 0 && d + 1 < rest.size() && (rest[d] == '.' || rest[d] == ')') &&
            rest[d + 1] == ' ')
            mark = d + 2;
        else
            ord = 0;
    }
    if (!mark)
        return false;
    *markerLen = uint32_t(sp + mark);
    *ordinal   = ord;
    *indent    = uint8_t(std::min<size_t>(sp / 2, 3));
    return true;
}

// Lines of [start, end) → Paragraph / ListItem blocks. Consecutive non-list
// lines share a paragraph; a list item is one line.
void splitLines(
    const std::string &text, uint32_t start, uint32_t end, bool quoted, std::vector<Block> &out
) {
    bool     inPara = false;
    uint32_t pos    = start;
    while (pos <= end) {
        uint32_t eol = uint32_t(text.find('\n', pos));
        if (eol > end || eol == uint32_t(std::string::npos))
            eol = end;
        const std::string_view line(text.data() + pos, eol - pos);
        uint32_t               markerLen = 0;
        uint16_t               ordinal   = 0;
        uint8_t                indent    = 0;
        if (listMarker(line, &markerLen, &ordinal, &indent)) {
            Block b;
            b.kind      = BlockKind::ListItem;
            b.quoted    = quoted;
            b.indent    = indent;
            b.ordinal   = ordinal;
            b.start     = pos;
            b.end       = eol;
            b.markerEnd = pos + markerLen;
            out.push_back(b);
            inPara = false;
        } else if (inPara) {
            out.back().end = eol; // extend over the line break
        } else {
            Block b;
            b.quoted = quoted;
            b.start  = pos;
            b.end    = eol;
            out.push_back(b);
            inPara = true;
        }
        if (eol >= end)
            break;
        pos = eol + 1;
    }
}

// Text between block-level elements: one newline next to a code block or a
// quote is that block's own line break, not an empty line.
void addRegion(
    const std::string  &text,
    uint32_t            start,
    uint32_t            end,
    bool                quoted,
    bool                afterBlock,
    bool                beforeBlock,
    std::vector<Block> &out
) {
    if (afterBlock && start < end && text[start] == '\n')
        ++start;
    if (beforeBlock && end > start && text[end - 1] == '\n')
        --end;
    if (end == text.size()) // trailing newlines of the message draw nothing
        while (end > start && text[end - 1] == '\n')
            --end;
    if (start >= end)
        return;
    splitLines(text, start, end, quoted, out);
}

void addCode(const Rich &r, const Entity &e, bool quoted, std::vector<Block> &out) {
    uint32_t start = e.start, end = e.end();
    // "```\ncode\n```": the fence's own line breaks are not code lines (the
    // parser keeps them).
    if (start < end && r.text[start] == '\n')
        ++start;
    while (end > start && r.text[end - 1] == '\n')
        --end;
    Block b;
    b.kind   = BlockKind::Code;
    b.quoted = quoted;
    b.start  = start;
    b.end    = end;
    out.push_back(b);
}

// Blocks of [start, end), splitting at Pre (and, at the top level, Quote) entities.
void blocksIn(const Rich &r, uint32_t start, uint32_t end, bool quoted, std::vector<Block> &out) {
    uint32_t pos        = start;
    bool     afterBlock = false;
    for (const auto &e : r.entities) {
        if (e.start < pos || e.end() > end)
            continue; // outside, or nested in a block already handled
        if (e.kind == Kind::Pre) {
            addRegion(r.text, pos, e.start, quoted, afterBlock, true, out);
            addCode(r, e, quoted, out);
        } else if (e.kind == Kind::Quote && !quoted) {
            addRegion(r.text, pos, e.start, quoted, afterBlock, true, out);
            blocksIn(r, e.start, e.end(), true, out);
        } else {
            continue;
        }
        pos        = e.end();
        afterBlock = true;
    }
    addRegion(r.text, pos, end, quoted, afterBlock, false, out);
}

bool isTarget(Kind k) {
    switch (k) {
    case Kind::Link:
    case Kind::User:
    case Kind::Channel:
    case Kind::Here:
    case Kind::ChannelCmd:
    case Kind::Emoji:
    case Kind::MessageLink:
    case Kind::Usergroup:
        return true;
    default:
        return false;
    }
}

uint16_t styleOf(Kind k) {
    switch (k) {
    case Kind::Bold:
        return StyleBold;
    case Kind::Italic:
        return StyleItalic;
    case Kind::Underline:
        return StyleUnderline;
    case Kind::Strike:
        return StyleStrike;
    case Kind::Code:
        return StyleCode;
    default:
        return 0;
    }
}

} // namespace

std::vector<Block> blocks(const Rich &r) {
    std::vector<Block> out;
    blocksIn(r, 0, uint32_t(r.text.size()), false, out);
    return out;
}

void runs(const Rich &r, uint32_t start, uint32_t end, std::vector<Run> &out) {
    if (start >= end)
        return;
    // One sweep over the entities' edges inside the range, sorted (position,
    // entity, open): the segments between edges, each styled by the entities
    // open across it — a handful at once (nesting), not all of them.
    std::vector<uint64_t> edges;
    for (size_t k = 0; k < r.entities.size(); ++k) {
        const Entity &e = r.entities[k];
        if (e.length == 0 || e.end() <= start || e.start >= end)
            continue;
        edges.push_back(uint64_t(std::max(e.start, start)) << 32 | uint64_t(k) << 1 | 1);
        if (e.end() < end)
            edges.push_back(uint64_t(e.end()) << 32 | uint64_t(k) << 1);
    }
    std::sort(edges.begin(), edges.end());
    std::vector<uint32_t> open; // entity indices
    const size_t          first = out.size();
    size_t                next  = 0;
    for (uint32_t pos = start; pos < end;) {
        for (; next < edges.size() && uint32_t(edges[next] >> 32) == pos; ++next) {
            const auto k = uint32_t(edges[next] >> 1) & 0x7FFFFFFF;
            if (edges[next] & 1)
                open.push_back(k);
            else
                std::erase(open, k);
        }
        Run run;
        run.start = pos;
        run.end   = next < edges.size() ? uint32_t(edges[next] >> 32) : end;
        pos       = run.end;
        for (const uint32_t k : open) {
            const Entity &e = r.entities[k];
            run.style |= styleOf(e.kind);
            // The deepest target: the latest one (parents come first).
            if (isTarget(e.kind) && int32_t(k) > run.entity)
                run.entity = int32_t(k);
        }
        // Merge with the previous run when nothing differs (entity edges of
        // purely structural spans such as Quote produce no visible change).
        if (out.size() > first && out.back().style == run.style &&
            out.back().entity == run.entity && out.back().end == run.start) {
            out.back().end = run.end;
            continue;
        }
        // ":wave::skin-tone-3:" arrives as two Emoji entities; draw one glyph.
        if (out.size() > first && run.entity >= 0 && out.back().entity >= 0 &&
            out.back().end == run.start && out.back().skinTone == 0) {
            const Entity &prev = r.entities[size_t(out.back().entity)];
            const Entity &cur  = r.entities[size_t(run.entity)];
            if (prev.kind == Kind::Emoji && cur.kind == Kind::Emoji && cur.data.size() == 11 &&
                startsWith(cur.data, "skin-tone-") && cur.data[10] >= '2' && cur.data[10] <= '6' &&
                !startsWith(prev.data, "skin-tone-")) {
                out.back().end      = run.end;
                out.back().skinTone = uint8_t(cur.data[10] - '0');
                continue;
            }
        }
        out.push_back(run);
    }
}

} // namespace mrkdwn
