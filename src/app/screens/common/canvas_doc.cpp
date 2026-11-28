#include "app/screens/common/canvas_doc.h"

#include "base/str.h"

#include <algorithm>
#include <cstdlib>

namespace screens::canvas {

namespace {

// An attribute's raw value in an opening tag ("" if absent): exactly
// name="…", as canvases write them.
std::string_view attr(std::string_view tag, std::string_view name) {
    return ui::rich::tagAttr(tag, name, true);
}

// Builds the editor's HTML: one <p> per line, its markdown prefix first.
struct Lines {
    std::string out, line;
    bool        hasText = false, space = false;

    void start(std::string_view prefix) {
        end();
        line = prefix;
    }
    void end() {
        if (hasText)
            out += str::concat({"<p>", line, "</p>"});
        line.clear();
        hasText = space = false;
    }
    void text(std::string_view t) {
        for (char c : t) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                space = hasText;
                continue;
            }
            if (space)
                line += ' ';
            space   = false;
            hasText = true;
            line += c;
        }
    }
    void markup(std::string_view tag) { line += tag; } // inline tags: no text of their own
};

} // namespace

std::string
editorHtml(std::string_view h, const std::vector<std::string> &titles, std::string *title) {
    if (title)
        title->clear();
    // The title: a leading <h1> naming the file.
    {
        size_t at = 0;
        while (at < h.size()) {
            at = h.find_first_not_of(" \t\r\n", at);
            if (at == std::string_view::npos || h.compare(at, 4, "<div") != 0)
                break;
            at = h.find('>', at) + 1; // the quip-canvas-content wrapper
        }
        if (at != std::string_view::npos && h.compare(at, 3, "<h1") == 0) {
            const size_t open  = h.find('>', at);
            const size_t close = open == std::string_view::npos ? open : h.find("</h1>", open);
            if (close != std::string_view::npos) {
                std::string              text;
                std::vector<uint16_t>    fmt;
                std::vector<std::string> links;
                ui::rich::fromHtml(h.substr(open + 1, close - open - 1), &text, &fmt, &links);
                const std::string t(str::trim(text));
                for (const std::string &cand : titles)
                    if (!t.empty() && std::string_view(str::trim(cand)) == t) {
                        if (title)
                            *title = t;
                        h = h.substr(close + 5);
                        break;
                    }
            }
        }
    }

    Lines lines;
    struct List {
        bool    ordered = false, checklist = false;
        int32_t n = 0;
    };
    std::vector<List> lists;
    int               quote = 0, pre = 0, rowCells = -1, skip = 0, sectionStyle = 0;
    bool              firstRow = true, br = false, code = false;
    auto              prefix = [&](int heading) {
        std::string p;
        for (int i = 0; i < quote; ++i)
            p += "&gt; ";
        if (heading)
            return p + std::string(size_t(heading), '#') + ' ';
        return p;
    };
    for (size_t i = 0; i < h.size();) {
        if (h[i] != '<') {
            const size_t e = std::min(h.find('<', i), h.size());
            if (!skip) {
                if (br && rowCells < 0) {
                    lines.start(prefix(0));
                    br = false;
                }
                if (pre) {
                    std::string_view t = h.substr(i, e - i);
                    for (size_t nl; (nl = t.find('\n')) != std::string_view::npos;) {
                        lines.line += t.substr(0, nl);
                        lines.hasText = true;
                        lines.start("");
                        t.remove_prefix(nl + 1);
                    }
                    lines.line += t;
                    lines.hasText = lines.hasText || !t.empty();
                } else {
                    lines.text(h.substr(i, e - i));
                }
            }
            i = e;
            continue;
        }
        ui::rich::Tag t;
        const size_t  e = ui::rich::readTag(h, i, &t);
        if (e == std::string_view::npos)
            break;
        i = e;
        if (ui::rich::skippedTag(t, &skip, false))
            continue;
        const std::string_view tag = t.text, name = t.name;
        const bool             closing = t.closing;
        if (str::iequals(name, "br")) {
            br = lines.hasText; // a break inside a block: the rest on its own line
            continue;
        }
        if (name.size() == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '1' &&
            name[1] <= '6') {
            if (closing)
                lines.end();
            else
                lines.start(prefix(std::min(name[1] - '0', 3)));
            br = false;
            continue;
        }
        if (str::iequals(name, "ul") || str::iequals(name, "ol")) {
            if (closing) {
                if (!lists.empty())
                    lists.pop_back();
                lines.end();
            } else {
                lines.end();
                // Quip's lists are all <ul>s; the wrapping div's
                // data-section-style says which kind (5 bullets, 6
                // numbers, 7 checklist).
                List l;
                l.ordered   = str::iequals(name, "ol") || sectionStyle == 6;
                l.checklist = sectionStyle == 7 ||
                              attr(tag, "class").find("checklist") != std::string_view::npos;
                lists.push_back(l);
                sectionStyle = 0;
            }
            continue;
        }
        if (str::iequals(name, "li")) {
            if (closing) {
                lines.end();
            } else {
                std::string p = prefix(0);
                if (!lists.empty()) {
                    List &l = lists.back();
                    p.append((lists.size() - 1) * 2, ' ');
                    if (l.checklist)
                        p += attr(tag, "class").find("checked") != std::string_view::npos
                                 ? "- [x] "
                                 : "- [ ] ";
                    else if (l.ordered)
                        (p += str::number(++l.n)) += ". ";
                    else
                        p += "- ";
                } else {
                    p += "- ";
                }
                lines.start(p);
            }
            br = false;
            continue;
        }
        if (str::iequals(name, "blockquote")) {
            lines.end();
            quote = std::max(0, quote + (closing ? -1 : 1));
            continue;
        }
        if (str::iequals(name, "pre")) { // a fenced block, a line per line
            lines.start("```");
            lines.hasText = true;
            lines.end();
            pre = std::max(0, pre + (closing ? -1 : 1));
            continue;
        }
        if (str::iequals(name, "table")) {
            lines.end();
            firstRow = true;
            continue;
        }
        if (str::iequals(name, "tr")) {
            if (closing) {
                lines.markup(" |");
                lines.hasText   = true;
                const int cells = rowCells;
                lines.end();
                if (firstRow && cells > 0) { // markdown's header rule
                    std::string rule = "|";
                    for (int c = 0; c < cells; ++c)
                        rule += " --- |";
                    lines.out += str::concat({"<p>", rule, "</p>"});
                }
                firstRow = false;
                rowCells = -1;
            } else {
                lines.start("|");
                rowCells = 0;
            }
            continue;
        }
        if (str::iequals(name, "td") || str::iequals(name, "th")) {
            if (!closing && rowCells >= 0) {
                lines.markup(rowCells ? " | " : " ");
                lines.space = false;
                ++rowCells;
            }
            continue;
        }
        if (str::iequals(name, "div")) {
            if (!closing)
                sectionStyle = std::atoi(std::string(attr(tag, "data-section-style")).c_str());
            continue;
        }
        if (str::iequals(name, "p")) {
            if (rowCells >= 0 || !lists.empty())
                continue; // a cell's or an item's paragraph stays on its line
            // A code block: one <p class="prettyprint">, lines split by <br>.
            if (!closing && attr(tag, "class").find("prettyprint") != std::string_view::npos) {
                lines.start("```");
                lines.hasText = true;
                code          = true;
            }
            if (closing) {
                lines.end();
                if (code) {
                    lines.start("```");
                    lines.hasText = true;
                    lines.end();
                }
                code = false;
            } else {
                lines.start(prefix(0));
            }
            br = false;
            continue;
        }
        // Inline formats pass through for insertHtml; links (Slack's <lnk>
        // too) keep their target.
        static const struct {
            std::string_view from, to;
        } kInline[] = {
            {"b", "b"},
            {"strong", "b"},
            {"i", "i"},
            {"em", "i"},
            {"s", "s"},
            {"del", "s"},
            {"strike", "s"},
            {"code", "code"},
        };
        bool known = false;
        for (const auto &k : kInline)
            if (str::iequals(name, k.from)) {
                lines.markup(str::concat({closing ? "</" : "<", k.to, ">"}));
                known = true;
                break;
            }
        if (!known && (str::iequals(name, "a") || str::iequals(name, "lnk"))) {
            if (closing)
                lines.markup("</a>");
            else
                lines.markup(str::concat({"<a href=\"", attr(tag, "href"), "\">"}));
        }
    }
    lines.end();
    return lines.out;
}

