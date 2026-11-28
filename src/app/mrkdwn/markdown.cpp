#include "app/mrkdwn/markdown.h"

#include "app/mrkdwn/mrkdwn.h"
#include "base/json.h"
#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace mrkdwn {

namespace {

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

// <…> the way Slack reads it: a mention, a command or a link.
bool isSlackToken(std::string_view inner) {
    return !inner.empty() &&
           (inner[0] == '@' || inner[0] == '#' || inner[0] == '!' || looksLikeUrl(inner));
}

size_t run(std::string_view s, size_t i, char c) {
    size_t n = 0;
    while (i + n < s.size() && s[i + n] == c)
        ++n;
    return n;
}

// Just past a code span opened by `n` backticks at `open` (the closer is a
// run of exactly that length), or npos.
size_t codeSpanEnd(std::string_view s, size_t open, size_t n) {
    size_t j = open + n;
    while (j < s.size()) {
        if (s[j] != '`') {
            ++j;
            continue;
        }
        const size_t r = run(s, j, '`');
        if (r == n)
            return j + r;
        j += r;
    }
    return std::string_view::npos;
}

// The closing delimiter run for one opened at `open`: the first that
// follows non-space text.
size_t delimiterClose(std::string_view s, size_t open, char d, size_t width) {
    for (size_t j = open + width; j + width <= s.size(); ++j) {
        if (space(s[j - 1]) || s[j - 1] == d)
            continue;
        bool all = true;
        for (size_t k = 0; k < width && all; ++k)
            all = s[j + k] == d;
        if (all)
            return j;
    }
    return std::string_view::npos;
}

// [label](url) or ![alt](url) with an optional "title" at i: its end and parts.
bool markdownLink(std::string_view s, size_t i, size_t *end, std::string *label, std::string *url) {
    size_t p = i + (s[i] == '!' ? 1 : 0);
    if (p >= s.size() || s[p] != '[')
        return false;
    const size_t lb = p + 1, le = s.find(']', lb);
    if (le == std::string_view::npos || le == lb || le + 1 >= s.size() || s[le + 1] != '(')
        return false;
    if (s.substr(lb, le - lb).find('[') != std::string_view::npos)
        return false;
    size_t u = le + 2, depth = 0, q = u;
    for (; q < s.size(); ++q) {
        const char c = s[q];
        if (c == '(')
            ++depth;
        else if (c == ')') {
            if (depth == 0)
                break;
            --depth;
        } else if (space(c) || c == '<' || c == '>')
            break;
    }
    if (q == u || depth)
        return false;
    size_t close = q;
    if (close < s.size() && space(s[close])) { // an optional "title"
        size_t t = close;
        while (t < s.size() && space(s[t]))
            ++t;
        if (t >= s.size() || s[t] != '"')
            return false;
        const size_t te = s.find('"', t + 1);
        if (te == std::string_view::npos)
            return false;
        close = te + 1;
    }
    if (close >= s.size() || s[close] != ')')
        return false;
    *label = std::string(s.substr(lb, le - lb));
    *url   = std::string(s.substr(u, q - u));
    *end   = close + 1;
    return true;
}

std::string slackLink(const std::string &url, std::string label) {
    // No escape for '|' in a URL: a pipe-bearing URL goes bare.
    if (url.find('|') != std::string::npos || str::trim(label).empty() || label == url)
        return "<" + url + ">";
    std::string l;
    for (char c : label)
        if (c == '|')
            l += '/';
        else if (c == '>')
            l += "&gt;";
        else
            l += c;
    return str::concat({"<", url, "|", l, ">"});
}

int indentCols(std::string_view line) {
    int cols = 0;
    for (char c : line) {
        if (c == ' ')
            ++cols;
        else if (c == '\t')
            cols += 4;
        else
            break;
    }
    return cols;
}

bool blank(std::string_view line) {
    return str::trim(line).empty();
}

bool fenceOpen(std::string_view line, std::string *rest) {
    const std::string_view t = str::trim(line);
    if (!str::startsWith(t, "```"))
        return false;
    *rest = std::string(t.substr(3));
    return true;
}

struct Item {
    int         indent = 0, level = 0, number = 1;
    bool        ordered = false;
    std::string text;
};

// "- a", "* a", "+ a", "1. a", "1) a": a marker, a blank, then something.
bool listItem(std::string_view line, Item *item, std::string *content) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    size_t m       = i;
    bool   ordered = false;
    if (m < line.size() && (line[m] == '-' || line[m] == '*' || line[m] == '+')) {
        ++m;
    } else {
        while (m < line.size() && m - i < 9 && line[m] >= '0' && line[m] <= '9')
            ++m;
        if (m == i || m >= line.size() || (line[m] != '.' && line[m] != ')'))
            return false;
        ordered = true;
        ++m;
    }
    if (m >= line.size() || (line[m] != ' ' && line[m] != '\t'))
        return false;
    size_t c = m;
    while (c < line.size() && (line[c] == ' ' || line[c] == '\t'))
        ++c;
    if (c >= line.size())
        return false;
    item->indent  = indentCols(line);
    item->ordered = ordered;
    item->number  = ordered ? std::atoi(std::string(line.substr(i, m - 1 - i)).c_str()) : 1;
    *content      = std::string(line.substr(c));
    return true;
}

