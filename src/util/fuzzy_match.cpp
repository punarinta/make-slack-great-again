// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "fuzzy_match.h"

#include <QChar>

#include <algorithm>
#include <limits>
#include <vector>

namespace {

// Weights borrowed from fzy: a consecutive match (1.0) outranks any single
// boundary bonus, so a verbatim substring always beats a scattered alignment
// of the same characters; gap penalties are two orders smaller so they only
// break ties between otherwise equal alignments. Unlike fzy, characters left
// over AFTER the last match cost nothing: "des" scores "design-review" and
// "design-backend" the same, so the caller's own order (recency in the
// switcher) decides, instead of the shorter name always winning. Typing the
// whole name still puts that conversation first via kExact.
constexpr double kMin              = -std::numeric_limits<double>::infinity();
constexpr double kGapLeading       = -0.005;
constexpr double kGapTrailing      = 0.0;
constexpr double kGapInner         = -0.01;
constexpr double kExact            = 1.0; // query == whole haystack
constexpr double kMatchConsecutive = 1.0;
constexpr double kMatchStart       = 0.9; // first character of the haystack
constexpr double kMatchWord        = 0.8; // after a separator
constexpr double kMatchCamel       = 0.7; // lower→Upper transition
constexpr double kMatchDot         = 0.6; // after '.'

// Beyond this the O(n·m) table is not worth it for a pick-list; names are short.
constexpr int kMaxHaystack = 512;

bool isSeparator(QChar c) {
    switch (c.unicode()) {
    case ' ':
    case '-':
    case '_':
    case ',':
    case '/':
    case '\\':
    case '@':
    case '#':
    case ':':
    case '(':
    case ')':
    case '[':
    case ']':
        return true;
    default:
        return c.isSpace() || c.isPunct();
    }
}

// Positional bonus for a match at index j, from the character before it.
double bonusAt(const QString &original, int j) {
    if (j == 0)
        return kMatchStart;
    const QChar prev = original[j - 1];
    const QChar cur  = original[j];
    if (prev == QChar('.'))
        return kMatchDot;
    if (isSeparator(prev))
        return kMatchWord;
    if (prev.isLower() && cur.isUpper())
        return kMatchCamel;
    return 0.0;
}

// Cheap gate: is the (lowercased) query a subsequence of the (lowercased)
// haystack at all? Most candidates fail here and never reach the table.
bool isSubsequence(const QString &q, const QString &h) {
    int qi = 0;
    for (int hi = 0; hi < h.size() && qi < q.size(); ++hi)
        if (h[hi] == q[qi])
            ++qi;
    return qi == q.size();
}

} // namespace

std::optional<double> Fuzzy::score(const QString &query, const QString &haystack) {
    if (query.isEmpty())
        return 0.0;
    if (haystack.isEmpty())
        return std::nullopt;

    const QString q = query.toLower();
    const QString h = haystack.toLower();
    if (q.size() > h.size() || !isSubsequence(q, h))
        return std::nullopt;
    if (h.size() > kMaxHaystack)
        return 0.0; // matched, but not worth ranking

    const int n = q.size();
    const int m = h.size();

    std::vector<double> bonus(m);
    for (int j = 0; j < m; ++j)
        bonus[j] = bonusAt(haystack, j);

    // M[j]: best score with q[i] matched exactly at h[j].
    // D[j]: best score with q[0..i] consumed somewhere within h[0..j].
    // Rows are rolled: only the previous row of each is needed.
    std::vector<double> prevM(m, kMin), prevD(m, kMin), curM(m), curD(m);

    for (int i = 0; i < n; ++i) {
        const double gap       = (i == n - 1) ? kGapTrailing : kGapInner;
        double       prevScore = kMin;
        for (int j = 0; j < m; ++j) {
            if (q[i] == h[j]) {
                double s = kMin;
                if (i == 0) {
                    s = j * kGapLeading + bonus[j];
                } else if (j > 0) {
                    s = std::max(prevD[j - 1] + bonus[j], prevM[j - 1] + kMatchConsecutive);
                }
                curM[j]   = s;
                prevScore = std::max(s, prevScore + gap);
                curD[j]   = prevScore;
            } else {
                curM[j]   = kMin;
                prevScore = prevScore + gap;
                curD[j]   = prevScore;
            }
        }
        std::swap(prevM, curM);
        std::swap(prevD, curD);
    }

    return prevD[m - 1] + (n == m ? kExact : 0.0);
}
