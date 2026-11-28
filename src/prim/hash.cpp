#include "prim/hash.h"

namespace prim {

uint64_t fnv1a(std::string_view data, uint64_t h) {
    for (unsigned char c : data)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

} // namespace prim
