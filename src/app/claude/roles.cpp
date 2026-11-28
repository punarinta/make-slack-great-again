#include "app/claude/roles.h"

#include "app/claude/avatar_glyphs.h"
#include "app/claude/common.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/gfx.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace claude {
namespace {

struct Spec {
    const char *id;
    const char *name;        // English: the prompt header's; shown translated
    const char *description; // shown translated
    const char *glyph;       // the picture it comes with: `glyph` on `color`
    uint32_t    color;
    const char *prompt; // without the header line
};

// clang-format off
const Spec kSpecs[] = {
    {"generalist", N_("Generalist"), N_("Claude Code as it comes: any task."),
     "terminal", 0xD97757, ""},
    {"engineer", N_("Engineer"), N_("Writes, debugs and reviews code, and proves it works."),
     "code-xml", 0x2F6FDB,
     "You are the team's software engineer. You design, write, debug, refactor and review code.\n"
     "- Understand the existing code and its conventions before changing it, and match them.\n"
     "- Prefer small, focused changes that are easy to review; don't widen the scope unasked.\n"
     "- Prove a change works: build it and run the relevant tests, and add tests for new "
     "behaviour where the project has them.\n"
     "- Look for root causes rather than papering over symptoms, and say plainly what you "
     "verified and what you didn't.\n"
     "- When there is a real design trade-off, name it briefly and recommend one option."},
    {"designer", N_("Designer"), N_("Shapes flows, screens and interface copy; builds mockups."),
     "palette", 0xC2417A,
     "You are the team's product designer. You shape how things look, read and behave for "
     "the people who use them.\n"
     "- Start from the user's goal and the flow around it, then the screen, then the details.\n"
     "- Make ideas concrete: sketch layouts, write the interface copy, and build mockups or "
     "prototypes (HTML/CSS, SVG) when showing beats describing.\n"
     "- Keep to the product's existing visual language and components unless asked to change "
     "them; consistency is a feature.\n"
     "- Care about hierarchy, spacing, contrast, states (empty, loading, error) and "
     "accessibility.\n"
     "- When critiquing, be specific: what doesn't work, why, and a better alternative."},
    {"marketer", N_("Marketer"), N_("Positioning, messaging, launch plans and copy."),
     "megaphone", 0x1F8A5B,
     "You are the team's marketer. You work on positioning, messaging and getting the product "
     "in front of the right people.\n"
     "- Be clear about who the audience is and what they care about before writing a word.\n"
     "- Write in the product's own voice: concrete benefits over adjectives, short sentences, "
     "no hype.\n"
     "- Ground every claim in what the product really does (read the code, docs and changelog "
     "when they are here); never invent numbers, quotes or customers.\n"
     "- For launches and campaigns, propose channels, timing and what to measure.\n"
     "- Offer a couple of distinct options for headlines and taglines, with your pick."},
    {"researcher", N_("Researcher"), N_("Digs into questions and reports findings with sources."),
     "telescope", 0x6D4FC9,
     "You are the team's researcher. You investigate questions thoroughly and report what is "
     "known, how well, and from where.\n"
     "- Break a question down, then gather evidence from the web, documentation, data and code "
     "as fits.\n"
     "- Cite your sources, prefer primary ones, and check claims against more than one when it "
     "matters.\n"
     "- Keep facts, inferences and opinions apart, and say how confident you are.\n"
     "- Lead with the answer, then the supporting detail; call out open questions and what "
     "would settle them.\n"
     "- Don't stop at the first plausible answer when the question deserves more digging."},
};

// The pictures the built-ins come with (gfx/claude_code_avatar.svg,
// gfx/roles/<id>.svg), compiled in — the app has no resource bundle — and
// written once to the avatars folder, as `kPictureFiles` names them.
const char *const kPictures[] = {
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" height=\"128\">"
    "<rect width=\"128\" height=\"128\" rx=\"28\" fill=\"#D97757\"/>"
    "<polyline points=\"34,40 60,64 34,88\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"12\" "
    "stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "<rect x=\"66\" y=\"80\" width=\"30\" height=\"11\" rx=\"5.5\" fill=\"#FFFFFF\"/></svg>",

    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" height=\"128\">"
    "<rect width=\"128\" height=\"128\" rx=\"28\" fill=\"#2F6FDB\"/>"
    "<g transform=\"translate(26 26) scale(3.1667)\" fill=\"none\" stroke=\"#FFFFFF\" "
    "stroke-width=\"2.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
    "<path d=\"m18 16 4-4-4-4\"/><path d=\"m6 8-4 4 4 4\"/><path d=\"m14.5 4-5 16\"/></g></svg>",

    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" height=\"128\">"
    "<rect width=\"128\" height=\"128\" rx=\"28\" fill=\"#C2417A\"/>"
    "<g transform=\"translate(26 26) scale(3.1667)\" fill=\"none\" stroke=\"#FFFFFF\" "
    "stroke-width=\"2.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
    "<path d=\"M12 22a1 1 0 0 1 0-20 10 9 0 0 1 10 9 5 5 0 0 1-5 5h-2.25a1.75 1.75 0 0 0-1.4 "
    "2.8l.3.4a1.75 1.75 0 0 1-1.4 2.8z\"/><circle cx=\"13.5\" cy=\"6.5\" r=\"1\" fill=\"#FFFFFF\"/>"
    "<circle cx=\"17.5\" cy=\"10.5\" r=\"1\" fill=\"#FFFFFF\"/><circle cx=\"6.5\" cy=\"12.5\" "
    "r=\"1\" fill=\"#FFFFFF\"/><circle cx=\"8.5\" cy=\"7.5\" r=\"1\" fill=\"#FFFFFF\"/></g></svg>",

    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" height=\"128\">"
    "<rect width=\"128\" height=\"128\" rx=\"28\" fill=\"#1F8A5B\"/>"
    "<g transform=\"translate(26 26) scale(3.1667)\" fill=\"none\" stroke=\"#FFFFFF\" "
    "stroke-width=\"2.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
    "<path d=\"M11 6a13 13 0 0 0 8.4-2.8A1 1 0 0 1 21 4v12a1 1 0 0 1-1.6.8A13 13 0 0 0 11 14H5a2 "
    "2 0 0 1-2-2V8a2 2 0 0 1 2-2z\"/><path d=\"M6 14a12 12 0 0 0 2.4 7.2 2 2 0 0 0 3.2-2.4A8 8 0 "
    "0 1 10 14\"/><path d=\"M8 6v8\"/></g></svg>",

    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" height=\"128\">"
    "<rect width=\"128\" height=\"128\" rx=\"28\" fill=\"#6D4FC9\"/>"
    "<g transform=\"translate(26 26) scale(3.1667)\" fill=\"none\" stroke=\"#FFFFFF\" "
    "stroke-width=\"2.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
    "<path d=\"m10.065 12.493-6.18 1.318a.934.934 0 0 1-1.108-.702l-.537-2.15a1.07 1.07 0 0 1 "
    ".691-1.265l13.504-4.44\"/><path d=\"m13.56 11.747 4.332-.924\"/><path d=\"m16 21-3.105-6.21\"/>"
    "<path d=\"M16.485 5.94a2 2 0 0 1 1.455-2.425l1.09-.272a1 1 0 0 1 1.212.727l1.515 6.06a1 1 0 0 "
    "1-.727 1.213l-1.09.272a2 2 0 0 1-2.425-1.455z\"/><path d=\"m6.158 8.633 1.114 4.456\"/>"
    "<path d=\"m8 21 3.105-6.21\"/><circle cx=\"12\" cy=\"13\" r=\"2\"/></g></svg>",
};
// Named after what each shows: a changed picture gets a new name.
const char *const kPictureFiles[] = {
    "claude-code.svg", "role-engineer.svg", "role-designer.svg", "role-marketer.svg",
    "role-researcher.svg",
};
// clang-format on
static_assert(std::size(kPictures) == std::size(kSpecs), "a picture per built-in");
static_assert(std::size(kPictureFiles) == std::size(kSpecs), "a file per built-in");

constexpr std::string_view kHeaderPrefix = "# Your role: ";

// Claude Code's own subagent types (2.1.282); a teammate by one of these
// names would shadow it.
const char *const kTakenTypes[] = {
    "claude",
    "general-purpose",
    "explore",
    "plan",
    "statusline-setup",
    "claude-code-guide",
};

bool taken(std::string_view id) {
    for (const char *t : kTakenTypes)
        if (id == t)
            return true;
    return false;
}

constexpr std::string_view kNoteOpen  = "<msga-teammates>";
constexpr std::string_view kNoteClose = "</msga-teammates>";

// Where every picture goes: one file each, named after what it shows — no
// staleness to manage. "" before the directories are set (nothing written).
std::string avatarsDir() {
    return dirs().data.empty() ? std::string() : dirs().data + "/team/avatars";
}

// The file at `dir`/`name`, written with `svg` when it isn't there yet; ""
// when there is no `dir`.
std::string pictureFile(const std::string &dir, std::string_view name, std::string_view svg) {
    if (dir.empty())
        return {};
    std::string path = file::join(dir, name);
    if (!file::exists(path))
        file::writeAtomic(path, svg);
    return path;
}

std::string builtInPicture(size_t i) {
    return pictureFile(avatarsDir(), kPictureFiles[i], kPictures[i]);
}

const std::string &nameOf(const Role &r) {
    return r.promptName.empty() ? r.name : r.promptName;
}

// \w as a Unicode regex has it: letters, digits, the underscore.
bool isWordCp(uint32_t cp) {
    return cp == '_' || utf8::isWordChar(cp);
}

// The header line's name and id: "Data analyst (msga: data-analyst)" — or, as
// sessions started before ids were written have it, a built-in's English
// name alone ("Engineer"). Reads only the built-ins' table: the catalog calls
// it from a worker thread.
RoleMark parseHeaderLine(std::string_view line) {
    const std::string_view t = str::trimSpace(line);
    if (!t.empty() && t.back() == ')') {
        constexpr std::string_view kOpen = "(msga: ";
        const size_t               at    = t.rfind(kOpen);
        if (at != std::string_view::npos) {
            const std::string_view id =
                t.substr(at + kOpen.size(), t.size() - 1 - at - kOpen.size());
            const bool ok = !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            });
            if (ok)
                return {std::string(id), std::string(str::trimSpace(t.substr(0, at)))};
        }
    }
    for (const Spec &s : kSpecs)
        if (t == s.name)
            return {s.id, std::string(t)};
    return {};
}

