// File helpers the freedesktop pieces share (XDG directories, URL-scheme
// registration, sound themes, shared-memory buffers). POSIX calls, not <fstream>/<filesystem>:
// those cost a static binary ~400 KB of locale and filesystem code.
#pragma once

#include "prim/file.h"

#include <cstddef>
#include <optional>
#include <string>

namespace plat::linux_files {

// The whole regular file; nullopt when it cannot be read.
std::optional<std::string> readFile(const std::string &path);
using prim::file::isDir;              // follows symlinks
bool isFile(const std::string &path); // a regular file, ditto
// A close-on-exec fd of `bytes` of shared memory to mmap and hand to the
// display server (wl_shm pools, MIT-SHM fd passing); -1 on failure.
// memfd_create (Linux, FreeBSD 13+), else an unlinked file in
// $XDG_RUNTIME_DIR or /tmp (OpenBSD, which has no anonymous memory files).
int  sharedMemoryFd(const char *name, size_t bytes);

} // namespace plat::linux_files
