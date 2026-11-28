// Files an agent made, shown as attachments on the answer that names them.
//
// Claude Code records no list of the files a turn produced: the Write tool is
// one way, but a shell command (an ImageMagick convert, a heredoc) leaves only
// its command line. What an answer does say is where things are — a full path,
// or a folder and the bare names under it ("Files are in /tmp/x/: - a.png").
// So an answer's attachments are the media files it names that were made
// during its turn (modified between the turn's prompt and the answer): a file
// it merely refers to, made earlier, isn't an output.
//
// Each is copied into msga's cache (on a worker) the first time the answer is
// shown, under the session and the answer, so the attachment keeps showing
// what the agent made even after the file is changed or deleted (a job's tmp
// folder goes with the job). The copies go when the session is removed from
// msga. An answer whose named files were none of them made in its turn gets
// an empty index: never looked through again. One that names no file at all
// gets nothing written (most answers): naming none, it never makes any.
//
// An SVG is kept as the file it is (mime image/svg+xml, no size): there is no
// PNG encoder here to render a preview of it.
#pragma once

#include "app/model/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace claude {

// The existing files `text` names whose kind is shown as an attachment
// (images, PDFs, audio, video, HTML, CSV): absolute paths, and bare or
// relative names resolved against the folders the text names, then `cwd`.
// Absolute, in the order named, each once.
std::vector<std::string> mentionedFiles(std::string_view text, std::string_view cwd);
// False when `text` names no path of such a kind, so mentionedFiles finds
// nothing in it — told from the text alone, no file looked at.
bool                     mayNameFiles(std::string_view text);

struct OutputContext {
    std::string convId;        // whose copies they are (cleared with the session)
    std::string messageKey;    // the answer's own key (its record uuid, else ts)
    std::string cwd;           // for names relative to the session's folder
    int64_t     turnStart = 0; // epoch micros: the turn's prompt
    int64_t     date      = 0; // epoch micros: the answer
};

// An answer's attachments come in two halves (File::path is the copy):
// the copies made for the answer before, in *files (none when it made none):
// false when they're yet to be made (makeOutputs). A small read.
bool        cachedOutputs(const OutputContext &ctx, std::vector<model::File> *files);
// Copies the files `text` names that were made during its turn and writes the
// answer's index — an empty one when none of them were, so it's never looked
// for again (nothing at all when it names no file). Reads and writes up to
// kMaxFiles files: off the UI thread.
void        makeOutputs(std::string_view text, const OutputContext &ctx);
// The folder holding the answer's copies and index: which answer it is.
std::string outputsFolder(const OutputContext &ctx);

// Where the copies of `convId`'s outputs live (<dirs().cache>/files/<id>),
// and dropping them.
std::string outputsDir(std::string_view convId);
void        clearOutputs(std::string_view convId);
// Drops the copies of every session not in `keep` (conversation ids).
void        pruneOutputs(const std::vector<std::string> &keep);

// ── A file helper the module shares (base/file.h lacks it) ──────────────────
// `path` with '/' separators (on Windows), no "." or empty parts, ".."
// resolved lexically, no trailing '/' but on a root.
std::string cleanPath(std::string_view path);

} // namespace claude