std::string markdown(const std::string &t, const std::vector<ui::TextEdit::Run> &runs) {
    // Lines of the block kinds that stay together without a blank line.
    auto grouped = [](std::string_view l) {
        l = str::trim(l);
        if (l.empty())
            return false;
        if (str::startsWith(l, "- ") || str::startsWith(l, "* ") || str::startsWith(l, "> ") ||
            l[0] == '|' || str::startsWith(l, "```"))
            return true;
        size_t d = 0;
        while (d < l.size() && l[d] >= '0' && l[d] <= '9')
            ++d;
        return d > 0 && str::startsWith(l.substr(d), ". ");
    };
    std::string out, prev;
    bool        inFence = false;
    size_t      first   = 0; // runs are in order: the first one not before this line
    for (size_t at = 0; at <= t.size();) {
        const size_t           nl = std::min(t.find('\n', at), t.size());
        const std::string_view raw(t.data() + at, nl - at);
        std::string            line;
        while (first < runs.size() && runs[first].end <= at)
            ++first;
        for (size_t k = first; k < runs.size() && runs[k].start < nl; ++k) {
            const ui::TextEdit::Run &r = runs[k];
            const uint32_t           a = std::max<uint32_t>(r.start, uint32_t(at));
            const uint32_t           b = std::min<uint32_t>(r.end, uint32_t(nl));
            if (a >= b)
                continue;
            std::string_view piece(t.data() + a, b - a);
            if ((!r.format && r.link.empty()) || inFence) {
                line += piece;
                continue;
            }
            const size_t lead  = piece.find_first_not_of(' ');
            const size_t trail = piece.find_last_not_of(' ');
            if (lead == std::string_view::npos) {
                line += piece;
                continue;
            }
            const std::string_view core = piece.substr(lead, trail - lead + 1);
            std::string            open, close;
            if (!r.link.empty())
                open += '[';
            if (r.format & ui::TextEdit::Bold)
                open += "**", close.insert(0, "**");
            if (r.format & ui::TextEdit::Italic)
                open += '_', close.insert(0, "_");
            if (r.format & ui::TextEdit::Strike)
                open += "~~", close.insert(0, "~~");
            if (r.format & ui::TextEdit::Code)
                open += '`', close.insert(0, "`");
            if (!r.link.empty())
                close += str::concat({"](", r.link, ")"});
            line += piece.substr(0, lead);
            line += open;
            line += core;
            line += close;
            line += piece.substr(trail + 1);
        }
        at = nl + 1;
        if (str::startsWith(str::trim(raw), "```"))
            inFence = !inFence;
        if (str::trim(line).empty()) {
            if (inFence)
                out += '\n';
            continue;
        }
        if (!out.empty())
            out += grouped(prev) && grouped(line) ? "\n" : "\n\n";
        out += line;
        prev = line;
    }
    return out;
}

