// Every Claude Code session on this machine, for "Find a session" — what
// Claude Code's own /resume picks from: the transcripts under
// ~/.claude/projects/<folder>/<sessionId>.jsonl, whatever folder they ran in.
//
// Transcripts run to tens of megabytes, so only each file's two ends are read:
// the start for the first prompt, the end for the rest. Claude Code repeats
// the title records ("custom-title" from /rename, "ai-title") and the
// "last-prompt" record all along a transcript, and reads the title from the
// tail itself, so the tail holds the current ones. Plain file reading, safe
// on a worker thread.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace claude {

struct CatalogEntry {
    std::string sessionId;
    std::string transcriptPath;
    std::string cwd;
    std::string title;       // the /rename name, else Claude's own title; "" = none
    std::string firstPrompt; // one line
    std::string lastPrompt;  // one line
    std::string role;        // the team role it was started with (roles.h); "" = none
    std::string roleName;    // …as its prompt names it
    int64_t     modifiedMs = 0;
};

// Sessions with at least one prompt, newest first. `projectsDir` is
// Paths::projectsDir(). Blocking.
std::vector<CatalogEntry> scanCatalog(const std::string &projectsDir);

// One transcript's entry, read from its ends; false when it can't be read or
// holds no prompt.
bool readCatalogEntry(const std::string &transcriptPath, CatalogEntry &entry);

// One transcript's entry from its first and last bytes (the tail's first line
// is taken to be cut, and dropped); false when it holds no prompt at all.
bool catalogEntryFrom(std::string_view head, std::string_view tail, CatalogEntry &entry);

} // namespace claude