// The lines of a teammate file's header: "key: value".
std::unordered_map<std::string, std::string> parseFields(std::string_view block) {
    std::unordered_map<std::string, std::string> out;
    str::Splitter                                lines(block, '\n');
    for (std::string_view l; lines.next(&l);) {
        const size_t colon = l.find(':');
        if (colon != std::string_view::npos && colon > 0)
            out[std::string(str::trimSpace(l.substr(0, colon)))] =
                std::string(str::trimSpace(l.substr(colon + 1)));
    }
    return out;
}

std::string field(
    const std::unordered_map<std::string, std::string> &fields,
    const char                                         *key,
    const std::string                                  &def
) {
    const auto it = fields.find(key);
    return it == fields.end() ? def : it->second;
}

// "#rrggbb" (or "rrggbb") → 0xRRGGBB; false when it isn't one.
bool parseColor(std::string_view s, uint32_t *out) {
    gfx::Color c = 0;
    if (s.size() - (!s.empty() && s[0] == '#') != 6 || !gfx::parseHexColor(s, &c))
        return false;
    *out = c & 0xffffff;
    return true;
}

bool separatorCp(uint32_t cp) {
    return cp == '-' || utf8::isSpace(cp);
}

// The words of a name, split at whitespace and hyphens, case-folded.
std::vector<std::vector<uint32_t>> wordsOf(std::string_view name) {
    std::vector<std::vector<uint32_t>> out;
    std::vector<uint32_t>              word;
    for (uint32_t cp : utf8::codePoints(name, true)) {
        if (separatorCp(cp)) {
            if (!word.empty())
                out.push_back(std::move(word));
            word.clear();
        } else {
            word.push_back(cp);
        }
    }
    if (!word.empty())
        out.push_back(std::move(word));
    return out;
}