std::string markdown(
    const std::string &t, const std::vector<uint16_t> &fmt, const std::vector<std::string> &links
) {
    std::vector<ui::TextEdit::Run> runs;
    const uint32_t                 n = uint32_t(std::min(t.size(), fmt.size()));
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i + 1;
        while (j < n && fmt[j] == fmt[i])
            ++j;
        ui::TextEdit::Run r;
        r.start           = i;
        r.end             = j;
        r.format          = uint8_t(fmt[i] & 0xff);
        const size_t link = fmt[i] >> 8;
        if (link && link <= links.size())
            r.link = links[link - 1];
        runs.push_back(r);
        i = j;
    }
    if (n < t.size()) {
        ui::TextEdit::Run r;
        r.start = n;
        r.end   = uint32_t(t.size());
        runs.push_back(r);
    }
    return markdown(t, runs);
}

// ── Section diff ────────────────────────────────────────────────────────────

namespace {

using Kind = Chunk::Kind;

bool isListLine(std::string_view l) {
    l = str::trim(l);
    if (str::startsWith(l, "- ") || str::startsWith(l, "* "))
        return true;
    size_t d = 0;
    while (d < l.size() && l[d] >= '0' && l[d] <= '9')
        ++d;
    return d > 0 && str::startsWith(l.substr(d), ". ");
}

// The text from the start of `html` with the leading title h1 taken out
// (editorHtml's rule), and the quip wrapper dropped.
std::string_view bodyOf(std::string_view h, const std::vector<std::string> &titles) {
    size_t at = 0;
    while (at < h.size()) {
        at = h.find_first_not_of(" \t\r\n", at);
        if (at == std::string_view::npos || h.compare(at, 4, "<div") != 0 ||
            attr(h.substr(at, h.find('>', at) - at), "class").find("quip-canvas-content") ==
                std::string_view::npos)
            break;
        at         = h.find('>', at) + 1;
        // ... and its closing tag at the very end.
        size_t end = h.find_last_not_of(" \t\r\n");
        if (end != std::string_view::npos && end >= 5 && h.compare(end - 5, 6, "</div>") == 0)
            h = h.substr(0, end - 5);
    }
    if (at == std::string_view::npos)
        return {};
    h = h.substr(at);
    std::string title;
    editorHtml(
        h.substr(
            0,
            std::min(
                h.size(),
                h.find("</h1>") == std::string_view::npos ? size_t(0) : h.find("</h1>") + 5
            )
        ),
        titles,
        &title
    );
    if (!title.empty())
        h = h.substr(h.find("</h1>") + 5);
    return h;
}

// End (exclusive) of the element whose opening tag starts at `start`,
// counting nested tags of the same name; npos when unbalanced.
size_t elementEnd(std::string_view h, size_t start, std::string_view name) {
    int depth = 0;
    for (size_t pos = start; pos < h.size();) {
        const size_t lt = h.find('<', pos);
        if (lt == std::string_view::npos)
            return std::string_view::npos;
        ui::rich::Tag t;
        pos = ui::rich::readTag(h, lt, &t);
        if (pos == std::string_view::npos)
            return pos;
        if (str::iequals(t.name, name) && t.text.back() != '/') {
            depth += t.closing ? -1 : 1;
            if (depth == 0)
                return pos;
        }
    }
    return std::string_view::npos;
}

// An element's markdown as the editor would produce it.
std::string elementMd(std::string_view element) {
    const std::string        eh = editorHtml(element, {}, nullptr);
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(eh, &text, &fmt, &links);
    return markdown(text, fmt, links);
}

std::string_view tagId(std::string_view openTag) {
    return attr(openTag, "id");
}

} // namespace

