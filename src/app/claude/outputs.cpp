#include "app/claude/outputs.h"

#include "app/claude/common.h"
#include "app/model/image_size.h"
#include "base/file.h"
#include "base/mime.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/gfx.h"
#include "prim/bytes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace claude {

namespace {

constexpr int     kMaxFiles     = 10;
constexpr int64_t kMaxFileBytes = 50LL * 1024 * 1024;
// A file counts as the turn's when it was modified from a little before its
// prompt to a little after the answer (clocks, and the answer's record being
// stamped once written).
constexpr int64_t kSlackMicros  = 2'000'000;

constexpr std::string_view kIndex         = "index.json";
constexpr size_t           kMaxIndexBytes = 1024 * 1024; // ten entries are a few hundred bytes

// The kinds shown as attachments: pictures, sound, video, PDF, HTML and CSV
// (mime's table by extension; text, JSON and the like stay paths).
std::string_view shownMimeOf(std::string_view path) {
    const std::string_view m = mime::fromName(path);
    const bool shown = str::startsWith(m, "image/") || str::startsWith(m, "audio/") ||
                       str::startsWith(m, "video/") || m == "application/pdf" || m == "text/html" ||
                       m == "text/csv";
    return shown ? m : std::string_view();
}

bool shownKind(std::string_view path) {
    return !shownMimeOf(path).empty();
}

std::string_view prettyTypeOf(std::string_view name, std::string_view mime) {
    if (const std::string_view m = shownMimeOf(name); !m.empty() && mime == m)
        return mime::label(m);
    return "File";
}

std::string expandHome(std::string_view token) {
    if (str::startsWith(token, "~/"))
        return str::concat({base::homeDir(), token.substr(1)});
    return std::string(token);
}

// "file:///a/b%20c" → "/a/b c": the local path a file URL names.
std::string localFileOf(std::string_view url) {
    return file::fromFileUrl(url);
}

bool isFile(std::string_view path) {
    return file::exists(path) && !file::isDir(path);
}

// What splits an answer into candidate paths: whitespace and `'"<>()[]{}|*,;.
bool splitsTokens(uint32_t cp) {
    return utf8::isSpace(cp) || (cp < 0x80 && std::strchr("`'\"<>()[]{}|*,;", int(cp)) && cp);
}

// Any character but [A-Za-z0-9._-] as '_': a folder name from a key.
std::string safeName(std::string_view key) {
    std::string out;
    for (size_t i = 0; i < key.size();) {
        const char c = key[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            out += c;
            ++i;
        } else {
            utf8::decode(key, i);
            out += '_';
        }
    }
    return out;
}

// A folder for the copies of one answer.
std::string messageDir(const OutputContext &ctx) {
    return file::join(outputsDir(ctx.convId), safeName(ctx.messageKey));
}

// Width and height of a BMP (which model::imageSize doesn't read).
bool bmpSize(const std::string &path, int32_t *w, int32_t *h) {
    std::string data;
    if (!str::endsWith(str::asciiLower(path), ".bmp") || !file::readAll(path, &data) ||
        data.size() < 26 || data[0] != 'B' || data[1] != 'M')
        return false;
    const auto le32 = [&](size_t at) { return int32_t(prim::le32(data.data() + at)); };
    *w              = le32(18);
    *h              = std::abs(le32(22)); // negative: top-down rows
    return *w > 0 && *h > 0;
}

model::File fileFor(json::Value e, const std::string &dir, const std::string &idPrefix, int n) {
    model::File f;
    f.id         = str::concat({idPrefix, str::number(n)});
    f.name       = e["name"].str();
    f.mime       = e["mime"].str();
    f.prettyType = prettyTypeOf(f.name, f.mime);
    f.path       = file::join(dir, e["file"].str());
    f.size       = std::max<int64_t>(0, file::size(f.path));
    // Decided here, not at copy time, so a build that decodes more (WebP)
    // shows the copies made before as pictures too.
    if (showsAsPicture(f.path, f.mime)) {
        f.width  = int32_t(e["w"].integer());
        f.height = int32_t(e["h"].integer());
    }
    return f;
}

// The copy of `src` as file `name` in `dir`, and how it's shown, written into
// `w` as an index entry; false when it can't be read.
bool copyInto(
    const std::string &src, const std::string &dir, const std::string &name, json::Writer &w
) {
    const std::string dest = file::join(dir, name);
    std::string       data;
    if (!file::readAll(src, &data) || !file::writeAtomic(dest, data, 0600))
        return false;
    const std::string_view kind = shownMimeOf(src);
    std::string            mime(kind.empty() ? std::string_view("application/octet-stream") : kind);
    int32_t                iw = 0, ih = 0;
    // An SVG is a picture at its own size (the image cache renders it).
    float                  sw = 0, sh = 0;
    if (mime == "image/svg+xml") {
        if (gfx::svgSize(data, &sw, &sh) && sw >= 1 && sh >= 1) {
            iw = int32_t(std::lround(sw));
            ih = int32_t(std::lround(sh));
        }
    } else if (
        str::startsWith(mime, "image/") && !model::imageSize(dest, &iw, &ih) &&
        !bmpSize(dest, &iw, &ih)
    ) {
        mime = "application/octet-stream"; // unreadable: a file card, not a broken picture
    }
    w.beginObject();
    w.key("name").value(file::baseName(src));
    w.key("file").value(name);
    w.key("mime").value(mime);
    if (iw > 0) {
        w.key("w").value(int64_t(iw));
        w.key("h").value(int64_t(ih));
    }
    w.endObject();
    return true;
}

} // namespace