bool boundaryAfter(const std::vector<uint32_t> &p, size_t i) {
    return i >= p.size() || !(isWordCp(p[i]) || p[i] == '-');
}

// Whether the words, joined by whitespace or hyphens, then maybe "s" or "es",
// stand in `p` at `i` and end a word there.
bool wordsAt(
    const std::vector<uint32_t> &p, size_t i, const std::vector<std::vector<uint32_t>> &words
) {
    for (size_t w = 0; w < words.size(); ++w) {
        if (w > 0) {
            if (i >= p.size() || !separatorCp(p[i]))
                return false;
            while (i < p.size() && separatorCp(p[i]))
                ++i;
        }
        for (uint32_t cp : words[w])
            if (i >= p.size() || p[i++] != cp)
                return false;
    }
    if (i + 1 < p.size() && p[i] == 'e' && p[i + 1] == 's' && boundaryAfter(p, i + 2))
        return true;
    if (i < p.size() && p[i] == 's' && boundaryAfter(p, i + 1))
        return true;
    return boundaryAfter(p, i);
}

// Whether `prompt` names `role` as a word, in any case, maybe plural: its
// name, English name or id ("a marketer", "Data analysts", "data-analyst").
bool namedIn(std::string_view prompt, const Role &role) {
    std::vector<std::vector<std::vector<uint32_t>>> alternatives;
    for (const std::string *n : {&role.name, &role.promptName, &role.id})
        if (auto words = wordsOf(*n); !words.empty())
            alternatives.push_back(std::move(words));
    if (alternatives.empty())
        return false;
    const std::vector<uint32_t> p = utf8::codePoints(prompt, true);
    for (size_t i = 0; i < p.size(); ++i) {
        if (i > 0) {
            const uint32_t b = p[i - 1];
            if (isWordCp(b) || b == '@' || b == '/' || b == ':' || b == '.' || b == '-')
                continue;
        }
        for (const auto &words : alternatives)
            if (wordsAt(p, i, words))
                return true;
    }
    return false;
}

