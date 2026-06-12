// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Section-level diffing between the canvas HTML Slack serves and the locally
// edited QTextDocument, expressed as minimal canvases.edit operations.
//
// All structural knowledge here is empirical (verified against a real
// workspace, June 2026):
//  - Server HTML: <div class="quip-canvas-content"> whose top-level children
//    are <h1..h6 id>, <p id> (code blocks: class="prettyprint", lines joined
//    with <br>), <div data-section-style><ul id><li id>…</ul></div> for lists,
//    <blockquote> and <table> (NO ids of their own — only their inner <p>s).
//  - The canvas *title* renders as a leading <h1> whose section id is stable;
//    "rename" updates it in place (or prepends one when missing), and a
//    whole-document "replace" leaves it untouched.
//  - "replace" on a section id rewrites that section and INVALIDATES the ids
//    inside it (lists get a whole new id set) — the base must be refreshed
//    after every successful save.
//  - Replacing a blockquote's inner <p> with quote markdown nests/duplicates
//    instead of replacing — quotes and tables are therefore "fragile": valid
//    as unchanged context, but any edit touching them (or needing them as an
//    insert anchor) forces the whole-document fallback.
//  - canvases.edit accepts at most ONE change per call; sequences are sent
//    one call each (PublicBackend handles that).
#pragma once

#include "backend/domain.h"

#include <QString>
#include <optional>
#include <vector>

class QTextDocument;

namespace CanvasDiff {

struct Chunk {
    enum class Kind { Heading, Para, List, Quote, Table };
    Kind    kind = Kind::Para;
    QString id;              // base side: section id usable in ops; empty on the doc side
    bool    fragile = false; // Quote/Table: cannot be edited in place or anchored on
    QString md;              // normalized markdown; equality = "section unchanged"

    bool operator==(const Chunk &) const = default;
};

// Strips the leading <h1> when its text matches one of titleCandidates (the
// file title and/or the locally known title) — that h1 is the rendered canvas
// title, not content. Returns {titleText, remainingHtml}; empty titleText
// when nothing was stripped.
std::pair<QString, QString> splitTitleH1(const QString &html, const QStringList &titleCandidates);

// Parses server canvas body HTML (title h1 already removed) into chunks.
// nullopt = unrecognized structure; caller must use the whole-doc fallback.
std::optional<std::vector<Chunk>> parseBaseChunks(const QString &bodyHtml);

// Chunks the edited document at the same granularity (ids stay empty).
std::vector<Chunk> documentChunks(QTextDocument *doc);

// Minimal ops turning base into current. nullopt = not expressible safely
// (a fragile chunk changed, no usable insert anchor, …) → whole-doc fallback.
// Ops are ordered inserts → replaces → deletes so every referenced id is
// still alive when its op applies.
std::optional<std::vector<CanvasChange>>
diff(const std::vector<Chunk> &base, const std::vector<Chunk> &current);

// Shared markdown normalization (also used by tests).
QString normalizeMd(QString md);

} // namespace CanvasDiff
