// FNV-1a, 64-bit: the backends' allocation-free hash for names that must be
// short (socket paths, pipe names, monitor ids). Not cryptographic. It is
// prim's (src/prim), the one the app side has as crypto::fnv1a, with the same
// basis (a digit short of FNV's published one; see prim/hash.h): the
// instance socket and pipe names derived from it must not change.
#pragma once

#include "prim/hash.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace plat::core {

using prim::fnv1a; // fnv1a(std::string_view)
inline uint64_t fnv1a(const void *data, size_t n) {
    return prim::fnv1a(std::string_view(static_cast<const char *>(data), n));
}

} // namespace plat::core
