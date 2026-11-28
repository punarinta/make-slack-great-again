// The mrkdwn parser (offsets are bytes; they equal UTF-16 offsets for these
// inputs except where noted), plus the layout view and bare URLs.
#include "app/mrkdwn/link_labels.h"
#include "app/mrkdwn/mrkdwn.h"
#include "support/test.h"
#include "base/time.h"

#include <algorithm>

using mrkdwn::Kind;
using mrkdwn::Rich;

namespace {

// Exactly one entity with the given properties.
bool checkOne(
    const Rich      &r,
    std::string_view text,
    Kind             kind,
    uint32_t         start,
    uint32_t         length,
    std::string_view data = {}
) {
    bool ok = CHECK_STR(r.text, text);
    if (!CHECK(r.entities.size() == 1))
        return false;
    ok &= CHECK(r.entities[0].kind == kind);
    ok &= CHECK(r.entities[0].start == start);
    ok &= CHECK(r.entities[0].length == length);
    ok &= CHECK_STR(r.entities[0].data, data);
    return ok;
}

std::string localDateNum(int64_t secs) {
    return base::isoDate(secs);
}

} // namespace

TEST("mrkdwn: empty string") {
    auto r = mrkdwn::parse("");
    CHECK(r.text.empty());
    CHECK(r.entities.empty());
}

TEST("mrkdwn: plain text passes through unchanged") {
    auto r = mrkdwn::parse("hello world");
    CHECK_STR(r.text, "hello world");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: bold *text*") {
    checkOne(mrkdwn::parse("*bold*"), "bold", Kind::Bold, 0, 4);
}

TEST("mrkdwn: italic _text_") {
    checkOne(mrkdwn::parse("_italic_"), "italic", Kind::Italic, 0, 6);
}

TEST("mrkdwn: underscores inside a word stay literal") {
    auto r = mrkdwn::parse("docs/DEPENDENCY_BUILD_AUDIT.md and snake_case_name");
    CHECK_STR(r.text, "docs/DEPENDENCY_BUILD_AUDIT.md and snake_case_name");
    CHECK(r.entities.empty());
    r = mrkdwn::parse("MAX__LEN__X");
    CHECK_STR(r.text, "MAX__LEN__X");
    CHECK(r.entities.empty());
    // Non-ASCII letters are word characters too (UTF-8 decoding path).
    r = mrkdwn::parse("räk_smörgås_fil");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: italic skips an intraword closer") {
    checkOne(mrkdwn::parse("_see foo_bar_ now"), "see foo_bar now", Kind::Italic, 0, 11);
    checkOne(mrkdwn::parse("(_word_)."), "(word).", Kind::Italic, 1, 4);
}

TEST("mrkdwn: underline __text__") {
    checkOne(mrkdwn::parse("__under__"), "under", Kind::Underline, 0, 5);
}

TEST("mrkdwn: strikethrough ~text~") {
    checkOne(mrkdwn::parse("~strike~"), "strike", Kind::Strike, 0, 6);
}

TEST("mrkdwn: inline code") {
    checkOne(mrkdwn::parse("`code`"), "code", Kind::Code, 0, 4);
}

TEST("mrkdwn: code fence no language hint") {
    checkOne(mrkdwn::parse("```hello```"), "hello", Kind::Pre, 0, 5);
}

TEST("mrkdwn: code fence with language hint skips first line") {
    auto r = mrkdwn::parse("```python\nprint('x')\n```");
    CHECK_STR(r.text, "print('x')\n");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Pre);
    CHECK(r.entities[0].start == 0);
    CHECK(r.entities[0].length == 11);
}

TEST("mrkdwn: code fence keeps prose on the opening line") {
    auto r = mrkdwn::parse("a ```Can you swipe?\nI feel it``` b");
    CHECK_STR(r.text, "a Can you swipe?\nI feel it b");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Pre);
    CHECK(r.entities[0].start == 2);
    CHECK(r.entities[0].length == 24);
}

