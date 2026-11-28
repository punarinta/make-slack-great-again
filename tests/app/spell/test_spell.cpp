// Spell checking: what the
// composer checks (spell::checkableWords), the checker's cache, ignore list
// and dictionary additions over a fake backend, language names and defaults,
// and on Linux the Hunspell backend against tiny 8-bit dictionaries and the
// system's en_US one (skipped where it isn't installed).
#include "app/identity.h"
#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#include "base/file.h"
#include "support/test.h"
#include "plat/plat.h"

#include <cstdlib>

namespace {

plat::App &app() {
    static std::unique_ptr<plat::App> a = plat::App::create(nullptr);
    return *a;
}

template <class F>
bool until(F done, int ms = 3000) {
    for (int t = 0; t < ms && !done(); t += 5)
        app().pump(5);
    return done();
}

// The words checkableWords finds, '|'-joined.
std::string words(std::string_view text, const std::vector<spell::Span> &ex = {}) {
    std::string out;
    for (const spell::Span &s : spell::checkableWords(text, ex)) {
        if (!out.empty())
            out += '|';
        out += text.substr(s.start, s.length);
    }
    return out;
}

// Knows every word but `bad`; suggests from `suggestions`.
struct FakeBackend : spell::Backend {
    std::vector<std::string> bad, added;
    std::vector<std::string> suggestions;
    int                     *checks;
    bool                     safe;
    FakeBackend(int *c, bool threadSafe) : checks(c), safe(threadSafe) {}
    bool load(const std::vector<std::string> &) override { return true; }
    bool threadSafe() const override { return safe; }
    bool check(std::string_view w) override {
        ++*checks;
        const auto has = [&](const std::vector<std::string> &v) {
            for (const auto &x : v)
                if (x == w)
                    return true;
            return false;
        };
        return !has(bad) || has(added);
    }
    std::vector<std::string> suggest(std::string_view, int max) override {
        std::vector<std::string> s = suggestions;
        if (int(s.size()) > max)
            s.resize(size_t(max));
        return s;
    }
    void addToDictionary(std::string_view w) override { added.emplace_back(w); }
};

std::vector<spell::Span> checkNow(std::string text, std::vector<spell::Span> ex = {}) {
    std::vector<spell::Span> out;
    bool                     done = false;
    spell::Checker::instance().check(
        std::move(text), std::move(ex), [&](std::vector<spell::Span> b) {
            out  = std::move(b);
            done = true;
        }
    );
    until([&] { return done; });
    return out;
}

} // namespace

// ── What gets checked ───────────────────────────────────────────────────────

TEST("spell words: prose words are checked, in order") {
    CHECK_STR(words("Helo wrold, how are you?"), "Helo|wrold|how|are|you");
}

TEST("spell words: an apostrophe between letters stays in the word") {
    CHECK_STR(words("don't won\xE2\x80\x99t"), "don't|won\xE2\x80\x99t");
    CHECK_STR(words("'quoted' James'"), "quoted|James");
}

TEST("spell words: markdown markers around a word are not part of it") {
    CHECK_STR(words("*bold* _it_ ~gone~ (paren)"), "bold|it|gone|paren");
}

TEST("spell words: code is never checked") {
    CHECK_STR(words("see `fooo barr` here"), "see|here");
    CHECK_STR(words("a lone ` tick stays"), "lone|tick|stays");
    CHECK_STR(words("one ```xyzzy``` two"), "one|two");
    // A lone backtick's partner must be on its own line.
    CHECK_STR(words("tick ` here\nthere ` too"), "tick|here|there|too");
}

TEST("spell words: a code fence carries over to the next lines") {
    CHECK_STR(words("before ```qwrt\nzzkx vvqq\nzzkx``` after"), "before|after");
    CHECK_STR(words("open ```\nzzkx vvqq"), "open"); // unterminated: code to the end
}

TEST("spell words: links, tokens and emoji are never checked") {
    CHECK_STR(words("go https://exmaple.com/pth?q=1 now"), "go|now");
    CHECK_STR(words("www.exmaple.org ok"), "ok");
    CHECK_STR(words("hi <@U123ABC> and <https://x.io|lnk>"), "hi|and");
    CHECK_STR(words("nice :thumbsup: :skin-tone-2: yes"), "nice|yes");
}