std::vector<Chunk> documentChunks(std::string_view md) {
    std::vector<Chunk> out;
    Kind               open    = Kind::Para; // the group being extended
    bool               inGroup = false, fence = false;
    auto               push = [&](Kind k, std::string_view line, bool group) {
        std::string l(line);
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t'))
            l.pop_back();
        if (group && inGroup && open == k && !out.empty()) {
            out.back().md += '\n';
            out.back().md += l;
            return;
        }
        Chunk c;
        c.kind    = k;
        c.fragile = k == Kind::Quote || k == Kind::Table;
        c.md      = std::move(l);
        out.push_back(std::move(c));
        open    = k;
        inGroup = group;
    };
    for (size_t at = 0; at <= md.size();) {
        const size_t           nl   = std::min(md.find('\n', at), md.size());
        const std::string_view line = md.substr(at, nl - at);
        at                          = nl + 1;
        const std::string_view t    = str::trim(line);
        if (fence) {
            out.back().md += '\n';
            out.back().md += std::string(line);
            if (str::startsWith(t, "```"))
                fence = false;
            continue;
        }
        if (t.empty()) {
            inGroup = false;
            continue;
        }
        if (str::startsWith(t, "```")) {
            push(Kind::Para, line, false);
            inGroup = false;
            fence   = true;
            continue;
        }
        if (t[0] == '|')
            push(Kind::Table, line, true);
        else if (str::startsWith(t, "> ") || t == ">")
            push(Kind::Quote, line, true);
        else if (isListLine(line))
            push(Kind::List, line, true);
        else if (t[0] == '#')
            push(Kind::Heading, line, false);
        else
            push(Kind::Para, line, false);
    }
    return out;
}

