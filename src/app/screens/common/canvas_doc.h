// Canvas documents: the
// conversions between the HTML a Slack canvas file serves and the canvas
// markdown it is written back as, and the section diff that turns an edit
// into canvases.edit operations on the sections that changed.
//
// The editor (shell's CanvasPage) holds the markdown's block syntax as text
// ("## ", "- ", "1. ", "> ", "| a | b |") with inline formats; editorHtml
// builds that from the served HTML, markdown() turns it back.
//
// The diff's structural knowledge (verified against a real workspace): top-level <h1..h6 id>, <p
// id> (code: class="prettyprint"), <div data-section-style><ul id>…</ul></div> lists, and
// <blockquote> / <table> whose ids are only on their inner <p>s — those two are "fragile": fine as
// unchanged context, but an edit touching them (or needing one as an insert anchor) falls back to
// replacing the whole document. canvases.edit takes one change per call; a section replace reissues
// the ids inside it, so the base must be refetched after every save.
#pragma once

#include "app/model/backend.h"
#include "ui/textedit.h"

#include <string>
#include <string_view>
#include <vector>

namespace screens::canvas {

// Canvas HTML → the editor's text as HTML for TextEdit::insertHtml: a <p>
// per block with its markdown prefix, inline formats kept. A leading <h1>
// whose text is one of `titles` is taken out into *title.
std::string
editorHtml(std::string_view html, const std::vector<std::string> &titles, std::string *title);
// The editor's text back to canvas markdown: block prefixes as typed,
// formats as **b** _i_ ~~s~~ `c` [t](u), a blank line between paragraphs.
// `runs` in order, as TextEdit::runs() gives them.
std::string markdown(const std::string &text, const std::vector<ui::TextEdit::Run> &runs);
// The same from rich::fromHtml's per-byte formats.
std::string markdown(
    const std::string &text, const std::vector<uint16_t> &fmt, const std::vector<std::string> &links
);

// One section of a canvas at the diff's granularity.
struct Chunk {
    enum class Kind : uint8_t { Heading, Para, List, Quote, Table };
    Kind        kind    = Kind::Para;
    bool        fragile = false; // Quote/Table: never edited in place or anchored on
    std::string id;              // base side: the section id ops address; "" on the editor side
    std::string md;              // its markdown; equal = unchanged
};
// The editor's markdown (markdown()) cut into chunks.
std::vector<Chunk> documentChunks(std::string_view markdown);
// The served HTML (titles: as editorHtml) cut into chunks with their ids.
// False for a structure the diff can't address (unknown top-level elements,
// sections without ids): save the whole document instead.
bool               baseChunks(
    std::string_view html, const std::vector<std::string> &titles, std::vector<Chunk> *out
);
using Change = model::Backend::CanvasChange;
// The fewest operations turning base into current, ordered inserts →
// replaces → deletes (every id they name is still alive when it applies).
// False when no safe sequence exists (a fragile chunk changed, no anchor).
bool diff(
    const std::vector<Chunk> &base, const std::vector<Chunk> &current, std::vector<Change> *ops
);

} // namespace screens::canvas