TEST("mrkdwn: code fence closed on the opening line") {
    auto r = mrkdwn::parse("```x```\nnext");
    CHECK_STR(r.text, "x\nnext");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].length == 1);
}

TEST("mrkdwn: user mention no label") {
    checkOne(mrkdwn::parse("<@U123ABC>"), "@U123ABC", Kind::User, 0, 8, "U123ABC");
}

TEST("mrkdwn: user mention with label") {
    checkOne(mrkdwn::parse("<@U123|alice>"), "alice", Kind::User, 0, 5, "U123");
}

TEST("mrkdwn: channel mention") {
    checkOne(mrkdwn::parse("<#C456|general>"), "#general", Kind::Channel, 0, 8, "C456");
}

TEST("mrkdwn: link no label") {
    checkOne(
        mrkdwn::parse("<https://example.com>"),
        "https://example.com",
        Kind::Link,
        0,
        19,
        "https://example.com"
    );
}

TEST("mrkdwn: link with label") {
    checkOne(
        mrkdwn::parse("<https://example.com|click here>"),
        "click here",
        Kind::Link,
        0,
        10,
        "https://example.com"
    );
}

TEST("mrkdwn: <!here> broadcast") {
    checkOne(mrkdwn::parse("<!here>"), "@here", Kind::Here, 0, 5);
}

TEST("mrkdwn: <!channel> broadcast") {
    checkOne(mrkdwn::parse("<!channel>"), "@channel", Kind::ChannelCmd, 0, 8);
}

TEST("mrkdwn: <!subteam^S…|@handle> user-group mention") {
    checkOne(
        mrkdwn::parse("<!subteam^S0ABC|@eng-oncall>"),
        "@eng-oncall",
        Kind::Usergroup,
        0,
        11,
        "S0ABC"
    );
}

TEST("mrkdwn: <!subteam^S…> without a label shows the id") {
    checkOne(mrkdwn::parse("<!subteam^S0ABC>"), "@S0ABC", Kind::Usergroup, 0, 6, "S0ABC");
}

TEST("mrkdwn: <!everyone> stays a broadcast command") {
    checkOne(mrkdwn::parse("<!everyone>"), "@everyone", Kind::Here, 0, 9, "everyone");
}

TEST("mrkdwn: emoji :name:") {
    checkOne(mrkdwn::parse(":rocket:"), ":rocket:", Kind::Emoji, 0, 8, "rocket");
}

TEST("mrkdwn: a time is not emoji") {
    // "14:43:34" parsed ":43:" as a shortcode, which the renderer then drew in
    // the emoji font at line-height size.
    for (std::string s : {"Yesterday, 14:43:34", "1:2:3", "score 3:100:2"}) {
        CHECK(mrkdwn::parse(s).entities.empty());
        CHECK(mrkdwn::resolveTokens(s).entities.empty());
        CHECK(mrkdwn::parse("<https://e.com/p|" + s + ">").entities.size() == 1);
    }
}

TEST("mrkdwn: digit-only shortcode away from digits still resolves") {
    checkOne(mrkdwn::parse("this :100:"), "this :100:", Kind::Emoji, 5, 5, "100");
    auto r = mrkdwn::resolveTokens("this :100:");
    REQUIRE(r.entities.size() == 1);
    CHECK_STR(r.entities[0].data, "100");
}

TEST("mrkdwn: single-line blockquote") {
    auto r = mrkdwn::parse("> hello");
    CHECK_STR(r.text, "hello\n"); // a line break follows the quote
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Quote);
    CHECK(r.entities[0].start == 0);
    CHECK(r.entities[0].length == 5);
}

TEST("mrkdwn: multi-line blockquote") {
    auto r = mrkdwn::parse("> line1\n> line2");
    CHECK_STR(r.text, "line1\nline2\n");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Quote);
    CHECK(r.entities[0].start == 0);
    CHECK(r.entities[0].length == 11);
}

