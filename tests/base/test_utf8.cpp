#include "support/test.h"
#include "base/utf8.h"

#include <cstdio>
#include <cstring>
#include <vector>

TEST("utf8: decode and append round trip") {
    const std::string s = "a\xC3\xA5\xE2\x82\xAC\xF0\x9F\x9A\x80"; // a å € 🚀
    size_t            i = 0;
    CHECK(utf8::decode(s, i) == 'a');
    CHECK(utf8::decode(s, i) == 0xE5);
    CHECK(utf8::decode(s, i) == 0x20AC);
    CHECK(utf8::decode(s, i) == 0x1F680);
    CHECK(i == s.size());
    std::string out;
    for (uint32_t cp : {0x61u, 0xE5u, 0x20ACu, 0x1F680u})
        utf8::append(out, cp);
    CHECK(out == s);
    CHECK(utf8::countCodePoints(s) == 4);
}

TEST("utf8: invalid sequences decode as U+FFFD one byte at a time") {
    struct {
        const char *bytes;
        size_t      len;
    } bad[] = {
        {"\x80", 1},             // stray continuation
        {"\xC0\xAF", 2},         // overlong '/'
        {"\xE0\x80\xAF", 3},     // overlong
        {"\xED\xA0\x80", 3},     // UTF-16 surrogate
        {"\xF4\x90\x80\x80", 4}, // above U+10FFFF
        {"\xE2\x82", 2},         // truncated
        {"\xFF", 1},
    };
    for (const auto &b : bad) {
        const std::string_view s(b.bytes, b.len);
        CHECK_FALSE(utf8::isValid(s));
        size_t i = 0;
        CHECK(utf8::decode(s, i) == utf8::kReplacement);
        CHECK(i == 1);
    }
    CHECK(utf8::isValid("plain \xEF\xBF\xBD literal replacement is fine"));
    const char kBad[] = {'a', char(0xFF), 'b'};
    CHECK_STR(
        utf8::sanitize(std::string_view(kBad, 3)),
        "a\xEF\xBF\xBD"
        "b"
    );
    CHECK(utf8::sanitize("ok") == "ok");
    std::string out;
    utf8::append(out, 0xD800);
    CHECK(out == "\xEF\xBF\xBD");
}

TEST("utf8: boundaries never split a sequence") {
    const std::string s = "x\u20ACy"; // x € y
    CHECK(utf8::nextBoundary(s, 0) == 1);
    CHECK(utf8::nextBoundary(s, 1) == 4);
    CHECK(utf8::prevBoundary(s, 4) == 1);
    CHECK(utf8::prevBoundary(s, 1) == 0);
    CHECK(utf8::prevBoundary(s, 5) == 4);
    CHECK(utf8::truncateAt(s, 3) == 1);
    CHECK(utf8::truncateAt(s, 4) == 4);
    CHECK(utf8::truncateAt(s, 99) == s.size());
}

TEST("utf8: case folding for search") {
    CHECK_STR(utf8::foldCase("Hello ÅÄÖ Straße ΣΑΣ Привет"), "hello åäö straße σασ привет");
    CHECK(utf8::containsFolded("Palette v3 is out", "PALETTE"));
    CHECK(utf8::containsFolded("Lena Sørensen", "SØR"));
    CHECK(utf8::containsFolded("Tomás Ferreira", "TOMÁS"));
    CHECK_FALSE(utf8::containsFolded("abc", "abcd"));
    CHECK_FALSE(utf8::containsFolded("", "a"));
    CHECK(utf8::containsFolded("anything", ""));
    CHECK(utf8::containsFolded("xxab", "AB"));
    CHECK(utf8::foldCase(0x178) == 0xFF);
    CHECK(utf8::foldCase(0x141) == 0x142); // Ł → ł
}

TEST("utf8: word characters") {
    CHECK(utf8::isWordChar('a'));
    CHECK(utf8::isWordChar('7'));
    CHECK_FALSE(utf8::isWordChar('_'));
    CHECK_FALSE(utf8::isWordChar('.'));
    CHECK(utf8::isWordChar(0xE4));   // ä
    CHECK(utf8::isWordChar(0x4E2D)); // 中
    CHECK_FALSE(utf8::isWordChar(0x1F680));
    CHECK_FALSE(utf8::isWordChar(0x2014)); // em dash
    CHECK(utf8::isSpace(0xA0));
    CHECK_FALSE(utf8::isSpace('x'));
}

TEST("utf8: encode lengths at every boundary") {
    struct {
        uint32_t    cp;
        const char *want;
    } cases[] = {
        {0x00, ""},
        {0x7F, "\x7F"},
        {0x80, "\xC2\x80"},
        {0x7FF, "\xDF\xBF"},
        {0x800, "\xE0\xA0\x80"},
        {0xD7FF, "\xED\x9F\xBF"},
        {0xE000, "\xEE\x80\x80"},
        {0xFFFF, "\xEF\xBF\xBF"},
        {0x10000, "\xF0\x90\x80\x80"},
        {0x10FFFF, "\xF4\x8F\xBF\xBF"},
    };
    for (const auto &c : cases) {
        char         b[4];
        const size_t n    = utf8::encode(b, c.cp);
        const size_t want = c.cp ? std::strlen(c.want) : 1;
        CHECK(n == want);
        CHECK(std::string_view(b, n) == std::string_view(c.want, want));
        CHECK(int(n) == utf8::encodedLength(c.cp));
        // And it decodes back to itself.
        size_t i = 0;
        CHECK(utf8::decode(std::string_view(b, n), i) == c.cp);
        CHECK(i == n);
    }
    // Surrogates (either half) and values above U+10FFFF encode as U+FFFD.
    for (uint32_t cp : {0xD800u, 0xDBFFu, 0xDC00u, 0xDFFFu, 0x110000u, 0xFFFFFFFFu}) {
        char b[4];
        CHECK(utf8::encode(b, cp) == 3);
        CHECK(std::string_view(b, 3) == "\xEF\xBF\xBD");
        std::string out = "x";
        utf8::append(out, cp);
        CHECK(out == "x\xEF\xBF\xBD");
    }
}

