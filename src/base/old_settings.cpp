// old_settings.h: the store's value and INI encodings, and
// the Linux store (an INI file). macOS and Windows have their own backends
// (old_settings_mac.mm, old_settings_win.cpp); their tests use the INI file
// too (old_settings.h).
#include "base/old_settings.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oldsettings {

// ── Value ───────────────────────────────────────────────────────────────────

std::string Value::text() const {
    switch (kind) {
    case Kind::String:
    case Kind::Bytes:
        return s;
    case Kind::Int:
        return str::number(n);
    case Kind::Bool:
        return n ? "true" : "false";
    case Kind::List:
        return list.size() == 1 ? list[0] : std::string();
    case Kind::None:
        break;
    }
    return {};
}

bool Value::toBool(bool def) const {
    switch (kind) {
    case Kind::Bool:
    case Kind::Int:
        return n != 0;
    case Kind::String:
    case Kind::Bytes: {
        // True unless empty, "0" or "false" (any case).
        const std::string l = str::asciiLower(str::trim(s));
        return !(l.empty() || l == "0" || l == "false");
    }
    default:
        return def;
    }
}

int64_t Value::toInt(int64_t def) const {
    switch (kind) {
    case Kind::Bool:
    case Kind::Int:
        return n;
    case Kind::String: {
        const std::string t(str::trim(s));
        char             *end = nullptr;
        const long long   v   = std::strtoll(t.c_str(), &end, 10);
        return !t.empty() && end && *end == 0 ? int64_t(v) : def;
    }
    default:
        return def;
    }
}

std::vector<std::string> Value::toList() const {
    if (kind == Kind::List)
        return list;
    if (kind == Kind::String || kind == Kind::Bytes)
        return {s};
    return {};
}

// ── stringToVariant / variantToString ───────────────────────────────────────

namespace {

// The code points of a UTF-8 string as Latin-1 bytes.
std::string latin1(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = utf8::decode(s, i);
        out.push_back(char(cp <= 0xFF ? cp : '?'));
    }
    return out;
}

} // namespace

Value decodeString(std::string s) {
    Value v;
    if (s.size() > 1 && s[0] == '@') {
        if (s.back() == ')') {
            if (str::startsWith(s, "@ByteArray(")) {
                v.kind = Value::Kind::Bytes;
                v.s    = latin1(std::string_view(s).substr(11, s.size() - 12));
                return v;
            }
            if (str::startsWith(s, "@String(")) {
                v.kind = Value::Kind::String;
                v.s    = s.substr(8, s.size() - 9);
                return v;
            }
            if (s == "@Invalid()")
                return v;
            // @Variant / @DateTime / @Rect …: nothing earlier versions
            // stored that this one reads; keep the text.
        }
        if (s[1] == '@')
            s.erase(0, 1);
    }
    v.kind = Value::Kind::String;
    v.s    = std::move(s);
    return v;
}

std::string encodeString(std::string_view s) {
    return !s.empty() && s[0] == '@' ? str::concat({"@", s}) : std::string(s);
}

// ── INI ─────────────────────────────────────────────────────────────────────