TEST("mrkdwn: entity mid-sentence has correct offset and length") {
    auto r = mrkdwn::parse("hello *world* today");
    CHECK_STR(r.text, "hello world today");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Bold);
    CHECK(r.entities[0].start == 6);
    CHECK(r.entities[0].length == 5);
}

TEST("mrkdwn: unmatched delimiter passes through as plain text") {
    auto r = mrkdwn::parse("price: $5*2");
    CHECK_STR(r.text, "price: $5*2");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: emoji name with space is not an emoji") {
    auto r = mrkdwn::parse(":not valid:");
    CHECK_STR(r.text, ":not valid:");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: HTML entities are decoded in plain text") {
    auto r = mrkdwn::parse("Kamil &amp; 11 more &lt;3 a&gt;b");
    CHECK_STR(r.text, "Kamil & 11 more <3 a>b");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: decodeEntities decodes ampersand last") {
    CHECK_STR(mrkdwn::decodeEntities("&amp;lt;"), "&lt;");
    CHECK_STR(mrkdwn::decodeEntities("a &lt;b&gt; &amp; c"), "a <b> & c");
}

TEST("mrkdwn: escapeEntities escapes Slack's three, and decodeEntities undoes it") {
    CHECK_STR(mrkdwn::escapeEntities("a <b> & \"c\" 'd'"), "a &lt;b&gt; &amp; \"c\" 'd'");
    CHECK_STR(mrkdwn::escapeEntities("&lt;"), "&amp;lt;");
    CHECK_STR(mrkdwn::escapeEntities(""), "");
    for (const char *s : {"<@U1> & <#C2|x>", "&amp;lt;", "plain", "1 < 2 > 0 &"})
        CHECK_STR(mrkdwn::decodeEntities(mrkdwn::escapeEntities(s)), s);
}

TEST("mrkdwn: msga's &#NN; references are literal characters, never marks") {
    auto r = mrkdwn::parse("&#42;not bold&#42; &#95;x&#95; a&#58;b: &#126;s&#126; &#96;c&#96;");
    CHECK_STR(r.text, "*not bold* _x_ a:b: ~s~ `c`");
    CHECK(r.entities.empty());
    CHECK_STR(mrkdwn::decodeEntities("&#42;&amp;#42;&#7;&#200;"), "*&#42;&#7;&#200;");
}

TEST("mrkdwn: HTML entities are decoded in link URLs and labels") {
    checkOne(
        mrkdwn::parse("<https://example.com?a=1&amp;b=2|A &amp; B>"),
        "A & B",
        Kind::Link,
        0,
        5,
        "https://example.com?a=1&b=2"
    );
}

TEST("mrkdwn: HTML entities are decoded inside code spans") {
    checkOne(mrkdwn::parse("`a &amp;&amp; b`"), "a && b", Kind::Code, 0, 6);
}

TEST("mrkdwn: link nested in bold yields contained entities") {
    // The Google Calendar bot wraps event links in bold: *<url|label>*
    auto r = mrkdwn::parse("*<https://example.com|Stand-Up>*");
    CHECK_STR(r.text, "Stand-Up");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::Bold);
    CHECK(r.entities[0].start == 0);
    CHECK(r.entities[0].length == 8);
    CHECK(r.entities[1].kind == Kind::Link);
    CHECK(r.entities[1].start == 0);
    CHECK(r.entities[1].length == 8);
    CHECK_STR(r.entities[1].data, "https://example.com");
}

TEST("mrkdwn: italic nested in bold") {
    auto r = mrkdwn::parse("*a _b_ c*");
    CHECK_STR(r.text, "a b c");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::Bold);
    CHECK(r.entities[0].length == 5);
    CHECK(r.entities[1].kind == Kind::Italic);
    CHECK(r.entities[1].start == 2);
    CHECK(r.entities[1].length == 1);
}