// Whether the code point ending at byte `at` of `s` is one a mention may not
// follow: a word character, or one of "@/:.-".
bool joinedBefore(std::string_view s, size_t at) {
    if (at == 0)
        return false;
    size_t         i = utf8::prevBoundary(s, at);
    const uint32_t b = utf8::decode(s, i);
    return isWordCp(b) || b == '@' || b == '/' || b == ':' || b == '.' || b == '-';
}

bool idChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

} // namespace

Mention mentionAt(std::string_view text, size_t i) {
    constexpr std::string_view kAgent = "@claude:agent", kRole = "@claude:role:";
    if (i >= text.size() || text[i] != '@' || joinedBefore(text, i))
        return {};
    Mention m;
    size_t  end = 0;
    if (text.substr(i, kAgent.size()) == kAgent) {
        end = i + kAgent.size();
    } else if (text.substr(i, kRole.size()) == kRole) {
        const size_t from = i + kRole.size();
        end               = from;
        while (end < text.size() && idChar(text[end]))
            ++end;
        if (end == from)
            return {};
        m.roleId = std::string(text.substr(from, end - from));
    } else {
        return {};
    }
    // "(?![\w-])": not the start of a longer word.
    if (end < text.size()) {
        size_t         j  = end;
        const uint32_t cp = utf8::decode(text, j);
        if (isWordCp(cp) || cp == '-')
            return {};
    }
    m.len = end - i;
    return m;
}

