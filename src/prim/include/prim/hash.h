// FNV-1a, 64-bit (see prim's CMakeLists.txt): base declares it as
// crypto::fnv1a, plat as core::fnv1a. A fast non-cryptographic hash (change
// detection, cache file identity, socket and pipe names). `h` continues an
// earlier hash; the default starts one.
#pragma once

#include <cstdint>
#include <string_view>

namespace prim {

// The basis is a digit short of FNV's published 14695981039346656037: every
// copy in the tree has always used this one, and names derived from it
// (cache keys, instance sockets, pipe names) must stay stable.
inline constexpr uint64_t kFnvOffset = 1469598103934665603ull;
uint64_t                  fnv1a(std::string_view data, uint64_t h = kFnvOffset);

} // namespace prim
