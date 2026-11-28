// What the Claude Code modules share: where msga keeps its own files for the
// workspace, and the small types several modules pass around.
//
// The directories are set once, by whoever creates the backend (the accounts
// controller; tests point them at a temp dir), before any module touches a
// file. Nothing here depends on plat.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace claude {

struct Dirs {
    // msga's own data for the workspace: the team (team/<id>.md), the known
    // sessions, your profile — <dataDir>/claude-code (app/identity.h).
    std::string data;
    // Copies msga can make again or lose: pasted images (images/), files sent
    // to sessions (uploads/), files answers made (files/) —
    // <cacheDir>/claude-code.
    std::string cache;
};
void        setDirs(Dirs d);
const Dirs &dirs();

// "~/src/x" for a path under $HOME (native separators elsewhere).
std::string homeRelative(std::string_view path);

// Whether the image file at `path` (of type `mime`) shows as a picture: an
// SVG, or a format this build decodes. Anything else is a file card, never an
// empty preview.
bool showsAsPicture(std::string_view path, std::string_view mime);

// `s` cut to at most `max` code points, the last of them "…" when it's cut.
std::string ellipsized(std::string_view s, size_t max);

// The lines of a file (a transcript runs to tens of MB) from byte `from` on,
// read a megabyte at a time: only an unfinished line is carried from one read
// to the next. The last line counts without its newline too. Blocking.
class LineReader {
public:
    LineReader(std::string_view path, int64_t from) : _path(path), _at(from) {}
    // The next line, valid until the next call; false at the end, or when
    // the file can't be read (failed()).
    bool next(std::string_view *line);
    bool failed() const { return _failed; }

private:
    std::string _path, _buf, _chunk;
    int64_t     _at  = 0; // the file's next byte to read
    size_t      _pos = 0; // the next line's start in _buf
    bool        _eof = false, _failed = false;
};

// A slash command a session offers (Claude Code's own, a skill's, a project
// command), or one msga runs itself (source "msga").
struct SlashCommand {
    std::string name;          // without the slash: "compact"
    std::string desc;          // one line
    std::string usage;         // argument hint: "<question>"; may be empty
    std::string source;        // "builtin", "project", "user", "plugin", "msga", …
    bool        local = false; // msga answers it itself, nothing is sent (/status, /clear)
};

} // namespace claude