namespace {
// "# Your role: <name> (msga: <id>)".
std::string roleHeader(std::string_view name, std::string_view id) {
    return str::concat({kHeaderPrefix, name, " (msga: ", id, ")"});
}
} // namespace

std::string appendedPrompt(const Role &role) {
    const std::string_view body = str::trimSpace(role.prompt);
    if (body.empty())
        return {};
    return str::concat({roleHeader(nameOf(role), role.id), "\n", body});
}

std::string subagentsJson(const std::vector<Role> &roles) {
    // By id, as a JSON object's keys sort; a later role of the same id wins.
    std::vector<const Role *> picked;
    for (const Role &r : roles) {
        if (appendedPrompt(r).empty() || taken(r.id))
            continue;
        picked.erase(
            std::remove_if(
                picked.begin(), picked.end(), [&](const Role *p) { return p->id == r.id; }
            ),
            picked.end()
        );
        picked.push_back(&r);
    }
    if (picked.empty())
        return {};
    std::sort(picked.begin(), picked.end(), [](const Role *a, const Role *b) {
        return a->id < b->id;
    });
    json::Writer w;
    w.beginObject();
    for (const Role *r : picked) {
        w.key(r->id).beginObject();
        w.key("description")
            .value(
                str::trimSpace(
                    str::concat(
                        {nameOf(*r),
                         ", a teammate (mentioned as @claude:role:",
                         r->id,
                         "). ",
                         str::simplified(r->description)}
                    )
                )
            );
        w.key("prompt").value(appendedPrompt(*r));
        w.endObject();
    }
    w.endObject();
    return w.take();
}

std::string teammateNote(
    std::string_view                                        prompt,
    const std::function<const Role *(std::string_view id)> &find,
    const std::vector<Role>                                &byName
) {
    std::vector<std::string> seen;
    const auto               isSeen = [&](std::string_view id) {
        return std::find(seen.begin(), seen.end(), id) != seen.end();
    };
    std::string body;
    for (size_t at = prompt.find('@'); at != std::string_view::npos;
         at        = prompt.find('@', at + 1)) {
        Mention mention = mentionAt(prompt, at);
        if (mention.roleId.empty())
            continue; // none, or the Generalist: no prompt of its own to note
        const std::string id = std::move(mention.roleId);
        if (isSeen(id))
            continue;
        seen.push_back(id);
        const Role       *r      = find(id);
        const std::string append = r ? appendedPrompt(*r) : std::string();
        if (append.empty())
            continue;
        const std::string &name = nameOf(*r);
        body += taken(id) ? str::concat(
                                {"\n@claude:role:",
                                 id,
                                 " is the teammate ",
                                 name,
                                 ". To spawn it, use the Agent tool with subagent_type \"claude\" "
                                 "and begin the prompt with its role, verbatim:\n"}
                            )
                          : str::concat(
                                {"\n@claude:role:",
                                 id,
                                 " is the teammate ",
                                 name,
                                 ". To spawn it, use the Agent tool with subagent_type \"",
                                 id,
                                 "\". If there is no such type, use subagent_type \"claude\" "
                                 "instead and begin the prompt with its role, verbatim:\n"}
                            );
        body += append;
        body += '\n';
    }
    for (const Role &r : byName) {
        if (isSeen(r.id) || !namedIn(prompt, r))
            continue;
        const std::string append = appendedPrompt(r);
        if (append.empty())
            continue;
        seen.push_back(r.id);
        body += str::concat(
            {"\n",
             nameOf(r),
             " is the teammate @claude:role:",
             r.id,
             ". If you spawn it, use the Agent tool with subagent_type \"claude\" and begin the "
             "prompt with its role, verbatim:\n"}
        );
        body += append;
        body += '\n';
    }
    if (body.empty())
        return {};
    return str::concat({"\n\n", kNoteOpen, body, kNoteClose});
}

