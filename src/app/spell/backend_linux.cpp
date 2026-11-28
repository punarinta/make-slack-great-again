// Linux and the BSDs: the vendored, trimmed Hunspell (src/third_party/hunspell)
// over the dictionaries the system's packages installed. A port build: the
// same, over the dictionaries installed with the app (PORT_PATH_RESOURCES's
// hunspell/), with msga's own Unicode tables for case. The words someone adds live
// in msga's own list (one per line, <dataDir>/spelling/words.txt,
// app/identity.h) and are added to every dictionary on load.
#include "app/spell/codepages.h"
#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#include "app/identity.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"
#include "plat/plat.h"

#include <hunspell.hxx>

#include <algorithm>
#include <cstdlib>
#if defined(MSGA_PORT)
#include "port/port.h"
#include "text/unicode.h"
#else
#include <locale.h>
#include <wctype.h>
#endif

// Unicode case and letter lookups for the trimmed Hunspell
// (HUNSPELL_HOST_UNICODE in csutil.cxx): the C library's tables are in the
// binary already, Hunspell's own utf_tbl would add 384 KB. Hunspell asks
// about UTF-16 units. musl's wctype is Unicode whatever the locale; glibc's
// "C" locale knows ASCII only, so it is asked in C.UTF-8 (built in since 2.35).
#if defined(MSGA_PORT)
// A port's C library may know no Unicode (PEOS's has no wctype.h): msga's
// case folding (base/utf8.h: Latin, Greek, Cyrillic, Armenian; the scripts
// whose dictionaries have case) and HarfBuzz's general categories.
unsigned short hunspell_host_tolower(unsigned short c) {
    return static_cast<unsigned short>(utf8::foldCase(c));
}
unsigned short hunspell_host_toupper(unsigned short c) {
    if (utf8::isUpper(c))
        return c;
    // foldCase's pairs are 1:1, a fixed distance apart: the capital is the
    // one that folds to c.
    for (const int d : {32, 80, 48, 1})
        if (c >= d && utf8::foldCase(uint32_t(c - d)) == c)
            return static_cast<unsigned short>(c - d);
    if (c == 0xFF)
        return 0x178;
    return c;
}
int hunspell_host_isalpha(unsigned short c) {
    return text::uni::isLetter(c) ? 1 : 0;
}
#else
namespace {
#if defined(__GLIBC__)
locale_t utf8Ctype() {
    static const locale_t l = newlocale(LC_CTYPE_MASK, "C.UTF-8", locale_t(nullptr));
    return l;
}
#endif
} // namespace

unsigned short hunspell_host_toupper(unsigned short c) {
#if defined(__GLIBC__)
    if (const locale_t l = utf8Ctype())
        return static_cast<unsigned short>(towupper_l(c, l));
#endif
    return static_cast<unsigned short>(towupper(c));
}
unsigned short hunspell_host_tolower(unsigned short c) {
#if defined(__GLIBC__)
    if (const locale_t l = utf8Ctype())
        return static_cast<unsigned short>(towlower_l(c, l));
#endif
    return static_cast<unsigned short>(towlower(c));
}
int hunspell_host_isalpha(unsigned short c) {
#if defined(__GLIBC__)
    if (const locale_t l = utf8Ctype())
        return iswalpha_l(c, l) ? 1 : 0;
#endif
    return iswalpha(c) ? 1 : 0;
}
#endif