TEST("spell words: mentions, channels, commands, paths and hosts are skipped") {
    CHECK_STR(words("ask @jdoe in #genral"), "ask|in");
    CHECK_STR(words("mail me@exmaple.com"), "mail");
    CHECK_STR(words("/remnd me"), "me");
    CHECK_STR(words("open C:\\Users\\bob or msga.app"), "open|or");
}

TEST("spell words: identifiers, acronyms and single letters are skipped") {
    CHECK_STR(words("mp3 v2beta snake_case HTTP camelCase iPhone a Ok"), "Ok");
    CHECK_STR(words("\xD0\xA0\xD0\xA4 \xD0\xBC\xD0\xB8\xD1\x80"), "\xD0\xBC\xD0\xB8\xD1\x80");
}

TEST("spell words: excluded ranges read as whitespace") {
    // "@Jane Doe" is a mention pill: neither half is checked, and the word
    // after it is not glued to it.
    CHECK_STR(words("hey @Jane Doe thx", {{4, 9}}), "hey|thx");
    CHECK_STR(words("hey Jane Doethx", {{4, 4}}), "hey|Doethx");
}

TEST("spell words: offsets are bytes of the UTF-8 text") {
    const std::string t = "\xC3\xA9t\xC3\xA9 caf\xC3\xA9";
    const auto        w = spell::checkableWords(t);
    REQUIRE(w.size() == 2);
    CHECK(w[0].start == 0 && w[0].length == 5);
    CHECK(w[1].start == 6 && w[1].length == 5);
}

TEST("spell words: wordAt finds the word around a cursor") {
    const std::string line = "say don't now";
    CHECK(spell::wordAt(line, 4) == (spell::Span{4, 5}));
    CHECK(spell::wordAt(line, 9) == (spell::Span{4, 5})); // at its end
    CHECK(spell::wordAt(line, 6) == (spell::Span{4, 5})); // at the apostrophe
    CHECK(spell::wordAt("a  b", 2).length == 0);
}

// ── The checker ─────────────────────────────────────────────────────────────

TEST("spell checker: off by default, it checks nothing") {
    auto &checker = spell::Checker::instance();
    CHECK(!checker.enabled());
    CHECK(!checker.active());
    spell::Checker::instance().setBackendForTesting(app(), nullptr);
    CHECK(checkNow("wrold").empty());
}

TEST("spell checker: misspelled words come back as spans, each word checked once") {
    for (bool threadSafe : {true, false}) { // Hunspell's worker path, the system checkers' slices
        int  checks = 0;
        auto f      = std::make_unique<FakeBackend>(&checks, threadSafe);
        f->bad      = {"wrold", "teh"};
        spell::Checker::instance().setBackendForTesting(app(), std::move(f));
        const auto bad = checkNow("hello wrold, teh wrold `teh` <@U1>");
        REQUIRE(bad.size() == 3);
        CHECK(bad[0] == (spell::Span{6, 5}));
        CHECK(bad[1] == (spell::Span{13, 3}));
        CHECK(bad[2] == (spell::Span{17, 5}));
        CHECK(checks == 3); // hello, wrold, teh: the repeat is cached
        CHECK(checkNow("wrold").size() == 1);
        CHECK(checks == 3);
    }
    spell::Checker::instance().setBackendForTesting(app(), nullptr);
}

TEST("spell checker: a long text is checked in slices without losing words") {
    int  checks = 0;
    auto f      = std::make_unique<FakeBackend>(&checks, false);
    f->bad      = {"zzq"};
    spell::Checker::instance().setBackendForTesting(app(), std::move(f));
    std::string text;
    for (int i = 0; i < 1500; ++i)
        text += "w" + std::string(size_t(2 + i % 50), char('a' + i % 26)) + " zzq ";
    const auto bad = checkNow(text);
    CHECK(bad.size() == 1500);
    spell::Checker::instance().setBackendForTesting(app(), nullptr);
}