TEST("mrkdwn: inline marks nest inside blockquote") {
    auto r = mrkdwn::parse("> *bold* word");
    CHECK_STR(r.text, "bold word\n");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::Quote);
    CHECK(r.entities[0].length == 9);
    CHECK(r.entities[1].kind == Kind::Bold);
    CHECK(r.entities[1].start == 0);
    CHECK(r.entities[1].length == 4);
}

TEST("mrkdwn: API-escaped &gt; at line start is a blockquote") {
    auto r = mrkdwn::parse("&gt; line1\n&gt; line2");
    CHECK_STR(r.text, "line1\nline2\n");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Quote);
}

TEST("mrkdwn: stacked blockquote marks don't nest (layout-hang guard)") {
    // Real Slack renders one quote level. A renderer that turns each level
    // into a nested table has a layout cost that explodes: a message of many
    // stacked '>' freezes the UI. The parser caps quote nesting at one.
    const std::string deep   = std::string(64, '>') + " hi";
    auto              r      = mrkdwn::parse(deep);
    int               quotes = 0;
    for (const auto &e : r.entities)
        quotes += e.kind == Kind::Quote;
    CHECK(quotes == 1);
}

TEST("mrkdwn: date token with link") {
    auto r = mrkdwn::parse("<!date^1781248500^{date_num}^https://example.com/event|6/12/26>");
    checkOne(r, localDateNum(1781248500), Kind::Link, 0, 10, "https://example.com/event");
}

TEST("mrkdwn: date token without link renders as plain text") {
    auto r = mrkdwn::parse("on <!date^1781248500^{date_num}|6/12/26> ok");
    CHECK_STR(r.text, "on " + localDateNum(1781248500) + " ok");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: date token with unknown format token falls back") {
    auto r = mrkdwn::parse("<!date^1781248500^{bogus_token}|the fallback>");
    CHECK_STR(r.text, "the fallback");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: date token with invalid timestamp falls back") {
    auto r = mrkdwn::parse("<!date^notanumber^{date_num}|fallback text>");
    CHECK_STR(r.text, "fallback text");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: date token format keeps literal text around tokens") {
    auto r = mrkdwn::parse("<!date^1781248500^due {date_num}!|fb>");
    CHECK_STR(r.text, "due " + localDateNum(1781248500) + "!");
}

TEST("mrkdwn: date token long and time formats") {
    base::setUse24h(false);
    const int64_t ts = base::fromLocal(2020, 3, 15, 14, 34); // a Sunday, not this year
    auto          r  = mrkdwn::parse("<!date^" + std::to_string(ts) + "^{date_long} {time}|fb>");
    CHECK_STR(r.text, "Sunday, March 15, 2020 2:34 PM");
    const int64_t now = base::nowSecs();
    r = mrkdwn::parse("<!date^" + std::to_string(now - 120) + "^{date_pretty}, {ago}|fb>");
    CHECK_STR(r.text, "Today, 2 minutes ago");
}

TEST("mrkdwn: resolveTokens expands a date token") {
    auto r = mrkdwn::resolveTokens("<!date^1781248500^{date_num}|6/12/26>");
    CHECK_STR(r.text, localDateNum(1781248500));
    CHECK(r.entities.empty());
}

TEST("mrkdwn: resolveTokens linkifies <url|label>") {
    auto r = mrkdwn::resolveTokens("see <https://example.com/x?a=1|Ny händelse> now");
    CHECK_STR(r.text, "see Ny händelse now");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Link);
    CHECK(r.entities[0].start == 4);
    CHECK(r.entities[0].length == 12); // bytes: "ä" is two (11 UTF-16 units)
    CHECK_STR(r.entities[0].data, "https://example.com/x?a=1");
}

TEST("mrkdwn: resolveTokens leaves mrkdwn marks literal") {
    auto r = mrkdwn::resolveTokens("*Where:* 3 < 5 and a > b");
    CHECK_STR(r.text, "*Where:* 3 < 5 and a > b");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: resolveTokens keeps a bare <word> literal") {
    auto r = mrkdwn::resolveTokens("value <placeholder> here");
    CHECK_STR(r.text, "value <placeholder> here");
    CHECK(r.entities.empty());
}