std::string cleanPath(std::string_view in) {
    std::string path(in);
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    if (path.empty())
        return path;
    // The root part kept as it is: "/", "C:/", "//server".
    size_t rootLen = 0;
    if (path.size() >= 2 && path[1] == ':')
        rootLen = path.size() >= 3 && path[2] == '/' ? 3 : 2;
    else if (str::startsWith(path, "//"))
        rootLen = 2;
    else if (path[0] == '/')
        rootLen = 1;
    const bool                    absolute = rootLen > 0;
    std::vector<std::string_view> parts;
    std::string_view              rest = std::string_view(path).substr(rootLen);
    while (!rest.empty()) {
        const size_t           slash = rest.find('/');
        const std::string_view part  = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
            continue;
        }
        parts.push_back(part);
    }
    std::string out = path.substr(0, rootLen);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += '/';
        out.append(parts[i]);
    }
    if (out.empty())
        out = ".";
    return out;
}

namespace {

// The next candidate path in `text` from byte *i on, as written ("" when
// it's a web link or nothing); *i moves past it.
std::string nextPath(std::string_view text, size_t *i) {
    // The next token: up to a separator.
    size_t start = *i;
    while (start < text.size()) {
        size_t         j  = start;
        const uint32_t cp = utf8::decode(text, j);
        if (!splitsTokens(cp))
            break;
        start = j;
    }
    size_t end = start;
    while (end < text.size()) {
        size_t         j  = end;
        const uint32_t cp = utf8::decode(text, j);
        if (splitsTokens(cp))
            break;
        end = j;
    }
    *i                     = end;
    std::string_view token = text.substr(start, end - start);
    while (!token.empty() && std::strchr(".:!?", token.back()))
        token.remove_suffix(1);
    if (str::startsWith(token, "file://"))
        return localFileOf(token);
    if (token.find("://") != std::string_view::npos)
        return {}; // a web link
    return expandHome(token);
}

} // namespace

bool mayNameFiles(std::string_view text) {
    // mentionedFiles keeps only paths of a kind shown, whatever folders the
    // text names besides.
    for (size_t i = 0; i < text.size();)
        if (const std::string path = nextPath(text, &i); !path.empty() && shownKind(path))
            return true;
    return false;
}