TEST("spell checker: a main-thread checker gets each slice's new words in one call") {
    struct Batching : FakeBackend {
        using FakeBackend::FakeBackend;
        std::vector<size_t> batches;
        void checkAll(const std::vector<std::string> &w, std::vector<uint8_t> *right) override {
            batches.push_back(w.size());
            FakeBackend::checkAll(w, right);
        }
    };
    int   checks = 0;
    auto  f      = std::make_unique<Batching>(&checks, false);
    auto *fake   = f.get();
    f->bad       = {"zzq"};
    spell::Checker::instance().setBackendForTesting(app(), std::move(f));
    std::string text;
    for (int i = 0; i < 450; ++i) // 450 distinct words, each followed by zzq
        text += "w" + std::string(size_t(1 + i / 26), char('a' + i % 26)) + " zzq ";
    CHECK(checkNow(text).size() == 450);
    REQUIRE(fake->batches.size() == 3); // 451 new words: 200 + 200 + 51
    CHECK(fake->batches[0] == 200);
    CHECK(fake->batches[2] == 51);
    CHECK(checks == 451); // the default checkAll: once per new word
    spell::Checker::instance().setBackendForTesting(app(), nullptr);
}

TEST("spell checker: a curly apostrophe is checked as a straight one") {
    int  checks = 0;
    auto f      = std::make_unique<FakeBackend>(&checks, true);
    f->bad      = {"dont't"};
    spell::Checker::instance().setBackendForTesting(app(), std::move(f));
    CHECK(checkNow("dont\xE2\x80\x99t").size() == 1);
    spell::Checker::instance().setBackendForTesting(app(), nullptr);
}

TEST("spell checker: ignore and add to dictionary stop the underline, and say so") {
    int   checks = 0;
    auto  f      = std::make_unique<FakeBackend>(&checks, true);
    auto *fake   = f.get();
    f->bad       = {"worlld", "msgaa"};
    auto &c      = spell::Checker::instance();
    c.setBackendForTesting(app(), std::move(f));
    int        changed = 0;
    const auto id      = c.observe([&] { ++changed; });
    c.ignore("worlld");
    CHECK(changed == 1);
    REQUIRE(checkNow("worlld msgaa").size() == 1);
    c.addToDictionary("msgaa");
    REQUIRE(until([&] { return changed == 2; }));
    REQUIRE(fake->added.size() == 1);
    CHECK_STR(fake->added[0], "msgaa");
    CHECK(checkNow("worlld msgaa").empty());
    c.unobserve(id);
    c.setBackendForTesting(app(), nullptr);

    // A main-thread checker (the system ones) adds in a later turn too,
    // never inside the call that asked.
    auto  m     = std::make_unique<FakeBackend>(&checks, false);
    auto *mfake = m.get();
    c.setBackendForTesting(app(), std::move(m));
    int        later = 0;
    const auto id2   = c.observe([&] { ++later; });
    c.addToDictionary("msgab");
    CHECK(mfake->added.empty());
    CHECK(later == 0);
    REQUIRE(until([&] { return later == 1; }));
    CHECK(mfake->added.size() == 1);
    c.unobserve(id2);
    c.setBackendForTesting(app(), nullptr);
}

TEST("spell checker: suggestions arrive later, at most max") {
    int  checks    = 0;
    auto f         = std::make_unique<FakeBackend>(&checks, true);
    f->suggestions = {"world", "would", "wild"};
    spell::Checker::instance().setBackendForTesting(app(), std::move(f));
    std::vector<std::string> got;
    bool                     done = false;
    spell::Checker::instance().suggest("wrold", 2, [&](std::vector<std::string> s) {
        got  = std::move(s);
        done = true;
    });
    CHECK(!done);
    REQUIRE(until([&] { return done; }));
    REQUIRE(got.size() == 2);
    CHECK_STR(got[0], "world");
    spell::Checker::instance().setBackendForTesting(app(), nullptr);
}

