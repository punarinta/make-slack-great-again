// Composer spell checking (Settings → Appearance → Composer → "Check
// spelling"): the OS's own checker where there is one,
//   • Linux   — the vendored, trimmed Hunspell (src/third_party/hunspell) over
//               the dictionaries the distribution installed (/usr/share/hunspell,
//               …/myspell): a static binary can't dlopen Enchant's providers;
//   • macOS   — NSSpellChecker: the system's languages and learned words;
//   • Windows — ISpellChecker (Windows 8+), one checker per language.
// A word is right when any of the chosen languages knows it.
//
// Off by default; while off there is no backend and no dictionary in memory.
// Nothing runs on the UI thread that could take long: dictionaries load on a
// worker (30–50 ms and 8–27 MB apiece for Hunspell), a text is split into
// words and checked on a worker where the backend allows it (Hunspell) and in
// small slices between events where it doesn't (the macOS and Windows
// checkers are main-thread objects), and suggestions (15–120 ms) are made on
// request only — a right-click — on the same terms.
#pragma once

#include "base/observers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace spell {

// A spelling language this machine has: `code` is the backend's own id
// ("en_US" for Hunspell and AppKit, "en-US" on Windows), `name` is for
// people: "American English (en_US)".
struct Language {
    std::string code, name;
};

// A [start, start + length) byte range of composer text.
struct Span {
    uint32_t start = 0, length = 0;
    uint32_t end() const { return start + length; }
    bool     operator==(const Span &o) const { return start == o.start && length == o.length; }
};

// ── Words (spell_words.cpp, pure) ──────────────────────────────────────────
// The words of `text` (UTF-8, '\n' between lines) a spell checker should look
// at, in order. Never a word to check:
//   • ``` fenced code ``` (across lines) and `inline code`;
//   • URLs (scheme:// or www.), Slack's <…> tokens, :emoji: shortcodes;
//   • whitespace-separated chunks holding an '@' (mentions typed as text,
//     e-mail addresses), starting with '#' (channels) or '/' (commands,
//     paths), holding a '\' or a dot between two letters (files, hosts);
//   • words with a digit or an inner '_' (identifiers), ALL-CAPS and camelCase
//     words (acronyms, code), single letters and anything over 64 characters;
//   • the `excluded` ranges (mention pills), read as whitespace.
// A word is letters and combining marks, with apostrophes (' and ’) kept
// between letters ("don't") and markdown markers around it dropped (*bold*,
// _it_, ~gone~).
std::vector<Span> checkableWords(std::string_view text, const std::vector<Span> &excluded = {});
// The word around byte `pos` (inside it or at either end), with the same
// letters-and-apostrophes rule; {pos, 0} when there is none.
Span              wordAt(std::string_view text, uint32_t pos);

// "American English (en_US)": the language's own name for itself — readable to
// its speakers whatever the app language — and the code that tells variants
// apart ("de_DE", "de_DE_frami"); the bare code for one it doesn't know.
std::string languageName(std::string_view code);
// A language's English ("German") and own ("Deutsch") name by its bare
// ISO 639 code ("de"); null for one the table doesn't have.
const char *englishName(std::string_view code);
const char *nativeName(std::string_view code);

// ── The platform checker (backend_{linux,mac,win}.cpp) ──────────────────────
class Backend {
public:
    virtual ~Backend()                                       = default;
    // Gets ready to check `codes` (on a worker thread when loadsOffThread()).
    // False when none of them could be loaded.
    virtual bool load(const std::vector<std::string> &codes) = 0;
    virtual bool loadsOffThread() const { return false; }
    // check / suggest may run on worker threads (one at a time: the Checker
    // serialises them); otherwise they run on the UI thread.
    virtual bool threadSafe() const { return false; }
    virtual bool check(std::string_view word) = 0;
    // check() for many words at once: (*right)[i] for words[i]. The default
    // asks check() per word; a backend whose every call is a round trip
    // (macOS, Windows) checks them in one go.
    virtual void checkAll(const std::vector<std::string> &words, std::vector<uint8_t> *right);
    virtual std::vector<std::string> suggest(std::string_view word, int max) = 0;
    // Into the system's user dictionary (macOS, Windows) or msga's own word
    // list (Linux).
    virtual void                     addToDictionary(std::string_view word)  = 0;
};

std::unique_ptr<Backend> createPlatformBackend(plat::App &app);

// What Settings lists: the languages this machine can check, and — when there
// are none — the package with the system language's dictionary under the
// distribution's name for it ("hunspell-en-us"; "" where the OS always ships
// its languages). Answers later, on the UI thread (Linux scans directories on
// a worker).
struct Available {
    std::vector<Language> languages;
    std::string           packageHint;
};
void listLanguages(plat::App &app, std::function<void(Available)> done);

// ── The checker ─────────────────────────────────────────────────────────────
class Checker {
public:
    static Checker &instance();

    // Settings → Check spelling and its languages (empty = the system
    // language's, defaultLanguages). Turning it on loads them in the
    // background; changed() follows once words can be checked. A no-op when
    // nothing changed.
    void configure(plat::App &app, bool enabled, const std::vector<std::string> &languages);
    bool enabled() const { return _enabled; }
    const std::vector<std::string> &languages() const { return _languages; }
    // Enabled with its dictionaries loaded: words get checked.
    bool                            active() const { return _backend != nullptr; }

    // The misspelled words of `text`, answered later on the UI thread (never
    // inside this call); nothing while inactive. Ignored words are right.
    void check(
        std::string text, std::vector<Span> excluded, std::function<void(std::vector<Span>)> done
    );
    // Up to `max` suggestions, best first, later on the UI thread.
    void suggest(std::string word, int max, std::function<void(std::vector<std::string>)> done);
    void addToDictionary(const std::string &word);
    void ignore(const std::string &word); // for the rest of this run only

    // Whether and against what words are checked changed (on/off, loaded,
    // a word added or ignored): re-check what is on screen.
    using ObserverId = base::Observers::Id;
    ObserverId observe(std::function<void()> fn);
    void       unobserve(ObserverId id) { _observers.remove(id); }

    // What to check when no language was chosen: the first preferred UI
    // language's own dictionary ("en_US" for "en-US"), else another of its
    // language ("en_GB"), else the first there is.
    static std::vector<std::string> defaultLanguages(
        const std::vector<Language> &available, const std::vector<std::string> &preferred
    );

    // Tests: check with `backend` (already loaded) as if it were the
    // platform's; nullptr turns checking off.
    void setBackendForTesting(plat::App &app, std::unique_ptr<Backend> backend);

    struct State; // shared with the workers (spell_checker.cpp)

private:
    Checker();
    void apply();
    void reset();
    void changed() { _observers.notify(); }

    plat::App               *_app     = nullptr;
    bool                     _enabled = false;
    std::vector<std::string> _languages;
    std::shared_ptr<State>   _state;                // the backend, its cache and the ignore list
    Backend                 *_backend    = nullptr; // _state's, while active
    uint32_t                 _generation = 0;       // drops a load that apply() superseded
    base::Observers          _observers;
};

} // namespace spell