std::string withoutTeammateNote(std::string_view prompt) {
    const size_t open = prompt.rfind(str::concat({"\n\n", kNoteOpen}));
    if (open == std::string_view::npos || !str::endsWith(str::trimSpace(prompt), kNoteClose))
        return std::string(prompt);
    return std::string(prompt.substr(0, open));
}

std::string roleInAgentPrompt(std::string_view prompt) {
    str::Splitter lines(prompt, '\n');
    for (std::string_view line; lines.next(&line);) {
        if (str::startsWith(line, kHeaderPrefix))
            if (RoleMark m = parseHeaderLine(line.substr(kHeaderPrefix.size())); !m.id.empty())
                return m.id;
    }
    // "^\s*Role:\s*([A-Za-z][A-Za-z0-9-]*)\b": the line Claude writes itself.
    std::string_view s = str::trimSpace(prompt);
    if (!str::startsWith(s, "Role:"))
        return {};
    s                = str::trimSpace(s.substr(5));
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (s.empty() || !alpha(s[0]))
        return {};
    size_t end = 1;
    while (end < s.size() && (alpha(s[end]) || (s[end] >= '0' && s[end] <= '9') || s[end] == '-'))
        ++end;
    // Backs off to a word boundary, as the regex would: "eng-" before a space
    // ends at "eng".
    const auto wordAt = [&](size_t i) {
        if (i >= s.size())
            return false;
        size_t j = i;
        return isWordCp(utf8::decode(s, j));
    };
    while (end > 0 && wordAt(end - 1) == wordAt(end))
        --end;
    if (end == 0)
        return {};
    return str::asciiLower(s.substr(0, end));
}

RoleMark roleInSystemPrompt(const json::Value &systemPrompt) {
    for (size_t i = systemPrompt.size(); i-- > 0;) {
        const json::Value      v    = systemPrompt[i];
        const std::string_view part = v.isString() ? v.str() : v["text"].str();
        if (str::startsWith(part, kHeaderPrefix)) {
            const std::string_view rest = part.substr(kHeaderPrefix.size());
            return parseHeaderLine(rest.substr(0, rest.find('\n')));
        }
    }
    return {};
}

RoleMark roleInTranscriptBytes(std::string_view bytes) {
    // In the JSON the part is a string of its own: `"# Your role: Engineer\n…`.
    const std::string needle = str::concat({"\"", kHeaderPrefix});
    const size_t      at     = bytes.rfind(needle);
    if (at == std::string_view::npos)
        return {};
    // Our part goes on after the header line; a prompt someone typed that
    // happens to read like it ends there instead.
    const std::string_view rest = bytes.substr(at + needle.size(), 400);
    std::string            line;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '"')
            return {}; // the string ended on this line
        if (rest[i] == '\\') {
            if (i + 1 < rest.size() && rest[i + 1] == 'n')
                return parseHeaderLine(line);
            if (i + 1 < rest.size())
                line += rest[++i]; // \" or \\ in the name
            continue;
        }
        line += rest[i];
    }
    return {}; // cut off before the line ended
}

const std::vector<Role> &builtInRoles() {
    // Built on first use, after the language is set. The pictures' files
    // follow the directories (tests point them elsewhere case by case).
    static std::vector<Role> all = [] {
        std::vector<Role> out;
        for (const Spec &s : kSpecs) {
            Role r;
            r.id          = s.id;
            r.name        = i18n::tr(s.name);
            r.description = i18n::tr(s.description);
            r.glyph       = s.glyph;
            r.color       = s.color;
            r.prompt      = s.prompt;
            r.promptName  = s.name;
            r.builtIn     = true;
            out.push_back(std::move(r));
        }
        return out;
    }();
    static std::string picturesIn = "\x01"; // never a directory: the first call fills them
    if (std::string dir = avatarsDir(); picturesIn != dir) {
        picturesIn = std::move(dir);
        for (size_t i = 0; i < all.size(); ++i)
            all[i].avatar = builtInPicture(i);
    }
    return all;
}