TEST("spell checker: the default language follows the preferred UI language") {
    using L        = spell::Language;
    const auto def = [](std::vector<L> a, std::vector<std::string> p) {
        const auto r = spell::Checker::defaultLanguages(a, p);
        return r.empty() ? std::string() : r[0];
    };
    CHECK_STR(def({{"zz_ZZ", {}}, {"de_DE", {}}}, {"de-DE"}), "de_DE");
    CHECK_STR(def({{"zz_ZZ", {}}, {"de_AT", {}}}, {"de_DE.UTF-8"}), "de_AT");
    CHECK_STR(def({{"zz_ZZ", {}}, {"en-US", {}}}, {"en_US"}), "en-US"); // Windows codes
    CHECK_STR(def({{"zz_ZZ", {}}}, {"de_DE"}), "zz_ZZ");
    CHECK(spell::Checker::defaultLanguages({}, {"de"}).empty());
    CHECK_STR(spell::detail::localeCode("en_US.UTF-8@euro"), "en_US");
    CHECK_STR(spell::detail::localeCode("zh-Hans-CN"), "zh_CN");
    CHECK_STR(spell::detail::localeCode("C"), "");
}

TEST("spell checker: language names are the language's own") {
    CHECK_STR(spell::languageName("de_DE"), "Deutsch (de_DE)");
    CHECK_STR(
        spell::languageName("ru-RU"),
        "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9 (ru-RU)"
    );
    CHECK_STR(spell::languageName("de_DE_frami"), "Deutsch (de_DE_frami)");
    CHECK_STR(spell::languageName("en_US"), "American English (en_US)");
    CHECK_STR(spell::languageName("de"), "German (de)"); // no territory: the English name
    CHECK_STR(spell::languageName("qqq"), "qqq");
}

TEST("spell checker: configure with no such dictionary stays inactive; off frees it") {
    auto &c = spell::Checker::instance();
    c.configure(app(), true, {"xx_NONE"});
    CHECK(c.enabled());
    for (int i = 0; i < 40; ++i)
        app().pump(5);
    CHECK(!c.active());
    c.configure(app(), false, {"xx_NONE"});
    CHECK(!c.enabled());
    CHECK(!c.active());
}

// ── Hunspell (Linux, BSDs) ────────────────────────────────────────────────────────

#if defined(MSGA_FREEDESKTOP)

namespace {

bool hasSystemEnUs() {
    for (const spell::Language &l : spell::detail::availableLanguages())
        if (l.code == "en_US")
            return true;
    return false;
}

std::string tempDir() {
    std::string d = app().standardDir(plat::StandardDir::Temp) + "/msga-spell-test";
    file::makeDirs(d);
    return d;
}

} // namespace

TEST("hunspell: reads 8-bit dictionaries through their SET") {
    // Two tiny dictionaries on $DICPATH, as ISO-8859-1 and KOI8-R bytes.
    const std::string dir = tempDir();
    REQUIRE(file::writeAtomic(dir + "/xx_LATIN.aff", "SET ISO8859-1\nTRY esartn\n"));
    REQUIRE(
        file::writeAtomic(
            dir + "/xx_LATIN.dic",
            "2\nstra\xdf"
            "e\nf\xfcr\n"
        )
    );
    REQUIRE(file::writeAtomic(dir + "/xx_KOI.aff", "SET KOI8-R\nTRY \xd4\xc5\n"));
    // "привет" in KOI8-R.
    REQUIRE(file::writeAtomic(dir + "/xx_KOI.dic", "1\n\xd0\xd2\xc9\xd7\xc5\xd4\n"));
    const char       *old    = std::getenv("DICPATH");
    const std::string before = old ? old : "";
    base::test::setEnv("DICPATH", dir);

    bool latin = false, koi = false;
    for (const spell::Language &l : spell::detail::availableLanguages()) {
        latin = latin || l.code == "xx_LATIN";
        koi   = koi || l.code == "xx_KOI";
    }
    CHECK(latin);
    CHECK(koi);

    auto backend = spell::createPlatformBackend(app());
    CHECK(backend->loadsOffThread() && backend->threadSafe());
    REQUIRE(backend->load({"xx_LATIN", "xx_KOI"}));
    const std::string privet = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    CHECK(backend->check(
        "stra\xC3\x9F"
        "e"
    ));
    CHECK(backend->check("f\xC3\xBCr"));
    CHECK(backend->check(privet));
    CHECK(!backend->check("strase"));
    CHECK(!backend->check("\xD0\xBF\xD1\x80\xD1\x8B\xD0\xB2\xD0\xB5\xD1\x82"));
    CHECK(!backend->check("\xE6\x97\xA5\xE6\x9C\xAC")); // in neither set
    bool found = false;
    for (const std::string &s : backend->suggest(privet + "\xD1\x82", 5))
        found = found || s == privet;
    CHECK(found);
    found = false;
    for (const std::string &s : backend->suggest("strasse", 5))
        found = found || s == "stra\xC3\x9F"
                              "e";
    CHECK(found);

    if (old)
        base::test::setEnv("DICPATH", before);
    else
        base::test::unsetEnv("DICPATH");
    for (const char *f : {"xx_LATIN.aff", "xx_LATIN.dic", "xx_KOI.aff", "xx_KOI.dic"})
        file::remove(dir + "/" + f);
    file::remove(dir);
}