namespace {

using str::hexDigit;

bool isLetterOrDigit(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// iniUnescapedKey: '\' is the '/' separator, %XX and %UXXXX code points.
std::string unescapeKey(std::string_view k) {
    std::string out;
    for (size_t i = 0; i < k.size();) {
        const char c = k[i];
        if (c == '\\') {
            out.push_back('/');
            ++i;
            continue;
        }
        if (c == '%' && i + 1 < k.size()) {
            const bool   wide   = k[i + 1] == 'U';
            const size_t first  = i + 1 + (wide ? 1 : 0);
            const size_t digits = wide ? 4 : 2;
            uint32_t     cp     = 0;
            bool         ok     = first + digits <= k.size();
            for (size_t d = 0; ok && d < digits; ++d) {
                const int h = hexDigit(k[first + d]);
                ok          = h >= 0;
                cp          = cp * 16 + uint32_t(h);
            }
            if (ok) {
                utf8::append(out, cp);
                i = first + digits;
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// iniEscapedKey.
std::string escapeKey(std::string_view k) {
    const char *const hex = str::kHexUpper;
    std::string       out;
    for (size_t i = 0; i < k.size();) {
        const unsigned char c = static_cast<unsigned char>(k[i]);
        if (c == '/' || isLetterOrDigit(c) || c == '_' || c == '-' || c == '.') {
            out.push_back(c == '/' ? '\\' : char(c));
            ++i;
            continue;
        }
        uint32_t cp = utf8::decode(k, i);
        if (cp > 0xFFFF)
            cp = utf8::kReplacement; // UTF-16 would need two surrogates
        if (cp <= 0xFF) {
            out.push_back('%');
            out.push_back(hex[cp >> 4]);
            out.push_back(hex[cp & 15]);
        } else {
            out += "%U";
            for (int s = 12; s >= 0; s -= 4)
                out.push_back(hex[(cp >> s) & 15]);
        }
    }
    return out;
}

// "\x" and the lower-case hex digits of c, no leading zeros.
void hexEscape(std::string &out, uint32_t c) {
    out += "\\x";
    if (c >= 16)
        out.push_back(str::kHexLower[c >> 4]);
    out.push_back(str::kHexLower[c & 15]);
}

// A string value escaped for the INI file (UTF-8 in and out).
std::string escapeValue(std::string_view s) {
    std::string out;
    bool        quotes = false, escapeDigit = false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == ';' || c == ',' || c == '=')
            quotes = true;
        if (escapeDigit && hexDigit(char(c)) >= 0) {
            hexEscape(out, c);
            continue;
        }
        escapeDigit = false;
        switch (c) {
        case '\0':
            out += "\\0";
            escapeDigit = true;
            break;
        case '\a':
            out += "\\a";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\v':
            out += "\\v";
            break;
        case '"':
        case '\\':
            out.push_back('\\');
            out.push_back(char(c));
            break;
        default:
            if (c <= 0x1F) {
                hexEscape(out, c);
                escapeDigit = true;
            } else {
                out.push_back(char(c));
            }
        }
    }
    if (quotes || (!out.empty() && (out.front() == ' ' || out.back() == ' ')))
        out = str::concat({"\"", out, "\""});
    return out;
}

void chopTrailingSpaces(std::string &s, size_t limit) {
    while (s.size() > limit && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

// iniUnescapedStringList: a value, or a comma-separated list of them.
Value unescapeValue(std::string_view str) {
    static const char escapes[][2] = {
        {'a', '\a'},
        {'b', '\b'},
        {'f', '\f'},
        {'n', '\n'},
        {'r', '\r'},
        {'t', '\t'},
        {'v', '\v'},
        {'"', '"'},
        {'?', '?'},
        {'\'', '\''},
        {'\\', '\\'},
    };
    std::vector<std::string> items;
    std::string              cur;
    bool                     isList = false, inQuotes = false, quoted = false;
    size_t                   i = 0, chopLimit = 0;
    const auto               skipSpaces = [&] {
        while (i < str.size() && (str[i] == ' ' || str[i] == '\t'))
            ++i;
        chopLimit = cur.size();
    };
    skipSpaces();
    while (i < str.size()) {
        const char c = str[i];
        if (c == '\\') {
            if (++i >= str.size())
                break;
            const char e     = str[i++];
            bool       known = false;
            for (const auto &x : escapes)
                if (e == x[0]) {
                    cur.push_back(x[1]);
                    known = true;
                    break;
                }
            if (!known) {
                if (e == 'x' || (e >= '0' && e <= '7')) {
                    const int base = e == 'x' ? 16 : 8;
                    uint32_t  v    = e == 'x' ? 0 : uint32_t(e - '0');
                    int       n    = e == 'x' ? 0 : 1;
                    while (i < str.size()) {
                        const int d = base == 16                       ? hexDigit(str[i])
                                      : str[i] >= '0' && str[i] <= '7' ? str[i] - '0'
                                                                       : -1;
                        if (d < 0)
                            break;
                        v = v * uint32_t(base) + uint32_t(d);
                        ++i;
                        ++n;
                    }
                    if (n)
                        utf8::append(cur, v & 0xFFFF);
                } else if (
                    (e == '\n' || e == '\r') && i < str.size() &&
                    (str[i] == '\n' || str[i] == '\r') && str[i] != e
                ) {
                    ++i;
                }
            }
            chopLimit = cur.size();
        } else if (c == '"') {
            ++i;
            quoted   = true;
            inQuotes = !inQuotes;
            if (!inQuotes)
                skipSpaces();
        } else if (c == ',' && !inQuotes) {
            if (!quoted)
                chopTrailingSpaces(cur, chopLimit);
            isList = true;
            items.push_back(std::move(cur));
            cur.clear();
            quoted = false;
            ++i;
            skipSpaces();
        } else {
            size_t j = i + 1;
            while (j < str.size() && str[j] != '\\' && str[j] != '"' && str[j] != ',')
                ++j;
            cur.append(str.substr(i, j - i));
            i = j;
        }
    }
    if (!quoted)
        chopTrailingSpaces(cur, chopLimit);
    if (!isList)
        return decodeString(std::move(cur));
    items.push_back(std::move(cur));
    Value v;
    v.kind = Value::Kind::List;
    for (std::string &it : items)
        v.list.push_back(decodeString(std::move(it)).text());
    return v;
}

// One logical line of an INI file (a value never spans lines: newlines in
// values are escaped).
struct Line {
    size_t           begin = 0, end = 0; // [begin, end) in the text, without the EOL
    std::string_view text;               // trimmed
};

std::vector<Line> splitLines(std::string_view t) {
    std::vector<Line> out;
    size_t            i = 0;
    while (i < t.size()) {
        size_t j = i;
        while (j < t.size() && t[j] != '\n' && t[j] != '\r')
            ++j;
        out.push_back({i, j, str::trim(t.substr(i, j - i))});
        i = j;
        if (i < t.size() && t[i] == '\r')
            ++i;
        if (i < t.size() && t[i] == '\n')
            ++i;
    }
    return out;
}

// A section header's key prefix: "" for [General], else "name/".
bool sectionOf(std::string_view line, std::string *prefix) {
    if (line.empty() || line[0] != '[')
        return false;
    size_t                 close = line.find(']');
    const std::string_view name  = str::trim(
        line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)
    );
    const std::string lower = str::asciiLower(name);
    if (lower == "general")
        prefix->clear();
    else if (lower == "%general")
        *prefix = str::concat({name.substr(1), "/"});
    else
        *prefix = unescapeKey(name) + "/";
    return true;
}

// The "key=value" split of a line; false for comments. As readIniLine, a
// ';' outside quotes ends the line.
bool keyValue(std::string_view line, std::string_view *k, std::string_view *v) {
    if (line.empty() || line[0] == ';')
        return false;
    bool   quotes = false;
    size_t eq     = std::string_view::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') {
            ++i;
        } else if (line[i] == '"') {
            quotes = !quotes;
        } else if (line[i] == '=' && !quotes && eq == std::string_view::npos) {
            eq = i;
        } else if (line[i] == ';' && !quotes) {
            line = line.substr(0, i);
            break;
        }
    }
    if (eq == std::string_view::npos)
        return false;
    *k = str::trim(line.substr(0, eq));
    *v = line.substr(eq + 1);
    return true;
}

} // namespace

Map parseIni(std::string_view text) {
    if (str::startsWith(text, "\xEF\xBB\xBF"))
        text.remove_prefix(3);
    Map              out;
    std::string      section;
    std::string_view k, v;
    for (const Line &l : splitLines(text)) {
        if (sectionOf(l.text, &section))
            continue;
        if (keyValue(l.text, &k, &v))
            out[section + unescapeKey(k)] = unescapeValue(v);
    }
    return out;
}

std::string setIniValue(std::string_view text, std::string_view key, const std::string *value) {
    const size_t      slash = key.find('/');
    const std::string section =
        slash == std::string_view::npos ? std::string() : std::string(key.substr(0, slash + 1));
    const std::string_view sub = slash == std::string_view::npos ? key : key.substr(slash + 1);
    const std::string      line =
        value ? str::concat({escapeKey(sub), "=", escapeValue(encodeString(*value))})
              : std::string();

    // Replace (or drop) the key's line; remember where its section ends.
    // Lines before the first header are [General].
    std::string      cur;
    bool             inSection  = section.empty();
    size_t           sectionEnd = std::string_view::npos; // after its last non-blank line
    std::string      out;
    size_t           copied = 0;
    std::string_view k, v;
    for (const Line &l : splitLines(text)) {
        if (sectionOf(l.text, &cur)) {
            inSection = cur == section;
            if (inSection)
                sectionEnd = l.end;
            continue;
        }
        if (!inSection)
            continue;
        if (keyValue(l.text, &k, &v) && unescapeKey(k) == sub) {
            out.append(text.substr(copied, l.begin - copied));
            copied = l.end;
            if (value) // the first one becomes the new value: done
                return str::concat({out, line, text.substr(copied)});
            if (copied < text.size()) // the line goes, with its EOL
                copied += text.substr(copied, 2) == "\r\n" ? 2 : 1;
            continue;
        }
        if (!l.text.empty())
            sectionEnd = l.end;
    }
    out.append(text.substr(copied));
    if (!value)
        return out;

    // A new key (nothing was replaced, so out == text): at the end of its
    // section, or in a new section at the end of the file.
    if (sectionEnd != std::string_view::npos)
        return str::concat({text.substr(0, sectionEnd), "\n", line, text.substr(sectionEnd)});
    std::string header = "[General]";
    if (!section.empty()) {
        const std::string name = escapeKey(section.substr(0, section.size() - 1));
        header = str::asciiLower(name) == "general" ? "[%General]" : str::concat({"[", name, "]"});
    }
    if (!out.empty() && out.back() != '\n')
        out.push_back('\n');
    if (!out.empty())
        out.push_back('\n');
    return str::concat({out, header, "\n", line, "\n"});
}

// ── Registry values ─────────────────────────────────────────────────────────

namespace {

// UTF-16LE (up to the first NUL when `stopAtNul`) to UTF-8.
std::string fromUtf16(std::string_view b, size_t *pos, bool stopAtNul) {
    std::string out;
    size_t      i = *pos;
    while (i + 1 < b.size()) {
        uint32_t u = uint8_t(b[i]) | uint32_t(uint8_t(b[i + 1])) << 8;
        i += 2;
        if (u == 0 && stopAtNul)
            break;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < b.size()) {
            const uint32_t lo = uint8_t(b[i]) | uint32_t(uint8_t(b[i + 1])) << 8;
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        utf8::append(out, u);
    }
    *pos = i;
    return out;
}

} // namespace

Value decodeRegistry(uint32_t type, std::string_view bytes) {
    enum : uint32_t {
        kSz       = 1,
        kExpandSz = 2,
        kBinary   = 3,
        kDword    = 4,
        kDwordBe  = 5,
        kMultiSz  = 7,
        kQword    = 11
    };
    Value  v;
    size_t pos = 0;
    switch (type) {
    case kSz:
    case kExpandSz:
        return decodeString(fromUtf16(bytes, &pos, true));
    case 0: // REG_NONE
    case kBinary:
        return decodeString(fromUtf16(bytes, &pos, false));
    case kMultiSz:
        v.kind = Value::Kind::List;
        while (pos + 1 < bytes.size()) {
            std::string s = fromUtf16(bytes, &pos, true);
            if (s.empty())
                break;
            v.list.push_back(decodeString(std::move(s)).text());
        }
        return v;
    case kDword:
    case kDwordBe:
    case kQword: {
        const size_t n = type == kQword ? 8 : 4;
        if (bytes.size() < n)
            return v;
        uint64_t u = 0;
        for (size_t i = 0; i < n; ++i)
            u |= uint64_t(uint8_t(bytes[i])) << (8 * i);
        v.kind = Value::Kind::Int;
        v.n    = n == 4 ? int64_t(int32_t(uint32_t(u))) : int64_t(u);
        return v;
    }
    default:
        return v;
    }
}

// ── Linux (and every OS's tests): the INI file ──────────────────────────────

std::string iniPath(std::string_view app) {
    std::string dir = base::env("XDG_CONFIG_HOME");
    if (dir.empty() || !file::isAbsolute(dir)) {
        const std::string home = base::env("HOME");
        if (home.empty())
            return {};
        dir = file::join(home, ".config");
    }
    return file::join(dir, str::concat({"msga/", app, ".conf"}));
}

namespace {

#ifndef _WIN32
// Earlier versions sync the file under a lock file "<file>.lock" (pid, app
// name, host) and may be running, so take the same lock around a rewrite. A
// lock whose process is gone, or older than 30 s (stale), is broken.
class IniLock {
public:
    explicit IniLock(const std::string &file) : _path(file + ".lock") {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int fd = ::open(_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd >= 0) {
                char host[256] = {};
                gethostname(host, sizeof host - 1);
                const std::string body =
                    str::concat({str::number(getpid()), "\nmsga\n", host, "\n"});
                [[maybe_unused]] const ssize_t w = ::write(fd, body.data(), body.size());
                ::close(fd);
                _held = true;
                return;
            }
            if (errno != EEXIST)
                return; // a read-only directory: go on without it
            std::string other;
            struct stat st{};
            const bool  stale =
                (file::readAll(_path, &other) && std::atol(other.c_str()) > 0 &&
                 kill(pid_t(std::atol(other.c_str())), 0) != 0 && errno == ESRCH) ||
                (::stat(_path.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime > 30);
            if (stale) {
                ::unlink(_path.c_str());
                continue;
            }
            usleep(20 * 1000);
        }
    }
    ~IniLock() {
        if (_held)
            ::unlink(_path.c_str());
    }
    IniLock(const IniLock &)            = delete;
    IniLock &operator=(const IniLock &) = delete;

private:
    std::string _path;
    bool        _held = false;
};
#endif

bool rewrite(std::string_view key, const std::string *value, std::string_view app) {
    const std::string path = iniPath(app);
    if (path.empty())
        return false;
#ifndef _WIN32
    IniLock lock(path);
#endif
    std::string text;
    const bool  existed = file::readAll(path, &text);
    if (!existed && !value)
        return true;
    const std::string next = setIniValue(text, key, value);
    if (existed && next == text)
        return true;
    // Keep the file's permissions (earlier versions wrote it 0644 & ~umask); a new one
    // holds credentials, so owner-only.
    int mode = 0600;
#ifndef _WIN32
    struct stat st{};
    if (existed && ::stat(path.c_str(), &st) == 0)
        mode = int(st.st_mode & 0777);
#endif
    return file::writeAtomic(path, next, mode);
}

Map iniLoad(std::string_view app) {
    std::string       text;
    const std::string path = iniPath(app);
    if (path.empty() || !file::readAll(path, &text))
        return {};
    return parseIni(text);
}

// get() is asked for one key after another (the importer at start-up): the
// file is read each time, but parsed again only when its bytes changed.
Value iniGet(std::string_view key, std::string_view app) {
    static std::mutex  mutex;
    static std::string lastPath, lastText;
    static Map         lastMap;
    std::string        text;
    const std::string  path = iniPath(app);
    if (path.empty() || !file::readAll(path, &text))
        return {};
    std::lock_guard<std::mutex> lock(mutex);
    if (path != lastPath || text != lastText) {
        lastMap  = parseIni(text);
        lastPath = path;
        lastText = std::move(text);
    }
    const auto it = lastMap.find(key);
    return it == lastMap.end() ? Value() : it->second;
}

} // namespace

#if defined(_WIN32) || defined(__APPLE__)
namespace native { // old_settings_mac.mm, old_settings_win.cpp
Map   load(std::string_view app);
Value get(std::string_view key, std::string_view app);
bool  write(std::string_view key, std::string_view value, std::string_view app);
bool  remove(std::string_view key, std::string_view app);
} // namespace native

namespace detail {
bool nativeInTests = false;
}

namespace {
// Tests keep to the INI file under their temporary XDG_CONFIG_HOME: the
// native store is the user's (CFPreferences, the registry), shared by every
// test process at once and outliving them. So does a demo run
// (base::isolatedStorage()), under its throwaway XDG_CONFIG_HOME.
bool nativeStore() {
    return detail::nativeInTests || !base::isolatedStorage();
}
} // namespace
#endif

Map load(std::string_view app) {
#if defined(_WIN32) || defined(__APPLE__)
    if (nativeStore())
        return native::load(app);
#endif
    return iniLoad(app);
}

Value get(std::string_view key, std::string_view app) {
#if defined(_WIN32) || defined(__APPLE__)
    if (nativeStore())
        return native::get(key, app);
#endif
    return iniGet(key, app);
}

bool write(std::string_view key, std::string_view value, std::string_view app) {
#if defined(_WIN32) || defined(__APPLE__)
    if (nativeStore())
        return native::write(key, value, app);
#endif
    const std::string v(value);
    return rewrite(key, &v, app);
}

bool remove(std::string_view key, std::string_view app) {
#if defined(_WIN32) || defined(__APPLE__)
    if (nativeStore())
        return native::remove(key, app);
#endif
    return rewrite(key, nullptr, app);
}

} // namespace oldsettings