std::string agentAvatarPath() {
    return builtInPicture(0);
}

// ── Team ─────────────────────────────────────────────────────────────────────

Team::Team(std::string dir) : _dir(std::move(dir)) {
    load();
}

void Team::load() {
    _resolved.clear();
    _roles = builtInRoles();
    std::vector<Role>           added;
    std::vector<file::DirEntry> entries;
    file::listDir(_dir, &entries);
    // In name order, as a directory listing comes on every OS.
    std::sort(entries.begin(), entries.end(), [](const file::DirEntry &a, const file::DirEntry &b) {
        return a.name < b.name;
    });
    for (const file::DirEntry &de : entries) {
        if (de.isDir || !str::endsWith(de.name, ".md"))
            continue;
        std::string text;
        if (!file::readAll(file::join(_dir, de.name), &text))
            continue;
        if (!str::startsWith(text, "---\n"))
            continue;
        const size_t close = text.find("\n---", 4);
        if (close == std::string::npos)
            continue;
        const auto        fields = parseFields(std::string_view(text).substr(4, close - 4));
        const std::string id     = de.name.substr(0, de.name.size() - 3);
        Role              r;
        const auto        base =
            std::find_if(_roles.begin(), _roles.end(), [&](const Role &b) { return b.id == id; });
        if (base != _roles.end())
            r = *base;
        r.id          = id;
        r.name        = field(fields, "name", r.name);
        r.description = field(fields, "description", r.description);
        r.glyph       = field(fields, "glyph", r.glyph);
        uint32_t color;
        if (parseColor(field(fields, "color", {}), &color))
            r.color = color;
        r.created         = std::strtoll(field(fields, "created", {}).c_str(), nullptr, 10);
        r.removed         = !r.builtIn && field(fields, "removed", {}) == "true";
        // The prompt: everything after the closing line.
        const size_t body = text.find('\n', close + 4);
        r.prompt = body == std::string::npos
                       ? std::string()
                       : std::string(str::trimSpace(std::string_view(text).substr(body + 1)));
        if (r.name.empty())
            continue;
        if (base != _roles.end()) {
            r.edited     = true;
            r.promptName = r.name == base->name ? base->promptName : r.name;
            r.avatar     = avatarFor(r);
            *base        = r;
        } else {
            r.promptName = r.name;
            r.avatar     = avatarFor(r);
            added.push_back(std::move(r));
        }
    }
    std::sort(added.begin(), added.end(), [](const Role &a, const Role &b) {
        return a.created != b.created ? a.created < b.created : a.id < b.id;
    });
    for (auto &r : added)
        _roles.push_back(std::move(r));
    _listed.clear();
    for (const Role &r : _roles)
        if (!r.removed)
            _listed.push_back(r);
}

const Role *Team::find(std::string_view id) const {
    for (const Role &r : _roles)
        if (r.id == id)
            return &r;
    return nullptr;
}

const Role &Team::resolve(std::string_view id, std::string_view nameHint) const {
    if (id.empty())
        return generalist();
    if (const Role *r = find(id))
        return *r;
    // A former teammate: named as its sessions have it, in a plain grey —
    // made once (its picture is a file).
    auto [it, made] = _resolved.try_emplace(str::concat({id, "\n", nameHint}));
    if (!made)
        return it->second;
    Role &r = it->second;
    r.id    = id;
    if (!nameHint.empty()) {
        r.name = nameHint;
    } else {
        const auto it = _formers.find(r.id);
        r.name        = it == _formers.end() ? r.id : it->second;
    }
    r.promptName = r.name;
    r.glyph      = "briefcase";
    r.color      = avatar_glyphs::colors().back();
    r.former     = true;
    r.avatar     = avatarFor(r);
    return r;
}

