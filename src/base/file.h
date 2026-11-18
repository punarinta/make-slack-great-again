// File helpers. Paths are UTF-8 with '/' separators ('\' also accepted on
// Windows, where they are converted to wide strings at the OS boundary).
#pragma once

#include "prim/file.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace file {

// Reads the whole file into *out. False (and *out empty) on any error.
bool readAll(std::string_view path, std::string *out);
// Up to `maxBytes` from byte `offset` on (fewer at the end of the file; none
// past it) — the tail a growing log appended, or a bounded read of a file
// that should be small. False (and *out empty) when it can't be read.
#ifdef _WIN32
bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out);
#else
using prim::file::readRange; // the POSIX body is prim's, shared with plat
#endif
// Replaces an existing file's content in place (truncate, then write): the
// same file, so a process holding it open keeps appending to it. Not atomic;
// prefer writeAtomic unless that matters.
bool overwrite(std::string_view path, std::string_view data);
// Writes `data` so that readers see either the old or the new content, never
// a torn one: a temp file in the same directory, flushed to disk, then
// renamed over `path`. Creates missing parent directories.
// `mode`: POSIX permissions of a newly written file (0600 for secrets).
// `durable` false skips the flush to disk (5-50 ms): still never torn for a
// reader, but a power loss may leave the old content or an empty file. For
// data that can be rebuilt (caches, downloaded images).
#ifdef _WIN32
bool writeAtomic(
    std::string_view path, std::string_view data, int mode = 0644, bool durable = true
);
#else
using prim::file::writeAtomic;
#endif

bool    exists(std::string_view path);
int64_t size(std::string_view path); // -1 when missing
#ifdef _WIN32
bool isDir(std::string_view path);
// mkdir -p. True if the directory exists afterwards.
bool makeDirs(std::string_view path);
#else
using prim::file::isDir;
using prim::file::makeDirs; // mkdir -p; true if the directory exists afterwards
#endif
bool    remove(std::string_view path); // a file or an empty directory
// rm -r: a file, or a directory with everything in it. Links (and Windows
// junctions) are removed as themselves, never followed. True when nothing
// is left at `path` (also when nothing was there).
bool    removeTree(std::string_view path);
// The bytes of the regular files under `path` (a file: its size), links
// not followed; 0 when there is nothing.
int64_t treeBytes(std::string_view path);
// Copies a regular file's bytes to `to` (atomically, as writeAtomic).
bool    copy(std::string_view from, std::string_view to);
// Sets the modification time to now (LRU disk caches stamp "last used").
bool    touch(std::string_view path);

// What the file system says about a path (links followed).
struct Stat {
    int64_t  size        = 0; // bytes; 0 for a directory
    int64_t  mtimeMicros = 0; // last modification, unix microseconds
    // Creation, where the file system keeps it (Windows, macOS, Linux statx,
    // FreeBSD, OpenBSD's FFS2); else mtimeMicros.
    int64_t  birthMicros = 0;
    bool     isDir       = false;
    // Which file it is (POSIX st_dev, st_ino): a file replaced by another of
    // the same size within the clock's resolution still differs here. 0
    // where the system doesn't say (Windows, a port).
    uint64_t device = 0, inode = 0;
};
// False (*out untouched) when the path is missing or can't be looked at.
bool stat(std::string_view path, Stat *out);

// One directory entry ("." and ".." are never listed).
struct DirEntry {
    std::string name;
    int64_t     size   = 0; // bytes; 0 for directories
    int64_t     mtime  = 0; // last modification, unix seconds
    bool        isDir  = false;
    bool        hidden = false; // dot file, or the Windows hidden attribute
};
// The entries of `dir` in OS order (symlinks followed). False when it
// cannot be read (missing, not a directory, no permission).
bool listDir(std::string_view dir, std::vector<DirEntry> *out);

// ── Path strings (pure, no I/O) ─────────────────────────────────────────────
std::string_view dirName(std::string_view path);   // "a/b/c.txt" → "a/b"; "c" → ""
std::string_view baseName(std::string_view path);  // "a/b/c.txt" → "c.txt"
std::string_view extension(std::string_view path); // "c.tar.gz" → "gz" (no dot); "" if none
std::string      join(std::string_view dir, std::string_view rel);
bool             isAbsolute(std::string_view path);
// `rel` resolved against `base` unless it is already absolute.
std::string      resolve(std::string_view base, std::string_view rel);
// The current directory joined in front of a relative path.
std::string      absolute(std::string_view path);

// An absolute local path as a file:// URL: "/a/b c" → "file:///a/b%20c", "C:\x" → "file:///C:/x", a
// UNC
// "//srv/share/x" → "file://srv/share/x"; bytes outside RFC 3986's path
// characters (spaces, '%', '#', '?', non-ASCII UTF-8) percent-encoded.
std::string toFileUrl(std::string_view path);
// The local path of a file:// URL: percent-decoded,
// "localhost" dropped, "file:///C:/x" → "C:/x" and another host → UNC on
// Windows. "" when it isn't a file:// URL.
std::string fromFileUrl(std::string_view url);

} // namespace file