bool baseChunks(
    std::string_view html, const std::vector<std::string> &titles, std::vector<Chunk> *out
) {
    out->clear();
    const std::string_view h = bodyOf(html, titles);
    for (size_t pos = 0; pos < h.size();) {
        pos = h.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string_view::npos)
            break;
        if (h[pos] != '<')
            return false; // stray text at the top level: unknown structure
        const size_t gt = h.find('>', pos);
        if (gt == std::string_view::npos)
            return false;
        const std::string_view open = h.substr(pos + 1, gt - pos - 1);
        size_t                 ne   = 0;
        while (ne < open.size() && open[ne] != ' ' && open[ne] != '/' && open[ne] != '>')
            ++ne;
        const std::string name(open.substr(0, ne));
        // A top-level picture: invisible context (its section is never
        // rewritten, so the picture survives on the server).
        if (str::iequals(name, "img") || str::iequals(name, "br")) {
            pos = gt + 1;
            continue;
        }
        const size_t end = elementEnd(h, pos, name);
        if (end == std::string_view::npos)
            return false;
        const std::string_view element = h.substr(pos, end - pos);
        pos                            = end;
        std::string_view id;
        bool             fragile = false;
        if (name.size() == 2 && str::asciiLower(name[0]) == 'h' && name[1] >= '1' &&
            name[1] <= '6') {
            id = tagId(open);
        } else if (str::iequals(name, "p")) {
            id = tagId(open);
        } else if (
            str::iequals(name, "div") || str::iequals(name, "ul") || str::iequals(name, "ol")
        ) {
            // A list: the <ul> carries the section id.
            size_t ul = element.find("<ul");
            if (ul == std::string_view::npos)
                ul = element.find("<ol");
            if (ul == std::string_view::npos)
                return false;
            id = tagId(element.substr(ul, element.find('>', ul) - ul));
        } else if (str::iequals(name, "blockquote") || str::iequals(name, "table")) {
            fragile = true;
        } else {
            return false; // <hr>, embeds, …: the whole-document save
        }
        if (!fragile && id.empty())
            return false;
        std::vector<Chunk> parts = documentChunks(elementMd(element));
        // One element may read as several editor chunks (a <p> with <br>s):
        // the first takes the id, the rest are context only.
        for (size_t i = 0; i < parts.size(); ++i) {
            parts[i].fragile = parts[i].fragile || fragile || i > 0;
            if (i == 0 && !fragile)
                parts[i].id = std::string(id);
            out->push_back(std::move(parts[i]));
        }
    }
    return true;
}

