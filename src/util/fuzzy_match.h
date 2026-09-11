// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QString>

#include <optional>

// Fuzzy subsequence matching for pick-lists driven by a search field (the
// Ctrl/Cmd+K switcher, issue #60): "xdg" finds "xd-general", "bb" finds
// "Bob Builder". Every query character must appear in the haystack in order,
// but not adjacently; the score ranks the candidates that pass.
//
// Scoring follows fzy's dynamic programme: the best alignment over all
// placements of the query characters, rewarding runs of adjacent matches (a
// plain substring beats a scattered one), matches that start a word (after a
// space, '-', '_', '.', ',', '/', '@', '#', ':'), and matches at the very start
// of the haystack, while charging a small toll per skipped character so tighter,
// earlier alignments win. Case-insensitive.
namespace Fuzzy {

// std::nullopt when the query is not a subsequence of the haystack; otherwise a
// score where higher is better. Only comparable across haystacks for the same
// query. An empty query matches everything with a score of 0.
std::optional<double> score(const QString &query, const QString &haystack);

// Convenience for callers that only need the yes/no.
inline bool matches(const QString &query, const QString &haystack) {
    return score(query, haystack).has_value();
}

} // namespace Fuzzy