bool quoteLine(std::string_view line, std::string *prefix, std::string *content) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ')
        ++i;
    if (i >= line.size() || line[i] != '>')
        return false;
    ++i;
    if (i < line.size() && line[i] == ' ')
        ++i;
    *prefix  = std::string(line.substr(0, i));
    *content = std::string(line.substr(i));
    return true;
}

} // namespace

std::string convertInline(std::string_view s) {
    std::string  out;
    const size_t n = s.size();
    size_t       i = 0;
    out.reserve(n);
    while (i < n) {
        const char c = s[i];
        if (c == '`') { // code span: verbatim, delimiters included
            const size_t r = run(s, i, '`');
            const size_t e = codeSpanEnd(s, i, r);
            if (e != std::string_view::npos) {
                out += s.substr(i, e - i);
                i = e;
            } else {
                out += s.substr(i, r);
                i += r;
            }
            continue;
        }
        if (c == '<') { // a Slack token: opaque
            const size_t close = s.find('>', i + 1);
            if (close != std::string_view::npos && isSlackToken(s.substr(i + 1, close - i - 1))) {
                out += s.substr(i, close + 1 - i);
                i = close + 1;
                continue;
            }
        }
        // ***x*** → *_x_*, **x** → *x*, ~~x~~ → ~x~ (flanking: the opener
        // touches text, the closer follows it).
        if ((c == '*' || c == '~') && i + 1 < n && s[i + 1] == c) {
            const size_t width = (c == '*' && i + 2 < n && s[i + 2] == '*') ? 3 : 2;
            const size_t first = i + width;
            if (first < n && !space(s[first]) && s[first] != c) {
                const size_t close = delimiterClose(s, i, c, width);
                if (close != std::string_view::npos) {
                    const std::string inner = convertInline(s.substr(first, close - first));
                    if (width == 3)
                        out += str::concat({"*_", inner, "_*"});
                    else
                        out +=
                            str::concat({std::string_view(&c, 1), inner, std::string_view(&c, 1)});
                    i = close + width;
                    continue;
                }
            }
        }
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            size_t      end;
            std::string label, url;
            if (markdownLink(s, i, &end, &label, &url) && looksLikeUrl(url)) {
                out += slackLink(url, label);
                i = end;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// ── Markdown emphasis ───────────────────────────────────────────────────────

namespace {

// Bytes of `s` no mark applies to: ``` blocks, `spans` (closed on their
// line by a run of the same length) and <…> Slack tokens.
std::vector<bool> codeMask(std::string_view s) {
    constexpr size_t  npos = std::string_view::npos;
    std::vector<bool> code(s.size(), false);
    size_t            i = 0;
    while (i < s.size()) {
        size_t end = npos, skip = 1;
        if (s[i] == '`') {
            const size_t r = run(s, i, '`');
            skip           = r;
            if (r >= 3) {
                const size_t close = s.find("```", i + r);
                if (close != npos)
                    end = close + run(s, close, '`');
            } else {
                for (size_t j = i + r; j < s.size() && s[j] != '\n';) {
                    const size_t rr = run(s, j, '`');
                    if (rr == r) {
                        end = j + rr;
                        break;
                    }
                    j += rr ? rr : 1;
                }
            }
        } else if (s[i] == '<') {
            const size_t close = s.find('>', i + 1);
            const auto inner = close == npos ? std::string_view() : s.substr(i + 1, close - i - 1);
            if (close != npos && inner.find('\n') == npos && isSlackToken(inner))
                end = close + 1;
        }
        if (end == npos) {
            i += skip;
            continue;
        }
        std::fill(code.begin() + i, code.begin() + end, true);
        i = end;
    }
    return code;
}

// The closer of a single '*' opened just before `from`: the first lone '*'
// on the line that follows text, or npos. Longer runs are passed over.
size_t loneStarClose(std::string_view s, const std::vector<bool> &code, size_t from) {
    for (size_t j = from; j < s.size() && s[j] != '\n';) {
        if (s[j] != '*' || code[j]) {
            ++j;
            continue;
        }
        const size_t r = run(s, j, '*');
        if (r == 1 && !space(s[j - 1]))
            return j;
        j += r;
    }
    return std::string_view::npos;
}

// A single '*' at `i` that touches the text after it.
bool loneStarOpens(std::string_view s, const std::vector<bool> &code, size_t i) {
    return s[i] == '*' && !code[i] && run(s, i, '*') == 1 && i + 1 < s.size() && !space(s[i + 1]);
}

bool wordAt(std::string_view s, size_t pos) {
    return pos < s.size() && utf8::isWordChar(utf8::decode(s, pos));
}

bool wordBefore(std::string_view s, size_t pos) {
    return pos > 0 && wordAt(s, utf8::prevBoundary(s, pos));
}

} // namespace

std::string markdownItalics(std::string_view s) {
    std::string             out(s);
    const std::vector<bool> code = codeMask(s);
    size_t                  i    = 0;
    while (i < s.size()) {
        if (s[i] != '*' || code[i]) {
            ++i;
            continue;
        }
        if (loneStarOpens(s, code, i) && !wordBefore(s, i)) {
            const size_t close = loneStarClose(s, code, i + 1);
            if (close != std::string_view::npos && !wordAt(s, close + 1)) {
                out[i] = out[close] = '_';
                i                   = close + 1;
                continue;
            }
        }
        i += run(s, i, '*');
    }
    return out;
}

std::string markdownBold(std::string_view s) {
    std::string             out;
    const std::vector<bool> code = codeMask(s);
    size_t                  i    = 0;
    out.reserve(s.size() + 8);
    while (i < s.size()) {
        if (s[i] != '*' || code[i]) {
            out += s[i++];
            continue;
        }
        if (loneStarOpens(s, code, i)) {
            const size_t close = loneStarClose(s, code, i + 1);
            if (close != std::string_view::npos) {
                out += str::concat({"**", s.substr(i + 1, close - i - 1), "**"});
                i = close + 1;
                continue;
            }
        }
        const size_t r = run(s, i, '*');
        out += s.substr(i, r);
        i += r;
    }
    return out;
}

// ── rich_text elements ──────────────────────────────────────────────────────

namespace {

enum StyleBit : uint8_t { SBold = 1, SItalic = 2, SStrike = 4, SCode = 8 };

// One rich_text inline element, flat: written out by writeElements.
struct Element {
    enum class Type : uint8_t { Text, Link, User, Channel, Usergroup, Emoji, Broadcast } type;
    uint8_t     style    = 0;
    uint8_t     skinTone = 0;
    std::string text, id; // id: the URL, user / channel / usergroup id, emoji name, range
};

void emitText(std::vector<Element> &out, std::string_view t, uint8_t st) {
    if (t.empty())
        return;
    if (!out.empty() && out.back().type == Element::Type::Text && out.back().style == st) {
        out.back().text += t;
        return;
    }
    out.push_back({Element::Type::Text, st, 0, std::string(t), {}});
}

void emitLink(
    std::vector<Element> &out, const std::string &url, std::string_view label, uint8_t st
) {
    if (!looksLikeUrl(url)) {
        // The parser links any <word>; Slack refuses a link element without a
        // scheme, so the text goes back as typed.
        emitText(
            out, str::concat({"<", label == url ? url : str::concat({url, "|", label}), ">"}), st
        );
        return;
    }
    out.push_back(
        {Element::Type::Link, st, 0, label == url ? std::string() : std::string(label), url}
    );
}

void emitEmoji(std::vector<Element> &out, const std::string &name) {
    // ":+1::skin-tone-3:" is two Emoji spans; the official client sends one
    // element with skin_tone, and a lone skin-tone element shows a swatch.
    if (name.size() == 11 && str::startsWith(name, "skin-tone-") && name[10] >= '2' &&
        name[10] <= '6' && !out.empty() && out.back().type == Element::Type::Emoji &&
        !out.back().skinTone) {
        out.back().skinTone = uint8_t(name[10] - '0');
        return;
    }
    out.push_back({Element::Type::Emoji, 0, 0, {}, name});
}

// [from, to) of r.text, consuming entities from k (parents before children,
// so a parent's walk owns everything up to its end).
void walk(
    const Rich &r, size_t &k, uint32_t from, uint32_t to, uint8_t st, std::vector<Element> &out
) {
    uint32_t pos = from;
    while (k < r.entities.size() && r.entities[k].start < to) {
        const Entity e = r.entities[k++];
        if (e.start < pos || e.length == 0)
            continue; // overlapping or empty: nothing to wrap
        const uint32_t end = std::min(e.end(), to);
        emitText(out, std::string_view(r.text).substr(pos, e.start - pos), st);
        pos                         = end;
        const std::string_view span = std::string_view(r.text).substr(e.start, end - e.start);
        auto                   skip = [&] {
            while (k < r.entities.size() && r.entities[k].start < end)
                ++k;
        };
        switch (e.kind) {
        case Kind::Bold:
            walk(r, k, e.start, end, st | SBold, out);
            break;
        case Kind::Italic:
            walk(r, k, e.start, end, st | SItalic, out);
            break;
        case Kind::Strike:
            walk(r, k, e.start, end, st | SStrike, out);
            break;
        case Kind::Code:
        case Kind::Pre: // a mid-line ``` run can only be inline code here
            walk(r, k, e.start, end, st | SCode, out);
            break;
        case Kind::Underline: // Slack has no underline style
        case Kind::Quote:
            walk(r, k, e.start, end, st, out);
            break;
        case Kind::Link:
            // Inside `code` a URL is only linked for display: it goes as typed.
            if (st & SCode)
                emitText(out, span, st);
            else
                emitLink(out, e.data, span, st);
            skip();
            break;
        case Kind::MessageLink:
            emitLink(out, std::string(span), span, st); // the span IS the permalink
            skip();
            break;
        case Kind::User:
        case Kind::Channel:
        case Kind::Usergroup:
            // Mentions take bold/italic/strike only: a code flag is invalid_blocks.
            out.push_back(
                {e.kind == Kind::User      ? Element::Type::User
                 : e.kind == Kind::Channel ? Element::Type::Channel
                                           : Element::Type::Usergroup,
                 uint8_t(st & ~SCode),
                 0,
                 {},
                 e.data}
            );
            skip();
            break;
        case Kind::Emoji:
            emitEmoji(out, e.data);
            skip();
            break;
        case Kind::Here:
            // <!here> has no data; <!everyone> and unknown commands carry it.
            if (e.data.empty() || e.data == "here" || e.data == "channel" || e.data == "everyone")
                out.push_back(
                    {Element::Type::Broadcast, 0, 0, {}, e.data.empty() ? "here" : e.data}
                );
            else
                emitText(out, span, st); // an unknown command shows as its label
            skip();
            break;
        case Kind::ChannelCmd:
            out.push_back({Element::Type::Broadcast, 0, 0, {}, "channel"});
            skip();
            break;
        }
    }
    emitText(out, std::string_view(r.text).substr(pos, to - pos), st);
}

void writeStyle(json::Writer &w, uint8_t st) {
    if (!st)
        return;
    w.key("style").beginObject();
    static constexpr struct {
        uint8_t     bit;
        const char *name;
    } kStyles[] = {{SBold, "bold"}, {SItalic, "italic"}, {SStrike, "strike"}, {SCode, "code"}};
    for (const auto &s : kStyles)
        if (st & s.bit)
            w.key(s.name).value(true);
    w.endObject();
}

void writeElements(json::Writer &w, const std::vector<Element> &els) {
    static const char *const kTypes[] = {
        "text", "link", "user", "channel", "usergroup", "emoji", "broadcast"
    };
    static const char *const kIdKeys[] = {
        nullptr, "url", "user_id", "channel_id", "usergroup_id", "name", "range"
    };
    w.key("elements").beginArray();
    for (const Element &e : els) {
        const int t = int(e.type);
        w.beginObject().key("type").value(kTypes[t]);
        if (e.type == Element::Type::Text || (e.type == Element::Type::Link && !e.text.empty()))
            w.key("text").value(e.text);
        if (kIdKeys[t])
            w.key(kIdKeys[t]).value(e.id);
        if (e.skinTone)
            w.key("skin_tone").value(int(e.skinTone));
        writeStyle(w, e.style);
        w.endObject();
    }
    w.endArray();
}

// A rich_text_section / rich_text_quote of one chunk of converted mrkdwn.
void writeSection(json::Writer &w, const char *type, const std::string &mrkdwn) {
    const Rich           r = parse(mrkdwn);
    std::vector<Element> els;
    size_t               k = 0;
    walk(r, k, 0, uint32_t(r.text.size()), 0, els);
    if (els.empty()) // never an empty section: Slack refuses it
        els.push_back({Element::Type::Text, 0, 0, mrkdwn.empty() ? " " : mrkdwn, {}});
    w.beginObject().key("type").value(type);
    writeElements(w, els);
    w.endObject();
}

enum class ChunkKind : uint8_t { Section, Quote, Pre, List };

struct Chunk {
    ChunkKind         kind;
    std::string       text; // Section/Quote: converted mrkdwn; Pre: raw code
    std::vector<Item> items;
};

// One rich_text_list per run of items sharing level and style; a nested
// list is a sibling with a deeper indent, the way Slack shapes it.
void writeList(json::Writer &w, const std::vector<Item> &items) {
    size_t i = 0;
    while (i < items.size()) {
        const Item &head = items[i];
        w.beginObject().key("type").value("rich_text_list");
        w.key("style").value(head.ordered ? "ordered" : "bullet");
        w.key("elements").beginArray();
        size_t j = i;
        for (; j < items.size() && items[j].level == head.level && items[j].ordered == head.ordered;
             ++j)
            writeSection(w, "rich_text_section", items[j].text);
        w.endArray();
        if (head.level > 0)
            w.key("indent").value(head.level);
        if (head.ordered && head.number > 1)
            w.key("offset").value(head.number - 1);
        w.endObject();
        i = j;
    }
}

} // namespace

std::string richTextElements(std::string_view mrkdwnText) {
    const Rich           r = parse(mrkdwnText);
    std::vector<Element> els;
    size_t               k = 0;
    walk(r, k, 0, uint32_t(r.text.size()), 0, els);
    json::Writer w;
    w.beginObject();
    writeElements(w, els);
    w.endObject();
    return w.take();
}

Composed compose(std::string_view composer, bool alwaysBlocks) {
    std::string text;
    for (size_t i = 0; i < composer.size(); ++i) {
        if (composer[i] == '\r') {
            text += '\n';
            if (i + 1 < composer.size() && composer[i + 1] == '\n')
                ++i;
        } else
            text += composer[i];
    }
    std::vector<std::string> lines;
    for (size_t a = 0;;) {
        const size_t b = text.find('\n', a);
        lines.push_back(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos)
            break;
        a = b + 1;
    }
    std::vector<std::string> out;
    std::vector<Chunk>       chunks;
    bool                     sectionOpen   = false; // the last chunk is a Section taking lines
    int                      pendingBlanks = 0;     // blank lines since its last line
    bool                     anyList       = false;
    const auto               closeSection  = [&] {
        sectionOpen   = false;
        pendingBlanks = 0;
    };
    size_t i = 0;
    while (i < lines.size()) {
        const std::string line = lines[i];
        // ── Fenced code ──
        if (std::string rest; fenceOpen(line, &rest)) {
            const size_t same = rest.find("```");
            if (same != std::string::npos && same > 0 && same == rest.size() - 3) {
                closeSection();
                out.push_back(line); // mrkdwn one-liner "```code```"
                chunks.push_back({ChunkKind::Pre, rest.substr(0, same), {}});
                ++i;
                continue;
            }
            if (same == std::string::npos) {
                size_t j = i + 1;
                while (j < lines.size() && lines[j].find("```") == std::string::npos)
                    ++j;
                if (j < lines.size()) {
                    std::vector<std::string> code;
                    const std::string        info(str::trim(rest));
                    if (!info.empty() && !isLanguageHint(info))
                        code.push_back(rest);
                    for (size_t k = i + 1; k < j; ++k)
                        code.push_back(lines[k]);
                    const size_t      at     = lines[j].find("```");
                    const std::string before = lines[j].substr(0, at);
                    const std::string after  = lines[j].substr(at + 3);
                    if (!blank(before))
                        code.push_back(before); // "last line```"
                    closeSection();
                    out.push_back("```");
                    out.insert(out.end(), code.begin(), code.end());
                    out.push_back("```");
                    std::string joined;
                    for (size_t k = 0; k < code.size(); ++k)
                        joined += (k ? "\n" : "") + code[k];
                    chunks.push_back({ChunkKind::Pre, std::move(joined), {}});
                    i = j;
                    if (blank(after))
                        ++i;
                    else
                        lines[j] = std::string(str::trim(after)); // its own line
                    continue;
                }
            }
        }
        // ── List ──
        Item        item;
        std::string content;
        if (listItem(line, &item, &content)) {
            std::vector<Item> items;
            for (;;) {
                item.text = convertInline(content);
                items.push_back(item);
                ++i;
                // Continuation lines: indented, not a marker, not a fence.
                while (i < lines.size() && !blank(lines[i]) && indentCols(lines[i]) >= 2 &&
                       !listItem(lines[i], &item, &content)) {
                    std::string rest;
                    if (fenceOpen(lines[i], &rest))
                        break;
                    items.back().text += "\n" + convertInline(str::trim(lines[i]));
                    ++i;
                }
                size_t k = i; // a blank line ends the list unless an item follows
                while (k < lines.size() && blank(lines[k]))
                    ++k;
                if (k < lines.size() && listItem(lines[k], &item, &content)) {
                    i = k;
                    continue;
                }
                break;
            }
            // Nesting level = rank of the indent among this list's indents.
            std::vector<int> indents;
            for (const Item &it : items)
                indents.push_back(it.indent);
            std::sort(indents.begin(), indents.end());
            indents.erase(std::unique(indents.begin(), indents.end()), indents.end());
            for (Item &it : items)
                it.level = std::min<int>(
                    int(std::lower_bound(indents.begin(), indents.end(), it.indent) -
                        indents.begin()),
                    8
                );
            // The fallback text, like the official client's: "• a" / "1. a",
            // numbered on from the first item of each run.
            int  number = 0, prevLevel = -1;
            bool prevOrdered = false;
            for (Item &it : items) {
                if (it.level != prevLevel || it.ordered != prevOrdered)
                    number = it.number;
                else
                    ++number;
                it.number   = number;
                prevLevel   = it.level;
                prevOrdered = it.ordered;
                out.push_back(
                    std::string(size_t(it.level) * 4, ' ') +
                    (it.ordered ? str::concat({str::number(int64_t(number)), ". "})
                                : std::string("\xE2\x80\xA2 ")) +
                    it.text
                );
            }
            closeSection();
            chunks.push_back({ChunkKind::List, {}, std::move(items)});
            anyList = true;
            continue;
        }
        // ── Blockquote ──
        if (std::string prefix, rest; quoteLine(line, &prefix, &rest)) {
            std::string quoted;
            while (i < lines.size() && quoteLine(lines[i], &prefix, &rest)) {
                const std::string conv = convertInline(rest);
                out.push_back(prefix + conv);
                quoted += (quoted.empty() ? "" : "\n") + conv;
                ++i;
            }
            closeSection();
            chunks.push_back({ChunkKind::Quote, std::move(quoted), {}});
            continue;
        }
        if (blank(line)) {
            out.push_back(line);
            ++pendingBlanks;
            ++i;
            continue;
        }
        const std::string conv = convertInline(line);
        out.push_back(conv);
        if (sectionOpen) {
            chunks.back().text += std::string(size_t(pendingBlanks) + 1, '\n') + conv;
        } else {
            chunks.push_back({ChunkKind::Section, conv, {}});
            sectionOpen = true;
        }
        pendingBlanks = 0;
        ++i;
    }
    Composed c;
    for (size_t k = 0; k < out.size(); ++k) {
        if (k)
            c.mrkdwn += '\n';
        c.mrkdwn += out[k];
    }
    if (!anyList && !alwaysBlocks)
        return c;
    // Exactly one rich_text block mirroring the whole message (Slack renders
    // blocks instead of the text, never both).
    json::Writer w;
    w.beginArray().beginObject().key("type").value("rich_text").key("elements").beginArray();
    for (const Chunk &ch : chunks) {
        switch (ch.kind) {
        case ChunkKind::Section:
            writeSection(w, "rich_text_section", ch.text);
            break;
        case ChunkKind::Quote:
            writeSection(w, "rich_text_quote", ch.text);
            break;
        case ChunkKind::Pre:
            w.beginObject().key("type").value("rich_text_preformatted");
            w.key("elements").beginArray().beginObject().key("type").value("text");
            w.key("text").value(decodeEntities(ch.text)).endObject().endArray().endObject();
            break;
        case ChunkKind::List:
            writeList(w, ch.items);
            break;
        }
    }
    w.endArray().endObject().endArray();
    c.blocks = w.take();
    return c;
}

std::string convertOutgoing(std::string_view composer) {
    return compose(composer).mrkdwn;
}

} // namespace mrkdwn
