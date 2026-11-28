#include "app/mrkdwn/emoji.h"
#include "app/mrkdwn/smileys.h"
#include "base/json.h"
#include "base/file.h"
#include "base/utf8.h"
#include "support/test.h"

TEST("emoji: shortcodes resolve") {
    CHECK_STR(emoji::toUnicode("rocket"), "🚀");
    CHECK_STR(emoji::toUnicode("+1"), "👍");
    CHECK_STR(emoji::toUnicode("palm_tree"), "🌴");
    CHECK_STR(emoji::toUnicode("airplane"), "✈️");
    CHECK(emoji::toUnicode("no_such_emoji").empty());
    CHECK(emoji::toUnicode("").empty());
    CHECK(emoji::isKnown("tada"));
    CHECK_FALSE(emoji::isKnown("tad"));
}

TEST("emoji: every entry of the source table round-trips through the compact table") {
    std::string src;
    REQUIRE(file::readAll(MSGA_SOURCE_DIR "/scripts/emoji_table.json", &src));
    json::Document d;
    REQUIRE(d.parse(std::move(src), nullptr));
    int n = 0;
    for (json::Value e : d.root()) {
        if (!CHECK_STR(emoji::toUnicode(e.key()), e.str()))
            break;
        ++n;
    }
    CHECK(n == emoji::count());
}

TEST("emoji: skin tones") {
    CHECK_STR(emoji::toUnicode("+1::skin-tone-3"), "👍🏼");
    CHECK_STR(emoji::toUnicode("wave::skin-tone-6"), "👋🏿");
    // VS16 after the base is replaced by the modifier.
    CHECK_STR(emoji::applySkinTone("\xE2\x98\x9D\xEF\xB8\x8F", 4), "\xE2\x98\x9D\xF0\x9F\x8F\xBD");
    // ZWJ sequence: the modifier follows the first code point.
    CHECK_STR(
        emoji::applySkinTone("\xF0\x9F\x99\x8B\xE2\x80\x8D\xE2\x99\x82\xEF\xB8\x8F", 2),
        "\xF0\x9F\x99\x8B\xF0\x9F\x8F\xBB\xE2\x80\x8D\xE2\x99\x82\xEF\xB8\x8F"
    );
    CHECK(emoji::toUnicode("nope::skin-tone-2").empty());
}

TEST("emoji: expand, complete, iterate") {
    CHECK_STR(
        emoji::expandShortcodes("Hello :palm_tree: world :nope: 14:30"),
        "Hello 🌴 world :nope: 14:30"
    );
    std::vector<std::string> out;
    emoji::complete("thu", 10, out);
    REQUIRE(out.size() >= 2);
    CHECK_STR(out[0], "thumbsdown");
    CHECK_STR(out[1], "thumbsup");
    out.clear();
    emoji::complete("zzzz", 5, out);
    CHECK(out.empty());
    out.clear();
    emoji::complete("", 3, out);
    CHECK(out.size() == 3);
    int         seen = 0;
    std::string prevOwned;
    bool        sorted = true;
    emoji::forEach([&](std::string_view name, const std::string &u) {
        if (seen && !(prevOwned < name))
            sorted = false;
        prevOwned = std::string(name);
        ++seen;
        return !u.empty();
    });
    CHECK(sorted);
    CHECK(seen == emoji::count());
    // Names only, the same ones, already case-folded (the picker's search
    // matches against them as they are).
    int  named  = 0;
    bool folded = true;
    emoji::forEachName([&](std::string_view name) {
        folded = folded && utf8::foldCase(name) == name;
        ++named;
        return true;
    });
    CHECK(folded);
    CHECK(named == emoji::count());
}

// ── Text smileys ────────────────────────────────────────────────────────────