bool diff(const std::vector<Chunk> &base, const std::vector<Chunk> &cur, std::vector<Change> *ops) {
    using Op    = Change::Op;
    const int n = int(base.size()), m = int(cur.size());
    auto eq = [&](int i, int j) { return base[i].kind == cur[j].kind && base[i].md == cur[j].md; };
    // The sections an edit left alone at both ends match as they are: the
    // table covers only the middle [p, n2) × [p, m2) (one edit in a long
    // canvas is a few cells, not n × m).
    int  p = 0, s = 0;
    while (p < n && p < m && eq(p, p))
        ++p;
    while (s < n - p && s < m - p && eq(n - 1 - s, m - 1 - s))
        ++s;
    const int        n2 = n - s, m2 = m - s, w = m2 - p + 1;
    // Longest common subsequence, suffix table.
    std::vector<int> dp(size_t(n2 - p + 1) * size_t(w), 0);
    auto at = [&](int i, int j) -> int & { return dp[size_t(i - p) * size_t(w) + size_t(j - p)]; };
    for (int i = n2 - 1; i >= p; --i)
        for (int j = m2 - 1; j >= p; --j)
            at(i, j) = eq(i, j) ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
    std::vector<Change> inserts, replaces, deletes;
    // Slack rejects an empty document_content; a lone space is an empty line.
    auto safe = [](const std::string &md) { return md.empty() ? std::string(" ") : md; };
    int  i = p, j = p;
    while (i < n2 || j < m2) {
        if (i < n2 && j < m2 && eq(i, j)) {
            ++i, ++j;
            continue;
        }
        // The maximal unmatched gap [i, bi) × [j, cj).
        int bi = i, cj = j;
        while (bi < n2 || cj < m2) {
            if (bi < n2 && cj < m2 && eq(bi, cj) && at(bi, cj) == at(i, j))
                break; // the next match
            if (bi < n2 && at(bi + 1, cj) == at(i, j)) {
                ++bi;
                continue;
            }
            if (cj < m2 && at(bi, cj + 1) == at(i, j)) {
                ++cj;
                continue;
            }
            break;
        }
        const int lenB = bi - i, lenC = cj - j, pairs = std::min(lenB, lenC);
        for (int p = 0; p < pairs; ++p) {
            const Chunk &b = base[size_t(i + p)];
            if (b.fragile || b.id.empty())
                return false;
            replaces.push_back({Op::ReplaceSection, safe(cur[size_t(j + p)].md), b.id});
        }
        for (int p = pairs; p < lenB; ++p) {
            const Chunk &b = base[size_t(i + p)];
            if (b.fragile || b.id.empty())
                return false;
            deletes.push_back({Op::DeleteSection, {}, b.id});
        }
        if (lenC > pairs) {
            // An anchor for the extra chunks; inserts go first, so one that
            // is replaced or deleted later is still alive.
            std::string after, before;
            if (lenB > 0 && !base[size_t(i + lenB - 1)].fragile)
                after = base[size_t(i + lenB - 1)].id;
            else if (bi < n && !base[size_t(bi)].fragile && !base[size_t(bi)].id.empty())
                before = base[size_t(bi)].id;
            else if (i > 0 && !base[size_t(i - 1)].fragile && !base[size_t(i - 1)].id.empty())
                after = base[size_t(i - 1)].id;
            else if (n == 0)
                return false; // nothing to anchor on: replace the whole document
            else
                return false;
            if (!before.empty())
                for (int p = pairs; p < lenC; ++p)
                    inserts.push_back({Op::InsertBefore, safe(cur[size_t(j + p)].md), before});
            else // insert_after the same id: in reverse, so they end up in order
                for (int p = lenC - 1; p >= pairs; --p)
                    inserts.push_back({Op::InsertAfter, safe(cur[size_t(j + p)].md), after});
        }
        i = bi;
        j = cj;
    }
    ops->clear();
    ops->insert(ops->end(), inserts.begin(), inserts.end());
    ops->insert(ops->end(), replaces.begin(), replaces.end());
    ops->insert(ops->end(), deletes.begin(), deletes.end());
    return true;
}

} // namespace screens::canvas
