// Text smileys (":)", ":D", "<3") become emoji as they are typed, as Slack's
// own client does: what the box shows is what is sent. The pairs are the
// user's, one to a line in Settings.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mrkdwn {

// One pair: the text typed and what it stands for.
struct Smiley {
    std::string from, to;
};

// The usual set, as the settings hold it: one pair to a line.
std::string_view defaultSmileyMap();

// The pairs `map` lists, one to a line: the smiley, whitespace, then the emoji
// — its shortcode ("slightly_smiling_face", the colons optional) or the
// character itself. Blank lines, lines starting with "#" and lines without a
// second word are skipped. A shortcode this client does not know is kept as
// typed, since that is the form Slack needs for a custom emoji. An empty `map`
// is the usual set.
std::vector<Smiley> parseSmileys(std::string_view map);

// What finished a smiley: where it sits in the text, how long it is and what it
// stands for. `len == 0` is no smiley at all.
struct SmileyHit {
    uint32_t    at = 0, len = 0; // byte range of the text typed
    std::string glyph;           // the emoji to put there
    explicit    operator bool() const { return len != 0; }
};

// The smiley that ends at `stop` — the one the text up to there has just closed:
// a space or punctuation at `stop` ends a word while typing, and the end of the
// message (`stop == text.size()`) ends it too. The longest match first, and only
// where it stands as a word of its own: "a:)" is a word with a colon in it, and
// so is ":D" in ":Date:". Code spans and fenced blocks are left alone — a smiley
// in code is code. An empty `map` is the usual set.
SmileyHit smileyEndingAt(std::string_view text, size_t stop, std::string_view map);

} // namespace mrkdwn