namespace {

// What typing a smiley does at the moment a boundary closes it: the text with
// the smiley that ends just before its last byte (the space just typed) put
// there as its emoji.
std::string finished(std::string_view text, std::string_view map = {}) {
    std::string out(text);
    if (out.empty())
        return out;
    const mrkdwn::SmileyHit hit = mrkdwn::smileyEndingAt(out, out.size() - 1, map);
    if (hit)
        out.replace(hit.at, hit.len, hit.glyph);
    return out;
}

// What sending does with a smiley the end of the message closes.
std::string atEnd(std::string_view text, std::string_view map = {}) {
    std::string             out(text);
    const mrkdwn::SmileyHit hit = mrkdwn::smileyEndingAt(out, out.size(), map);
    if (hit)
        out.replace(hit.at, hit.len, hit.glyph);
    return out;
}

} // namespace

TEST("smileys: the one a word just ended, the longest match first, only as a word") {
    const std::string smile = emoji::toUnicode("slightly_smiling_face");
    const std::string wink  = emoji::toUnicode("wink");
    REQUIRE(!smile.empty());
    CHECK_STR(finished("on it :) "), "on it " + smile + " ");
    CHECK_STR(finished(":-) "), smile + " "); // ":-)" whole, not ":)"
    CHECK_STR(finished("a ;-) "), "a " + wink + " ");
    CHECK_STR(finished("nice :),"), "nice " + smile + ","); // punctuation ends it too
    CHECK_STR(finished("on it :)"), "on it :)");            // nothing has ended it yet
    CHECK_STR(finished("on it :"), "on it :");
    // A word with a colon in it is a word, and so is a shortcode.
    CHECK_STR(finished("a:) "), "a:) ");
    CHECK_STR(finished("the :Date: "), "the :Date: ");
    CHECK_STR(finished("see <3abc "), "see <3abc ");
    // Code is code: a smiley in it stays as typed.
    CHECK_STR(finished("use `:)` "), "use `:)` ");
    CHECK_STR(finished("use `:) "), "use `:) "); // an unclosed span is still code
    // The end of the message closes one without a space.
    CHECK_STR(atEnd("thanks :)"), "thanks " + smile);
    CHECK_STR(atEnd("thanks :"), "thanks :");
    CHECK_STR(atEnd("the :Date:"), "the :Date:");
    // What ends is the smiley nearest the caret: the other one was converted
    // when its own space closed it.
    CHECK_STR(finished(":) and :) "), ":) and " + smile + " ");
}

TEST("smileys: a map of their own, shortcode or character, the first line wins") {
    CHECK_STR(finished("hi :-) ", ":-) :wave:"), "hi " + emoji::toUnicode("wave") + " ");
    CHECK_STR(finished("hi :) ", ":-) :wave:"), "hi :) "); // not in the map
    // The character itself, and a custom emoji kept as the shortcode Slack knows.
    CHECK_STR(
        finished("love <3 ", "<3 \xE2\x9D\xA4\xEF\xB8\x8F"), "love \xE2\x9D\xA4\xEF\xB8\x8F "
    );
    CHECK_STR(finished("meh :| ", ":| :party_parrot:"), "meh :party_parrot: ");
    // Blank lines, a line without its emoji, a comment and a repeat are skipped.
    const auto pairs = mrkdwn::parseSmileys("\n  :D :smiley:  \n:-)\n# :( nope\n:D :laughing:\n");
    REQUIRE(pairs.size() == 1);
    CHECK_STR(pairs[0].from, ":D");
    CHECK_STR(pairs[0].to, emoji::toUnicode("smiley"));
    // An empty map is the usual set, and the two spellings of it agree.
    CHECK(mrkdwn::parseSmileys("").size() > 20);
    CHECK(finished("ok :) ", mrkdwn::defaultSmileyMap()) == finished("ok :) ", ""));
    CHECK_STR(
        finished("ok :) ", mrkdwn::defaultSmileyMap()),
        "ok " + emoji::toUnicode("slightly_smiling_face") + " "
    );
}