namespace spell {

namespace {

struct DictionaryFiles {
    std::string code, aff, dic;
};

#if defined(MSGA_PORT)
// A folder the port names ("" when it has none).
std::string portPath(int which) {
    char      buf[1024];
    const int n = port_path_get(which, buf, sizeof buf);
    return n > 0 && size_t(n) < sizeof buf ? std::string(buf, size_t(n)) : std::string();
}
#endif

// Hunspell's own search order, less the office-suite folders: $DICPATH, the
// user's data dir, then the system's. A port: $DICPATH, then those the app
// came with.
std::vector<std::string> dictionaryDirs() {
    std::vector<std::string> dirs;
    const std::string        dicpath = base::env("DICPATH");
    str::Splitter            parts(dicpath, ':');
    for (std::string_view d; parts.next(&d);)
        if (!d.empty())
            dirs.emplace_back(d);
#if defined(MSGA_PORT)
    if (const std::string r = portPath(PORT_PATH_RESOURCES); !r.empty())
        dirs.push_back(r + "/hunspell");
    return dirs;
#endif
    std::string data = base::env("XDG_DATA_HOME");
    if (const std::string home = base::homeDir(); data.empty() && !home.empty())
        data = home + "/.local/share";
    if (!data.empty())
        dirs.push_back(data + "/hunspell");
    for (const char *d :
         {"/usr/local/share/hunspell",
          "/usr/share/hunspell",
          "/usr/share/myspell",
          "/usr/share/myspell/dicts"})
        dirs.emplace_back(d);
    return dirs;
}

// Language code (the files' base name, "en_US") → files, by name; the first
// directory that has a language wins. A .dic without its .aff (hyphenation
// patterns, thesauri) is no spelling dictionary.
std::vector<DictionaryFiles> findDictionaries() {
    std::vector<DictionaryFiles> found;
    for (const std::string &dir : dictionaryDirs()) {
        std::vector<file::DirEntry> entries;
        if (!file::listDir(dir, &entries))
            continue;
        for (const file::DirEntry &e : entries) {
            if (e.isDir || file::extension(e.name) != "dic")
                continue;
            const std::string code = e.name.substr(0, e.name.size() - 4);
            if (code.empty() || str::startsWith(code, "hyph_"))
                continue;
            if (std::any_of(found.begin(), found.end(), [&](const DictionaryFiles &f) {
                    return f.code == code;
                }))
                continue;
            const std::string aff = file::join(dir, code + ".aff");
            if (file::exists(aff))
                found.push_back({code, aff, file::join(dir, e.name)});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.code < b.code;
    });
    return found;
}

// The character set a dictionary declares (its .aff SET line, which Hunspell
// reports): words go in and suggestions come out in it. Hunspell treats a set
// it doesn't know as ISO-8859-1; so does this.
class Charset {
public:
    explicit Charset(const std::string &set) {
        std::string norm;
        for (const char c : set)
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                norm += c;
            else if (c >= 'A' && c <= 'Z')
                norm += char(c - 'A' + 'a');
        if (norm == "utf8") {
            _kind = Kind::Utf8;
            return;
        }
        for (const auto &a : kCodepageAliases)
            if (norm == a.alias)
                norm = a.name;
        for (const auto &cp : kCodepages)
            if (norm == cp.name) {
                _kind = Kind::Table;
                _high = cp.high;
                return;
            }
    }

    // False when the word has a character this set can't hold: then the
    // dictionary can't have the word either.
    bool encode(std::string_view word, std::string &out) const {
        if (_kind == Kind::Utf8) {
            out.assign(word);
            return true;
        }
        out.clear();
        for (size_t i = 0; i < word.size();) {
            const uint32_t u = utf8::decode(word, i);
            if (u < 0x80 || (_kind == Kind::Latin1 && u < 0x100)) {
                out += char(u);
                continue;
            }
            if (_kind == Kind::Latin1)
                return false;
            int b = -1;
            for (int k = 0; k < 128 && b < 0; ++k)
                if (_high[k] == u && u != 0xFFFD)
                    b = k;
            if (b < 0)
                return false;
            out += char(0x80 + b);
        }
        return true;
    }

    std::string decode(const std::string &s) const {
        if (_kind == Kind::Utf8)
            return s;
        std::string out;
        for (const char c : s) {
            const auto b = static_cast<unsigned char>(c);
            utf8::append(out, b < 0x80 ? b : _kind == Kind::Latin1 ? b : _high[b - 0x80]);
        }
        return out;
    }

private:
    enum class Kind { Utf8, Latin1, Table };
    Kind            _kind = Kind::Latin1;
    const char16_t *_high = nullptr;
};

class HunspellBackend : public Backend {
public:
    explicit HunspellBackend(std::string wordsPath) : _wordsPath(std::move(wordsPath)) {}

    bool loadsOffThread() const override { return true; }
    bool threadSafe() const override { return true; }

    bool load(const std::vector<std::string> &codes) override {
        const auto files = findDictionaries();
        for (const std::string &code : codes) {
            const auto it = std::find_if(files.begin(), files.end(), [&](const auto &f) {
                return f.code == code;
            });
            if (it == files.end())
                continue;
            auto    hs = std::make_unique<Hunspell>(it->aff.c_str(), it->dic.c_str());
            Charset cs(hs->get_dict_encoding());
            _dicts.push_back({std::move(hs), cs});
        }
        if (_dicts.empty())
            return false;
        std::string words;
        if (file::readAll(_wordsPath, &words)) {
            str::Splitter lines(words, '\n');
            for (std::string_view line; lines.next(&line);)
                if (const std::string_view w = str::trim(line); !w.empty())
                    addToAll(w);
        }
        return true;
    }

