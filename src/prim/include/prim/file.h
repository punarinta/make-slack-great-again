// POSIX file primitives (see prim's CMakeLists.txt): base declares them as
// file::…, plat's freedesktop pieces call them here. Not on Windows, where base has
// its own wide-path versions. Paths are UTF-8 with '/' separators.
#pragma once

#ifndef _WIN32

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace prim::file {

// Up to `maxBytes` from byte `offset` on (fewer at the end of the file; none
// past it) — the tail a growing log appended, or a bounded read of a file
// that should be small. False (and *out empty) when it can't be read.
bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out);
// Writes `data` so that readers see either the old or the new content, never
// a torn one: a temp file in the same directory, flushed to disk, then
// renamed over the target. Creates missing parent directories. `durable`
// false skips the flush: still atomic for a reader, but a power loss may
// leave the old content or an empty file. For data that can be rebuilt
// (caches, downloaded images).
bool writeAtomic(
    std::string_view path, std::string_view data, int mode = 0644, bool durable = true
);
bool isDir(std::string_view path); // follows symlinks
// mkdir -p. True if the directory exists afterwards.
bool makeDirs(std::string_view path);

// The running executable's absolute path; "" when the OS won't say. Linux
// reads /proc/self/exe, FreeBSD and DragonFly ask sysctl. macOS (dyld) and
// OpenBSD are asked once, before main, so a relative start path still means
// the directory the process started in: OpenBSD keeps no path for a running
// image, so there it is argv[0] (from sysctl), resolved against that
// directory when it has a '/', else searched on $PATH — wrong if the binary
// has moved since, or if the launcher passed an argv[0] that is not the
// program's path.
std::string executablePath();

} // namespace prim::file

#endif