TEST("hunspell: checks, suggests and keeps added words with the system en_US") {
    if (!hasSystemEnUs()) {
        std::printf("  skipped: no en_US Hunspell dictionary installed (hunspell-en-us)\n");
        return;
    }
    // Added words go to <dataDir>/spelling/words.txt: a throwaway HOME.
    const std::string home = tempDir() + "/home";
    file::makeDirs(home);
    const char       *oldData = std::getenv("XDG_DATA_HOME");
    const std::string before  = oldData ? oldData : "";
    base::test::setEnv("XDG_DATA_HOME", home + "/.local/share");
    {
        auto backend = spell::createPlatformBackend(app());
        REQUIRE(backend->load({"en_US"}));
        CHECK(backend->check("hello"));
        CHECK(backend->check("Hello"));
        CHECK(backend->check("don't"));
        CHECK(!backend->check("helo"));
        CHECK(!backend->check("wrold"));
        const auto s     = backend->suggest("wrold", 5);
        bool       world = false;
        for (const auto &x : s)
            world = world || x == "world";
        CHECK(world);
        CHECK(s.size() <= 5);
        CHECK(!backend->check("msgaword"));
        backend->addToDictionary("msgaword");
        CHECK(backend->check("msgaword"));
    }
    auto again = spell::createPlatformBackend(app());
    REQUIRE(again->load({"en_US"}));
    CHECK(again->check("msgaword"));
    const std::string words = identity::dataDir(app()) + "/spelling/words.txt";
    file::remove(words);
    if (oldData)
        base::test::setEnv("XDG_DATA_HOME", before);
    else
        base::test::unsetEnv("XDG_DATA_HOME");
}

#endif

// ── The system checker (macOS, Windows) ─────────────────────────────────────

#if defined(__APPLE__) || defined(_WIN32)

TEST("system checker: checks and suggests English") {
    // Any English the OS offers: "en" / "en_US" (AppKit), "en-US" (Windows).
    std::string english;
    for (const spell::Language &l : spell::detail::availableLanguages())
        if (l.code.rfind("en", 0) == 0 && (english.empty() || l.code.size() < english.size()))
            english = l.code;
    if (english.empty()) {
        std::printf("  skipped: the system offers no English spell checking\n");
        return;
    }
    auto backend = spell::createPlatformBackend(app());
    REQUIRE(!backend->loadsOffThread() && !backend->threadSafe());
    REQUIRE(backend->load({english}));
    CHECK(backend->check("hello"));
    CHECK(backend->check("don't"));
    CHECK(!backend->check("wrold"));
    const auto s     = backend->suggest("wrold", 5);
    bool       world = false;
    for (const auto &x : s)
        world = world || x == "world";
    CHECK(world);
    CHECK(s.size() <= 5);
    CHECK(!spell::createPlatformBackend(app())->load({"xx-NONE"}));
}

#endif
