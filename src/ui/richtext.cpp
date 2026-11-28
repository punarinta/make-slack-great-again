// text/html for the clipboard: a writer for our few formats and a forgiving
// reader for what browsers, Slack, Google Docs and office suites put there.
// Not an HTML parser — a tag scanner that understands inline formatting,
// links, line breaks and block boundaries, and ignores everything else.
#include "ui/textedit.h"

#include "base/str.h"

#include <algorithm>
#include <cstring>

namespace ui::rich {

namespace {

constexpr uint16_t kBold = TextEdit::Bold, kItalic = TextEdit::Italic;
constexpr uint16_t kStrike = TextEdit::Strike, kCode = TextEdit::Code;

// Text as HTML: a line break is <br> in text and a space in an attribute.
void escape(std::string &out, std::string_view s, bool attr) {
    for (size_t at = 0;;) {
        const size_t nl = s.find('\n', at);
        str::appendEscapedHtml(&out, s.substr(at, nl - at), attr);
        if (nl == std::string_view::npos)
            return;
        out += attr ? " " : "<br>";
        at = nl + 1;
    }
}

// Decodes entities in s (text or an attribute value); &nbsp; is a plain space.
std::string decode(std::string_view s) {
    return str::decodeEntities(s, true);
}

// Value of attribute `name` inside a tag's attribute text, decoded.
std::string attr(std::string_view tag, const char *name) {
    return decode(tagAttr(tag, name));
}

bool isBlock(std::string_view n) {
    static const char *blocks[] = {
        "p",
        "div",
        "li",
        "tr",
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "ul",
        "ol",
        "blockquote",
        "table",
        "pre"
    };
    for (const char *b : blocks)
        if (str::iequals(n, b))
            return true;
    return false;
}

} // namespace

size_t readTag(std::string_view h, size_t at, Tag *tag) {
    *tag = {};
    if (h.substr(at, 4) == "<!--") {
        const size_t e = h.find("-->", at + 4);
        return e == std::string_view::npos ? h.size() : e + 3;
    }
    const size_t e = h.find('>', at);
    if (e == std::string_view::npos)
        return e;
    std::string_view t = h.substr(at + 1, e - at - 1);
    if (t.empty() || t[0] == '!' || t[0] == '?')
        return e + 1;
    tag->closing = t[0] == '/';
    if (tag->closing)
        t.remove_prefix(1);
    size_t ne = 0;
    while (ne < t.size() && t[ne] != ' ' && t[ne] != '/' && t[ne] != '\t' && t[ne] != '\n')
        ++ne;
    tag->name = t.substr(0, ne);
    tag->text = t;
    return e + 1;
}

bool skippedTag(const Tag &tag, int *depth, bool documentParts) {
    const std::string_view n = tag.name;
    if (str::iequals(n, "script") || str::iequals(n, "style") ||
        (documentParts && (str::iequals(n, "head") || str::iequals(n, "title")))) {
        *depth = std::max(*depth + (tag.closing ? -1 : 1), 0);
        return true;
    }
    return tag.text.empty() || *depth > 0;
}

std::string_view tagAttr(std::string_view tag, std::string_view name, bool strict) {
    const size_t n = name.size();
    for (size_t i = 0; i + n < tag.size(); ++i) {
        if ((i > 0 && tag[i - 1] != ' ' && tag[i - 1] != '\t' && tag[i - 1] != '\n') ||
            !(strict ? tag.substr(i, n) == name : str::iequals(tag.substr(i, n), name)))
            continue;
        size_t k = i + n;
        while (!strict && k < tag.size() && tag[k] == ' ')
            ++k;
        if (k >= tag.size() || tag[k] != '=')
            continue;
        ++k;
        while (!strict && k < tag.size() && tag[k] == ' ')
            ++k;
        if (k >= tag.size())
            return {};
        const char q = tag[k];
        if (q == '"' || q == '\'') {
            const size_t e = tag.find(q, k + 1);
            if (e == std::string_view::npos)
                return strict ? std::string_view() : tag.substr(k + 1);
            return tag.substr(k + 1, e - k - 1);
        }
        if (strict)
            continue;
        size_t e = k;
        while (e < tag.size() && tag[e] != ' ' && tag[e] != '>')
            ++e;
        return tag.substr(k, e - k);
    }
    return {};
}

std::string
toHtml(std::string_view text, const uint16_t *fmt, const std::vector<std::string> &links) {
    std::string out;
    size_t      i = 0;
    while (i < text.size()) {
        size_t j = i + 1;
        while (j < text.size() && fmt[j] == fmt[i])
            ++j;
        const uint16_t f    = fmt[i];
        const size_t   link = f >> 8;
        if (link && link <= links.size()) {
            out += "<a href=\"";
            escape(out, links[link - 1], true);
            out += "\">";
        }
        if (f & kBold)
            out += "<b>";
        if (f & kItalic)
            out += "<i>";
        if (f & kStrike)
            out += "<s>";
        if (f & kCode)
            out += "<code>";
        escape(out, text.substr(i, j - i), false);
        if (f & kCode)
            out += "</code>";
        if (f & kStrike)
            out += "</s>";
        if (f & kItalic)
            out += "</i>";
        if (f & kBold)
            out += "</b>";
        if (link && link <= links.size())
            out += "</a>";
        i = j;
    }
    return out;
}

void fromHtml(
    std::string_view          h,
    std::string              *text,
    std::vector<uint16_t>    *fmt,
    std::vector<std::string> *links
) {
    struct Open {
        std::string_view name;
        uint16_t         formats; // what this element turned on
        uint16_t         link;    // link index it set (0 = none)
        bool             pre;
    };
    std::vector<Open> stack;
    text->clear();
    fmt->clear();
    int  skip         = 0; // inside <script>/<style>/<head>/<title>
    bool pendingSpace = false;
    // Only the StartFragment…EndFragment part of Windows' CF_HTML-style payloads.
    if (size_t s = h.find("<!--StartFragment-->"); s != std::string_view::npos) {
        const size_t e = h.find("<!--EndFragment-->", s);
        h = h.substr(s + 20, e == std::string_view::npos ? std::string_view::npos : e - s - 20);
    }
    auto current = [&]() {
        uint16_t f = 0, link = 0;
        for (const Open &o : stack) {
            f |= o.formats;
            if (o.link)
                link = o.link;
        }
        return uint16_t(f | (link << 8));
    };
    auto inPre = [&]() {
        for (const Open &o : stack)
            if (o.pre)
                return true;
        return false;
    };
    auto emit = [&](std::string_view s, uint16_t f) {
        text->append(s);
        fmt->insert(fmt->end(), s.size(), f);
    };
    auto newline = [&]() {
        pendingSpace = false;
        if (!text->empty() && text->back() != '\n')
            emit("\n", 0);
    };
    size_t i = 0;
    while (i < h.size()) {
        if (h[i] == '<') {
            Tag          t;
            const size_t e = readTag(h, i, &t);
            if (e == std::string_view::npos)
                break;
            i = e;
            if (skippedTag(t, &skip, true))
                continue;
            const std::string_view tag = t.text, name = t.name;
            const bool             closing = t.closing;
            if (str::iequals(name, "br")) {
                emit("\n", 0);
                pendingSpace = false;
                continue;
            }
            if (isBlock(name))
                newline();
            if (closing) {
                for (size_t k = stack.size(); k-- > 0;)
                    if (str::iequals(stack[k].name, name)) {
                        stack.resize(k);
                        break;
                    }
                continue;
            }
            if (!tag.empty() && tag.back() == '/')
                continue; // self-closing: no content to format
            Open o{name, 0, 0, false};
            if (str::iequals(name, "b") || str::iequals(name, "strong"))
                o.formats |= kBold;
            else if (str::iequals(name, "i") || str::iequals(name, "em"))
                o.formats |= kItalic;
            else if (
                str::iequals(name, "s") || str::iequals(name, "strike") || str::iequals(name, "del")
            )
                o.formats |= kStrike;
            else if (
                str::iequals(name, "code") || str::iequals(name, "tt") ||
                str::iequals(name, "kbd") || str::iequals(name, "samp")
            )
                o.formats |= kCode;
            else if (str::iequals(name, "pre")) {
                o.formats |= kCode;
                o.pre = true;
            } else if (str::iequals(name, "a")) {
                std::string href = attr(tag, "href");
                if (!href.empty() && links->size() < 255) {
                    links->push_back(std::move(href));
                    o.link = uint16_t(links->size());
                }
            }
            // Inline styles (Google Docs wraps everything in <b style="font-weight:normal">).
            const std::string st = attr(tag, "style");
            if (!st.empty()) {
                if (str::icontains(st, "font-weight:normal") ||
                    str::icontains(st, "font-weight: normal") ||
                    str::icontains(st, "font-weight:400"))
                    o.formats &= uint16_t(~kBold);
                else if (
                    str::icontains(st, "font-weight:bold") ||
                    str::icontains(st, "font-weight: bold") ||
                    str::icontains(st, "font-weight:700") || str::icontains(st, "font-weight:600")
                )
                    o.formats |= kBold;
                if (str::icontains(st, "italic"))
                    o.formats |= kItalic;
                if (str::icontains(st, "line-through"))
                    o.formats |= kStrike;
            }
            stack.push_back(o);
            continue;
        }
        // Text up to the next tag.
        size_t e = h.find('<', i);
        if (e == std::string_view::npos)
            e = h.size();
        if (!skip) {
            const std::string t   = decode(h.substr(i, e - i));
            const uint16_t    f   = current();
            const bool        pre = inPre();
            std::string       run;
            for (char c : t) {
                const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
                if (pre) {
                    if (c != '\r')
                        run.push_back(c);
                } else if (ws) {
                    pendingSpace = true;
                } else {
                    if (pendingSpace && !run.empty()) {
                        run.push_back(' ');
                    } else if (pendingSpace && !text->empty() && text->back() != '\n') {
                        // A space collapsed across a tag takes the format of
                        // the text before it ("one <b>two</b>": the space is plain).
                        emit(" ", fmt->back());
                    }
                    pendingSpace = false;
                    run.push_back(c);
                }
            }
            emit(run, f);
        }
        i = e;
    }
    while (!text->empty() && (text->back() == '\n' || text->back() == ' ')) {
        text->pop_back();
        fmt->pop_back();
    }
}

} // namespace ui::rich