TEST("mrkdwn: resolveTokens expands mention and emoji") {
    auto r = mrkdwn::resolveTokens("hi <@U123|bob> :wave:");
    CHECK_STR(r.text, "hi bob :wave:");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::User);
    CHECK_STR(r.entities[0].data, "U123");
    CHECK(r.entities[1].kind == Kind::Emoji);
    CHECK_STR(r.entities[1].data, "wave");
}

TEST("mrkdwn: link label resolves emoji shortcodes") {
    auto r = mrkdwn::parse(
        "*<https://console.aws.example/pipe|:white_check_mark: AWS CodePipeline Notification>*"
    );
    CHECK_STR(r.text, ":white_check_mark: AWS CodePipeline Notification");
    REQUIRE(r.entities.size() == 3);
    CHECK(r.entities[0].kind == Kind::Bold); // Bold wraps Link wraps Emoji
    CHECK(r.entities[1].kind == Kind::Link);
    CHECK_STR(r.entities[1].data, "https://console.aws.example/pipe");
    CHECK(r.entities[2].kind == Kind::Emoji);
    CHECK_STR(r.entities[2].data, "white_check_mark");
    CHECK(r.entities[2].start == 0);
    CHECK(r.entities[2].length == 18);
}

TEST("mrkdwn: bare url label is never emoji-scanned") {
    auto r = mrkdwn::parse("<https://example.com/a:b:c>");
    CHECK_STR(r.text, "https://example.com/a:b:c");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Link);
}

TEST("mrkdwn: a url inside a label is not emoji-scanned either") {
    auto r = mrkdwn::parse("<https://example.com/p|https://example.com/a:b:c>");
    CHECK_STR(r.text, "https://example.com/a:b:c");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Link);
}

TEST("mrkdwn: resolveTokens skips colon pairs inside a raw url") {
    auto r = mrkdwn::resolveTokens("build https://ci.example/job/a:b:c :tada:");
    CHECK_STR(r.text, "build https://ci.example/job/a:b:c :tada:");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Emoji);
    CHECK_STR(r.entities[0].data, "tada");
}

TEST("mrkdwn: a label mixing a url and an emoji keeps the emoji") {
    auto r = mrkdwn::parse("<https://example.com/p|:tada: see https://example.com/a:b:c>");
    CHECK_STR(r.text, ":tada: see https://example.com/a:b:c");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::Link);
    CHECK(r.entities[1].kind == Kind::Emoji);
    CHECK_STR(r.entities[1].data, "tada");
    CHECK(r.entities[1].start == 0);
}

// ── Links to other messages ───────────────────────────────────────────────────

TEST("mrkdwn: a bare message permalink becomes a message link") {
    auto r = mrkdwn::parse("<https://cityteam.slack.com/archives/C1/p1786008939071009>");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::MessageLink);
    const auto ref = mrkdwn::refFromToken(r.entities[0].data);
    CHECK_STR(ref.conv, "C1");
    CHECK_STR(ref.ts, "1786008939.071009");
    CHECK(ref.author.empty());
    CHECK_STR(ref.host, "cityteam.slack.com");
}

TEST("mrkdwn: Slack's <url|url> echo of a permalink is still bare") {
    const std::string u = "https://cityteam.slack.com/archives/C1/p1786008939071009";
    auto              r = mrkdwn::parse("<" + u + "|" + u + ">");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::MessageLink);
}

TEST("mrkdwn: a permalink with real link text stays a plain link") {
    auto r = mrkdwn::parse("<https://cityteam.slack.com/archives/C1/p1786008939071009|see this>");
    CHECK_STR(r.text, "see this");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Link);
}