TEST("utf8: overlong forms of every length are rejected") {
    // NUL and '/' in 2, 3 and 4 bytes, the smallest overlong of each length,
    // and the CESU-8 surrogate pair for U+1F600.
    const std::string_view bad[] = {
        std::string_view("\xC0\x80", 2),
        "\xC1\xBF",
        "\xE0\x80\xAF",
        "\xE0\x9F\xBF",
        "\xF0\x80\x80\xAF",
        "\xF0\x8F\xBF\xBF",
        "\xED\xA0\xBD\xED\xB8\x80",
        "\xED\xBF\xBF",
        "\xF5\x80\x80\x80",
    };
    for (std::string_view s : bad) {
        CHECK_FALSE(utf8::isValid(s));
        // Every byte resyncs on its own: as many U+FFFD as bytes.
        size_t i = 0, n = 0;
        while (i < s.size()) {
            CHECK(utf8::decode(s, i) == utf8::kReplacement);
            ++n;
        }
        CHECK(n == s.size());
    }
    // The shortest forms right next to them are fine.
    for (std::string_view s : {"\xC2\x80", "\xE0\xA0\x80", "\xF0\x90\x80\x80", "\xED\x9F\xBF"})
        CHECK(utf8::isValid(s));
}

TEST("utf8: truncateAt is the largest boundary at or below the limit") {
    // Valid sequences of every length, a stray continuation byte, a cut
    // sequence, an invalid lead and a lone lead at the end.
    const std::string s =
        "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80\x80\xE2\x82z\xF8\xF0\x9F\x98\x80\xC3";
    std::vector<size_t> bounds{0};
    for (size_t i = 0; i < s.size();)
        bounds.push_back(i = utf8::nextBoundary(s, i));
    for (size_t max = 0; max <= s.size() + 2; ++max) {
        size_t want = 0;
        for (size_t b : bounds)
            if (b <= max)
                want = b;
        if (!CHECK(utf8::truncateAt(s, max) == want))
            std::fprintf(
                stderr, "    max %zu: %zu, want %zu\n", max, utf8::truncateAt(s, max), want
            );
    }
    CHECK(utf8::truncateAt("", 0) == 0);
    CHECK(utf8::truncateAt("\x80\x80\x80\x80\x80", 3) == 3);
}

TEST("utf8: containsFoldedNeedle takes the needle folded once") {
    const std::string needle = utf8::foldCase("\xC3\x96RJAN"); // "ÖRJAN"
    CHECK(utf8::containsFoldedNeedle("Hej \xC3\x96rjan!", needle));
    CHECK(utf8::containsFoldedNeedle("\xC3\xB6rjan", needle));
    CHECK_FALSE(utf8::containsFoldedNeedle("orjan", needle));
    CHECK(utf8::containsFoldedNeedle("anything", ""));
    CHECK_FALSE(utf8::containsFoldedNeedle("", "x"));
    CHECK(utf8::containsFolded("Hej \xC3\x96rjan!", "\xC3\xB6RJ"));
    CHECK(utf8::containsFoldedNeedle("\xC4\xB0stanbul", "ist")); // İ folds to ASCII i
    CHECK(utf8::containsFoldedNeedle("xx\xC5\xBFTOP", "stop"));  // ſ folds to s
    CHECK(utf8::containsFoldedNeedle("aaaB", "aab"));
    CHECK(utf8::containsFoldedNeedle("ABC", "c"));
    CHECK_FALSE(utf8::containsFoldedNeedle("ABC", "cd"));
}

TEST("utf8: containsPrefolded") {
    CHECK(utf8::containsPrefolded("hej \xC3\xB6rjan", "\xC3\xB6r"));
    CHECK(utf8::containsPrefolded("abc", ""));
    CHECK_FALSE(utf8::containsPrefolded("abc", "abcd"));
    CHECK_FALSE(utf8::containsPrefolded("", "a"));
}

TEST("utf8: prefixBytes and ellipsize count code points, not bytes") {
    const std::string s = "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80z"; // a ä € 😀 z
    CHECK(utf8::prefixBytes(s, 0) == 0);
    CHECK(utf8::prefixBytes(s, 2) == 3);
    CHECK(utf8::prefixBytes(s, 4) == 10);
    CHECK(utf8::prefixBytes(s, 99) == s.size());
    CHECK_STR(utf8::ellipsize(s, 5), s);
    CHECK_STR(utf8::ellipsize(s, 4), "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80\xE2\x80\xA6");
    CHECK_STR(utf8::ellipsize(s, 4, 2), "a\xC3\xA4\xE2\x80\xA6");
    CHECK_STR(utf8::ellipsize(s, 0), "\xE2\x80\xA6");
    CHECK_STR(utf8::ellipsize("", 0), "");
}
