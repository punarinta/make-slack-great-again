// Slack mrkdwn → rich text the UI renders.
//
// Two layers:
//  1. parse(): plain UTF-8 text plus nested entity spans (bold, links,
//     mentions, emoji, code, quotes…), parents before children, with byte
//     offsets.
//  2. blocks() + runs(): the layout view of that result — paragraphs, code
//     blocks, list items and quoted blocks, each flattened into
//     non-overlapping styled runs that map 1:1 onto text::AttributedText
//     spans.
//
// Slack is its own grammar, not Markdown: *bold* _italic_ ~strike~ `code`,
// ```pre```, "> " quotes (one level, see kMaxQuoteDepth), <@U…>, <#C…|name>,
// <!here>, <!channel>, <!subteam^S…|@handle>, <!date^ts^{format}|fallback>,
// <url|label>, :shortcodes:, and &lt; &gt; &amp; escapes (plus msga's
// "&#NN;" for printable ASCII, which Slack never sends: its '&' is "&amp;").
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mrkdwn {

enum class Kind : uint8_t {
    Bold,
    Italic,
    Underline, // __text__
    Strike,
    Code,
    Pre,         // block-level ``` code
    Quote,       // block-level "> " quote; data unused
    Link,        // data = URL
    User,        // data = user id (U…/W…); text = "@U…" or the label
    Channel,     // data = conversation id; text = "#name"
    Here,        // <!here>, <!everyone>, unknown <!cmd>; data = the command for the latter
    ChannelCmd,  // <!channel>
    Emoji,       // data = shortcode name ("rocket"); text = ":rocket:"
    MessageLink, // bare permalink, or msga's labelled threadLink; data =
                 // host/conv/ts/threadTs/author (see parseMessageLink)
    Usergroup,   // data = S… id; text = "@handle" (the UI may swap in the live handle)
};

struct Entity {
    Kind        kind;
    uint32_t    start = 0, length = 0; // byte offsets into Rich::text
    std::string data;
    uint32_t    end() const { return start + length; }
};

struct Rich {
    std::string         text;
    std::vector<Entity> entities; // parents before children (offset asc, longer first)
};

// Full mrkdwn: marks, tokens, emoji, quotes, code, entities; bare http(s)
// URLs in plain text (and inside `code`) become Link entities too.
Rich        parse(std::string_view mrkdwn);
// Only <…> tokens and :emoji: — for already-structured runs (rich_text "text"
// elements) whose emphasis comes from a style object. *_~` stay literal and a
// bare "<word>" without a URL scheme stays literal.
Rich        resolveTokens(std::string_view src);
// &lt; &gt; &amp; → < > & (&amp; last, so "&amp;lt;" is the literal "&lt;"),
// and msga's "&#42;"-style references (printable ASCII: a mark character
// meant literally, as a backend's words of its own escape it) to theirs.
std::string decodeEntities(std::string_view s);
// The other way: & < > → &amp; &lt; &gt;, the three Slack escapes (text sent
// as mrkdwn, a plain_text object's text). decodeEntities undoes it.
std::string escapeEntities(std::string_view s);
// A URL with a scheme that is linked ("://", mailto:, tel:): what tells a
// <url|label> token from a literal "<word>" (here and in markdown.cpp).
bool        looksLikeUrl(std::string_view s);
// The bare http(s) URLs in plain text — the ones the parser links — as byte
// ranges [from, to): a scheme and the word after it, the sentence punctuation
// Slack leaves out trimmed off. The composer links what is typed or pasted
// with this, so a URL reads as a link before it is sent.
struct BareUrl {
    uint32_t from = 0, to = 0;
};
std::vector<BareUrl> bareUrls(std::string_view text);
// A code fence's info string that is a language name ("js", "c++",
// "objective-c"): ^[A-Za-z][A-Za-z0-9_+#.-]{0,29}$.
bool                 isLanguageHint(std::string_view info);

// Slack renders a single quote level; deeper '>' stays literal text. It also
// keeps pathological input ("> > > > …") from building deep layouts.
constexpr int kMaxQuoteDepth = 1;
// Hard cap on recursion for nested inline marks (crafted input).
constexpr int kMaxParseDepth = 32;

// ── Message permalinks ──────────────────────────────────────────────────────
struct MessageRef {
    std::string host, conv, ts, threadTs, author;
    bool        valid() const { return !conv.empty() && !ts.empty(); }
};
// https://<team>.slack.com/archives/<conv>/p<16 digits>[?thread_ts=…]
MessageRef  parseMessageLink(std::string_view url);
std::string refToToken(const MessageRef &ref); // '/'-joined, 5 fields
MessageRef  refFromToken(std::string_view token);
// The permalink a ref came from (a link of a workspace we can't open goes to
// the browser). "" without a host.
std::string messagePermalink(const MessageRef &ref);
// msga's own link to a thread, for a backend's words of its own
// ("<msga://thread/<conv>/<root ts>|its thread>"): it parses to a MessageLink
// that keeps its label and opens the thread in place. No host: there is no
// permalink behind it (an agent session's thread has none).
std::string threadLink(std::string_view conv, std::string_view rootTs);
MessageRef  parseThreadLink(std::string_view url);

// ── Layout view ─────────────────────────────────────────────────────────────
enum class BlockKind : uint8_t {
    Paragraph, // one or more lines ('\n' inside is a hard line break)
    Code,      // a ``` block: monospace, own background, no wrapping of marks
    ListItem,  // a line starting with "• ", "◦ ", "- " or "1. " / "1) "
};

struct Block {
    BlockKind kind    = BlockKind::Paragraph;
    bool      quoted  = false;    // inside a "> " quote (draw the bar)
    uint8_t   indent  = 0;        // ListItem: nesting from leading spaces (0-3)
    uint16_t  ordinal = 0;        // ListItem: 1.. for "1. ", 0 for bullets
    uint32_t  start = 0, end = 0; // byte range in Rich::text (trailing '\n' excluded)
    // ListItem: end of the marker ("  2. "); the UI hangs the text after it.
    uint32_t  markerEnd = 0;
};

std::vector<Block> blocks(const Rich &r);

enum Style : uint16_t {
    StyleBold      = 1,
    StyleItalic    = 2,
    StyleUnderline = 4,
    StyleStrike    = 8,
    StyleCode      = 16, // inline `code` (Code blocks are BlockKind::Code instead)
};

// A maximal stretch of text with one style and one target.
struct Run {
    uint32_t start = 0, end = 0;
    uint16_t style    = 0; // Style bits
    // Index of the innermost Link / User / Channel / Here / ChannelCmd /
    // Emoji / MessageLink / Usergroup entity covering the run, or -1.
    int32_t  entity   = -1;
    // Emoji followed directly by :skin-tone-N: is merged into one run (the
    // text covers both shortcodes); 2-6, or 0.
    uint8_t  skinTone = 0;
};

// Runs covering [start, end) exactly, in order (e.g. a Block's range).
void runs(const Rich &r, uint32_t start, uint32_t end, std::vector<Run> &out);

} // namespace mrkdwn