TEST("mrkdwn: msga's own thread link is a message link that keeps its label") {
    const std::string link = mrkdwn::threadLink("new-1a2b", "1786008939.071009");
    auto              r    = mrkdwn::parse("_see <" + link + "|its thread>._");
    CHECK_STR(r.text, "see its thread.");
    const auto e = std::find_if(r.entities.begin(), r.entities.end(), [](const auto &e) {
        return e.kind == Kind::MessageLink;
    });
    REQUIRE(e != r.entities.end());
    CHECK_STR(r.text.substr(e->start, e->length), "its thread");
    const auto ref = mrkdwn::refFromToken(e->data);
    CHECK_STR(ref.conv, "new-1a2b");
    CHECK_STR(ref.ts, "1786008939.071009");
    CHECK_STR(ref.threadTs, "1786008939.071009"); // opens the thread
    CHECK(ref.host.empty());                      // no permalink behind it
    CHECK_FALSE(mrkdwn::parseThreadLink("msga://thread/a/b/c").valid());
    CHECK_FALSE(mrkdwn::parseThreadLink("https://x.slack.com/archives/C1/p1").valid());
}

TEST("mrkdwn: permalink parsing edge cases") {
    auto ref = mrkdwn::parseMessageLink(
        "https://X.slack.com:443/archives/C9Z/p1786008939071009?thread_ts=1786008000.000100&cid=C9Z"
    );
    CHECK(ref.valid());
    CHECK_STR(ref.host, "x.slack.com");
    CHECK_STR(ref.threadTs, "1786008000.000100");
    // The root's own link repeats its ts: no thread target.
    ref = mrkdwn::parseMessageLink(
        "https://x.slack.com/archives/C1/p1786008939071009?thread_ts=1786008939.071009"
    );
    CHECK(ref.threadTs.empty());
    CHECK_FALSE(mrkdwn::parseMessageLink("https://x.slack.com/archives/C1").valid());
    CHECK_FALSE(
        mrkdwn::parseMessageLink("https://x.slack.com/archives/search/p1786008939071009").valid()
    );
    CHECK_FALSE(mrkdwn::parseMessageLink("https://evil.com/archives/C1/p1786008939071009").valid());
    CHECK_FALSE(
        mrkdwn::parseMessageLink("ftp://x.slack.com/archives/C1/p1786008939071009").valid()
    );
    CHECK_FALSE(mrkdwn::refFromToken("a/b/c").valid());
}

// ── Bare URLs (new: Slack wraps URLs in <>, but typed/fixture text may not) ──

TEST("mrkdwn: bare URLs become links, trimmed of sentence punctuation") {
    auto r = mrkdwn::parse("see https://example.com/a_(b)). ok");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].kind == Kind::Link);
    CHECK_STR(r.entities[0].data, "https://example.com/a_(b)");
    CHECK(r.entities[0].start == 4);
    // Inside inline code the URL is linked too, nested in the Code span.
    r = mrkdwn::parse("run `curl https://x.test/p` now");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::Code);
    CHECK(r.entities[1].kind == Kind::Link);
    CHECK_STR(r.entities[1].data, "https://x.test/p");
    // Not inside code blocks, tokens or bare schemes.
    CHECK(mrkdwn::parse("```https://x.test```").entities.size() == 1);
    CHECK(mrkdwn::parse("<https://x.test|https://y.test>").entities.size() == 1);
    CHECK(mrkdwn::parse("just https:// here").entities.empty());
    // A URL inside bold nests after the Bold span.
    r = mrkdwn::parse("*https://x.test*");
    REQUIRE(r.entities.size() == 2);
    CHECK(r.entities[0].kind == Kind::Bold);
    CHECK(r.entities[1].kind == Kind::Link);
}