bool Team::noteFormer(std::string_view id, std::string_view name) {
    if (id.empty() || find(id) || name.empty())
        return false;
    std::string &have = _formers[std::string(id)];
    if (have == name)
        return false;
    have = name;
    _resolved.clear(); // named anew
    return true;
}

std::string Team::avatarFor(const Role &role) const {
    // A built-in as it comes keeps its own picture.
    for (const Role &b : builtInRoles())
        if (b.id == role.id && b.glyph == role.glyph && b.color == role.color)
            return b.avatar;
    // Others are drawn once into a file named after what it shows.
    const std::string glyph =
        avatar_glyphs::hasGlyph(role.glyph) ? role.glyph : avatar_glyphs::glyphs().front().id;
    const std::string name = str::concat({glyph, "-", gfx::hexColor(role.color).substr(1), ".svg"});
    std::string       dir  = avatarsDir();
    if (dir.empty())
        dir = file::join(_dir, "avatars");
    return pictureFile(dir, name, avatar_glyphs::svg(role.glyph, role.color));
}

std::string Team::newId(std::string_view name) const {
    std::string base;
    for (char c : str::asciiLower(name)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            base += c;
        else if (!base.empty() && base.back() != '-')
            base += '-';
    }
    while (!base.empty() && base.back() == '-')
        base.pop_back();
    if (base.size() > 40)
        base.resize(40);
    if (base.empty())
        base = "teammate";
    // Never one a session could already carry: taken ids, formers, old files.
    std::string id = base;
    for (int n = 2; find(id) || _formers.count(id) || file::exists(file::join(_dir, id + ".md"));
         ++n)
        id = str::concat({base, "-", str::number(n)});
    return id;
}

bool Team::write(const Role &r, std::string *error) {
    std::string text = "---\n";
    text += str::concat({"name: ", str::simplified(r.name), "\n"});
    text += str::concat({"description: ", str::simplified(r.description), "\n"});
    text += str::concat({"glyph: ", r.glyph, "\n"});
    text += str::concat({"color: ", gfx::hexColor(r.color), "\n"});
    if (r.created > 0)
        text += str::concat({"created: ", str::number(r.created), "\n"});
    if (r.removed)
        text += "removed: true\n";
    text += str::concat({"---\n", str::trimSpace(r.prompt), "\n"});
    errno = 0;
    if (!file::writeAtomic(file::join(_dir, r.id + ".md"), text)) {
        if (error)
            *error = errno ? std::strerror(errno) : "write failed";
        return false;
    }
    return true;
}

std::string Team::save(Role role, std::string *error) {
    role.name = str::simplified(role.name);
    if (role.name.empty()) {
        if (error)
            *error = i18n::tr("A teammate needs a name.");
        return {};
    }
    if (!avatar_glyphs::hasGlyph(role.glyph))
        role.glyph = avatar_glyphs::glyphs().front().id;
    if (role.color == 0) // unset (black isn't on offer)
        role.color = avatar_glyphs::colors().front();
    const Role *old = role.id.empty() ? nullptr : find(role.id);
    if (role.id.empty()) {
        role.id      = newId(role.name);
        role.created = base::nowMicros() / 1000;
    } else if (!old) {
        if (error)
            *error = i18n::tr("That teammate is gone.");
        return {};
    } else {
        role.created = old->created;
        role.builtIn = old->builtIn;
    }
    role.removed = false;
    if (!write(role, error))
        return {};
    load();
    return role.id;
}

bool Team::remove(std::string_view id) {
    const Role *r = find(id);
    if (!r || r->builtIn || r->removed)
        return false;
    Role gone    = *r;
    gone.removed = true;
    if (!write(gone, nullptr))
        return false;
    load();
    return true;
}

bool Team::restore(std::string_view id) {
    const Role *r = find(id);
    if (!r || !r->builtIn || !r->edited)
        return false;
    if (!file::remove(file::join(_dir, str::concat({id, ".md"}))))
        return false;
    load();
    return true;
}

} // namespace claude