std::vector<std::string> mentionedFiles(std::string_view text, std::string_view cwd) {
    std::vector<std::string> absolute, relative, dirs;
    for (size_t i = 0; i < text.size();) {
        std::string path = nextPath(text, &i);
        if (path.empty())
            continue;
        if (file::isAbsolute(path)) {
            if (file::isDir(path)) {
                dirs.push_back(cleanPath(path));
            } else if (shownKind(path)) {
                absolute.push_back(cleanPath(path));
                dirs.emplace_back(file::dirName(absolute.back())); // "…/x/a.svg and a.png"
            }
        } else if (shownKind(path)) {
            relative.push_back(std::move(path));
        }
    }
    if (!cwd.empty())
        dirs.emplace_back(cwd);

    std::vector<std::string>        out;
    std::unordered_set<std::string> seen;
    const auto                      add = [&](const std::string &path) {
        if (!isFile(path))
            return false;
        std::string abs = cleanPath(file::absolute(path));
        if (seen.insert(abs).second)
            out.push_back(std::move(abs));
        return true;
    };
    // In the order the text names them: absolute ones first is close enough —
    // answers name a folder, then what's in it.
    for (const std::string &p : absolute)
        add(p);
    for (const std::string &r : relative)
        for (const std::string &d : dirs)
            if (add(file::join(d, r)))
                break;
    return out;
}

std::string outputsFolder(const OutputContext &ctx) {
    return messageDir(ctx);
}

bool cachedOutputs(const OutputContext &ctx, std::vector<model::File> *files) {
    files->clear();
    if (ctx.convId.empty() || ctx.messageKey.empty() || dirs().cache.empty())
        return true; // nowhere to keep copies: none to make either
    const std::string dir = messageDir(ctx);
    std::string       indexJson;
    if (!file::readRange(file::join(dir, kIndex), 0, kMaxIndexBytes, &indexJson))
        return false;
    json::Document doc;
    if (!doc.parse(std::move(indexJson)))
        return true;
    const std::string idPrefix = str::concat({"out-", ctx.messageKey, "-"});
    int               i        = 0;
    for (json::Value e : doc.root()) {
        if (file::exists(file::join(dir, e["file"].str())))
            files->push_back(fileFor(e, dir, idPrefix, i));
        ++i;
    }
    return true;
}

void makeOutputs(std::string_view text, const OutputContext &ctx) {
    if (ctx.convId.empty() || ctx.messageKey.empty() || dirs().cache.empty())
        return;
    const std::vector<std::string> named = mentionedFiles(text, ctx.cwd);
    if (named.empty())
        return; // names no file (most answers): nothing to write down, cheap to look again
    const std::string        dir = messageDir(ctx);
    std::vector<std::string> made;
    for (const std::string &path : named) {
        file::Stat st;
        if (!file::stat(path, &st) || st.mtimeMicros < ctx.turnStart - kSlackMicros ||
            st.mtimeMicros > ctx.date + kSlackMicros)
            continue; // made before this turn, or changed since the answer
        if (st.size > kMaxFileBytes)
            continue;
        made.push_back(path);
        if (int(made.size()) == kMaxFiles)
            break;
    }
    file::makeDirs(dir);
    json::Writer w;
    w.beginArray();
    for (size_t i = 0; i < made.size(); ++i)
        copyInto(
            made[i], dir, str::concat({str::number(int64_t(i)), "-", file::baseName(made[i])}), w
        );
    w.endArray();
    file::writeAtomic(file::join(dir, kIndex), w.take());
}

std::string outputsDir(std::string_view convId) {
    return str::concat({dirs().cache, "/files/", safeName(convId)});
}

void clearOutputs(std::string_view convId) {
    if (!convId.empty() && !dirs().cache.empty())
        file::removeTree(outputsDir(convId));
}

void pruneOutputs(const std::vector<std::string> &keep) {
    if (dirs().cache.empty())
        return;
    std::unordered_set<std::string> kept;
    for (const std::string &id : keep)
        kept.insert(safeName(id));
    const std::string           root = dirs().cache + "/files";
    std::vector<file::DirEntry> entries;
    file::listDir(root, &entries);
    for (const file::DirEntry &e : entries)
        if (e.isDir && !kept.count(e.name))
            file::removeTree(file::join(root, e.name));
}

} // namespace claude