TEST("mrkdwn: bareUrls finds the URLs a parser would link, for the composer") {
    const std::string text = "a https://x.test/p?q=1, then http://y.test/(b) and mailto: no";
    const auto        r    = mrkdwn::bareUrls(text);
    REQUIRE(r.size() == 2); // http(s) only: a mailto: is no bare link
    CHECK_STR(text.substr(r[0].from, r[0].to - r[0].from), "https://x.test/p?q=1");
    CHECK_STR(text.substr(r[1].from, r[1].to - r[1].from), "http://y.test/(b)");
    // The same ranges the parser links: one source of truth for both.
    const Rich parsed = mrkdwn::parse(text);
    REQUIRE(parsed.entities.size() == 2);
    for (size_t i = 0; i < r.size(); ++i) {
        CHECK(parsed.entities[i].start == r[i].from);
        CHECK(parsed.entities[i].start + parsed.entities[i].length == r[i].to);
    }
    // A trailing period, a closing parenthesis without one, and a bare scheme.
    CHECK(mrkdwn::bareUrls("see https://x.test.").front().to == 4 + 14);
    CHECK(mrkdwn::bareUrls("https://x.test)").front().to == 14);
    CHECK(mrkdwn::bareUrls("just https:// here").empty());
    CHECK(mrkdwn::bareUrls("nothing").empty());
}

TEST("mrkdwn: crafted input stays bounded") {
    std::string s;
    for (int i = 0; i < 200; ++i)
        s += "*_~";
    s += "x";
    for (int i = 0; i < 200; ++i)
        s += "~_*";
    auto r = mrkdwn::parse(s);
    CHECK(r.text.find('x') != std::string::npos);
    std::string colons(20000, ':');
    CHECK(mrkdwn::parse(colons).entities.empty());
    // Invalid UTF-8 never crashes the word-character test.
    CHECK(mrkdwn::parse("\xFF_\xC3_\x80_").text.size() > 0);
}

// ── Layout view ───────────────────────────────────────────────────────────────

TEST("mrkdwn: blocks split paragraphs, code, quotes and list items") {
    auto r = mrkdwn::parse(
        "Intro *line*\nsecond line\n```\ncode here\n```\n> quoted _it_\n> more\n"
        "1. first\n2. second\n• bullet\nafter"
    );
    const auto bl = mrkdwn::blocks(r);
    REQUIRE(bl.size() == 7);
    auto text = [&](const mrkdwn::Block &b) { return r.text.substr(b.start, b.end - b.start); };
    CHECK(bl[0].kind == mrkdwn::BlockKind::Paragraph);
    CHECK_STR(text(bl[0]), "Intro line\nsecond line");
    CHECK(bl[1].kind == mrkdwn::BlockKind::Code);
    CHECK_STR(text(bl[1]), "code here");
    CHECK(bl[2].quoted);
    CHECK_STR(text(bl[2]), "quoted it\nmore");
    CHECK(bl[3].kind == mrkdwn::BlockKind::ListItem);
    CHECK(bl[3].ordinal == 1);
    CHECK_STR(r.text.substr(bl[3].markerEnd, bl[3].end - bl[3].markerEnd), "first");
    CHECK(bl[4].ordinal == 2);
    CHECK(bl[5].kind == mrkdwn::BlockKind::ListItem);
    CHECK(bl[5].ordinal == 0);
    CHECK_STR(r.text.substr(bl[5].markerEnd, bl[5].end - bl[5].markerEnd), "bullet");
    CHECK(bl[6].kind == mrkdwn::BlockKind::Paragraph);
    CHECK_FALSE(bl[6].quoted);
    CHECK_STR(text(bl[6]), "after");
}

TEST("mrkdwn: the fixture's quote-in-the-middle message") {
    auto r = mrkdwn::parse(
        "Started on the new empty states. Sketch of the copy:\n> No projects yet. Create one."
        "\nToo chatty?"
    );
    const auto bl = mrkdwn::blocks(r);
    REQUIRE(bl.size() == 3);
    CHECK_FALSE(bl[0].quoted);
    CHECK(bl[1].quoted);
    CHECK_STR(r.text.substr(bl[2].start, bl[2].end - bl[2].start), "Too chatty?");
}