    bool check(std::string_view word) override {
        std::string enc;
        for (const Dictionary &d : _dicts)
            if (d.charset.encode(word, enc) && d.hs->spell(enc))
                return true;
        return false;
    }

    std::vector<std::string> suggest(std::string_view word, int max) override {
        std::vector<std::string> out;
        std::string              enc;
        for (const Dictionary &d : _dicts) {
            if (!d.charset.encode(word, enc))
                continue;
            for (const std::string &s : d.hs->suggest(enc)) {
                std::string u = d.charset.decode(s);
                if (std::find(out.begin(), out.end(), u) == out.end())
                    out.push_back(std::move(u));
                if (int(out.size()) >= max)
                    return out;
            }
        }
        return out;
    }

    void addToDictionary(std::string_view word) override {
        addToAll(word);
        std::string all;
        file::readAll(_wordsPath, &all);
        all.append(word).push_back('\n');
        file::makeDirs(file::dirName(_wordsPath));
        file::writeAtomic(_wordsPath, all);
    }

private:
    void addToAll(std::string_view word) {
        std::string enc;
        for (const Dictionary &d : _dicts)
            if (d.charset.encode(word, enc))
                d.hs->add(enc);
    }

    struct Dictionary {
        std::unique_ptr<Hunspell> hs;
        Charset                   charset;
    };
    std::vector<Dictionary> _dicts;
    std::string             _wordsPath;
};

#if !defined(MSGA_PORT)
// ID and ID_LIKE of /etc/os-release, lower case: " ubuntu debian".
std::string osFamily() {
    std::string text, ids;
    if (!file::readAll("/etc/os-release", &text))
        return ids;
    str::Splitter lines(text, '\n');
    for (std::string_view line; lines.next(&line);)
        if (str::startsWith(line, "ID=") || str::startsWith(line, "ID_LIKE=")) {
            ids += ' ';
            for (char c : line.substr(line.find('=') + 1))
                if (c != '"')
                    ids += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
    return ids;
}
#endif

} // namespace

std::unique_ptr<Backend> createPlatformBackend(plat::App &app) {
    const std::string data = identity::dataDir(app);
    return std::make_unique<HunspellBackend>(
        data.empty() ? std::string() : file::join(data, "spelling/words.txt")
    );
}

namespace detail {

bool listsOffThread() {
    return true;
}

std::vector<Language> availableLanguages() {
    std::vector<Language> out;
    for (const DictionaryFiles &f : findDictionaries())
        out.push_back({f.code, languageName(f.code)});
    return out;
}

std::string dictionaryPackageHint(const std::vector<std::string> &preferred) {
#if defined(MSGA_PORT)
    (void)preferred; // no package manager behind a port: nothing to name
    return {};
#else
    // The system language's package, named the way the distribution does.
    const std::string sys  = preferred.empty() ? std::string() : localeCode(preferred[0]);
    const std::string lang = sys.substr(0, sys.find('_'));
    const std::string terr =
        sys.find('_') == std::string::npos ? std::string() : sys.substr(sys.find('_') + 1);
    if (lang.empty())
        return "hunspell-en-us";
    const std::string os  = osFamily();
    const auto        has = [&](const char *id) { return os.find(id) != std::string::npos; };
    if (has("debian") || has("ubuntu")) {
        // Debian splits English and German by country: hunspell-en-us, hunspell-de-de.
        if ((lang == "en" || lang == "de") && !terr.empty())
            return str::concat({"hunspell-", lang, "-", str::asciiLower(terr)});
        return "hunspell-" + lang;
    }
    if (has("fedora") || has("rhel"))
        return lang == "en" ? "hunspell-en-US" : "hunspell-" + lang;
    if (has("arch"))
        return lang == "en" ? "hunspell-en_us" : "hunspell-" + lang;
    if (has("suse"))
        return str::concat({"myspell-", lang, "_", terr.empty() ? str::asciiUpper(lang) : terr});
    return "hunspell-" + lang;
#endif
}

} // namespace detail

} // namespace spell
