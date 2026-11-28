// Fuzzy subsequence matching for pick-lists driven by a search field (the
// Ctrl/Cmd+K switcher, issue #60): "xdg"
// finds "xd-general", "bb" finds "Bob Builder". Every query character must
// appear in the haystack in order, but not adjacently; the score ranks the
// candidates that pass.
//
// Scoring follows fzy's dynamic programme: the best alignment over all
// placements of the query characters, rewarding runs of adjacent matches (a
// plain substring beats a scattered one), matches that start a word (after a
// space, '-', '_', '.', ',', '/', '@', '#', ':'), camelCase humps and the
// very start of the haystack, while charging a small toll per skipped
// character so tighter, earlier alignments win. Case-insensitive (simple
// case folding, by code point).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace shell {

// nullopt when `query` is not a subsequence of `haystack`; else a score,
// higher is better, comparable only across haystacks for the same query. An
// empty query matches everything with 0.
std::optional<double> fuzzyScore(std::string_view query, std::string_view haystack);

// The same, for a pick-list scored against query after query: each side
// decoded and folded once (fuzzyQuery per keystroke, FuzzyText per name).
// fuzzyScore(fuzzyQuery(q), FuzzyText(h)) == fuzzyScore(q, h), always.
struct FuzzyText {
    FuzzyText() = default;
    explicit FuzzyText(std::string_view haystack);
    std::vector<uint32_t> folded; // code points, case-folded
    std::vector<uint8_t>  bonus;  // each one's start-of-word kind
};
std::vector<uint32_t> fuzzyQuery(std::string_view query); // folded code points
std::optional<double> fuzzyScore(const std::vector<uint32_t> &query, const FuzzyText &haystack);

} // namespace shell