TEST("mrkdwn: runs flatten nested styles and targets") {
    auto r = mrkdwn::parse("a *b _c <https://x.test|d>_* `e` :wave::skin-tone-3: <@U1>");
    std::vector<mrkdwn::Run> rs;
    mrkdwn::runs(r, 0, uint32_t(r.text.size()), rs);
    auto text = [&](const mrkdwn::Run &x) { return r.text.substr(x.start, x.end - x.start); };
    // "a " | "b " | "c " | "d" | " " | "e" | " " | ":wave::skin-tone-3:" | " " | "@U1"
    REQUIRE(rs.size() == 10);
    CHECK_STR(text(rs[1]), "b ");
    CHECK(rs[1].style == mrkdwn::StyleBold);
    CHECK(rs[2].style == (mrkdwn::StyleBold | mrkdwn::StyleItalic));
    CHECK_STR(text(rs[3]), "d");
    REQUIRE(rs[3].entity >= 0);
    CHECK(r.entities[size_t(rs[3].entity)].kind == Kind::Link);
    CHECK(rs[5].style == mrkdwn::StyleCode);
    CHECK_STR(text(rs[7]), ":wave::skin-tone-3:");
    CHECK(rs[7].skinTone == 3);
    CHECK_STR(r.entities[size_t(rs[7].entity)].data, "wave");
    CHECK(r.entities[size_t(rs[9].entity)].kind == Kind::User);
    // Runs cover the range exactly, in order.
    uint32_t pos = 0;
    for (const auto &x : rs) {
        CHECK(x.start == pos);
        pos = x.end;
    }
    CHECK(pos == r.text.size());
}

TEST("link labels: shortened labels, URL labels, GIPHY media") {
    using namespace mrkdwn;
    const char *url = "https://github.com/lumen/atlas/pull/1482/files";
    CHECK(isShortenedUrlLabel("github.com/lumen/…/1482/files", url));
    CHECK(isShortenedUrlLabel("github.com/lumen/.../files", url));
    CHECK_FALSE(isShortenedUrlLabel("the PR…", url));
    CHECK_FALSE(isShortenedUrlLabel("github.com/lumen/atlas", url)); // no ellipsis
    CHECK(isUrlLabel("", url) && isUrlLabel(url, url));
    CHECK(isUrlLabel("github.com/lumen/atlas/pull/1482/files", url));
    CHECK_FALSE(isUrlLabel("Atlas PR", url));
    CHECK_STR(expandedLabel(url, 96), "github.com/lumen/atlas/pull/1482/files");
    CHECK_STR(expandedLabel(url, 12), "github.com/\xE2\x80\xA6");
    CHECK(isGiphyMediaUrl("https://media2.giphy.com/media/abc/giphy.gif?cid=1"));
    CHECK(isGiphyMediaUrl("https://i.giphy.com/xyz.webp"));
    CHECK(isGiphyMediaUrl("https://giphy.com/media/xyz"));
    CHECK_FALSE(isGiphyMediaUrl("https://giphy.com/gifs/cat-xyz"));
    CHECK_FALSE(isGiphyMediaUrl("https://notgiphy.com/a.gif"));
}

TEST("mrkdwn: openers without a closer on their line are scanned once") {
    // A line's failed search answers its later openers; the next line
    // looks again.
    {
        auto r = mrkdwn::parse("a _b_c _d *e ~f `g\n_i_ *j* ~k~ `l`");
        CHECK_STR(r.text, "a _b_c _d *e ~f `g\ni j k l");
        CHECK(r.entities.size() == 4);
    }
    // Pathological lines (tens of thousands of openers, no closer and no
    // '>' at all): linear, not quadratic (seconds before).
    std::string line;
    for (int i = 0; i < 40000; ++i)
        line += " _a_b <f";
    const int64_t t0 = base::monotonicMs();
    const Rich    r  = mrkdwn::parse(line);
    CHECK(base::monotonicMs() - t0 < 2000);
    CHECK(r.text.size() == line.size());
    CHECK(r.entities.empty());
}
