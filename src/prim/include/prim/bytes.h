// Little- and big-endian integer loads from a byte buffer (file headers,
// sfnt tables, WAV chunks). Byte by byte, so any alignment and any host
// order; the compiler turns each into one load where it can. The caller
// checks the bounds.
#pragma once

#include <cstdint>

namespace prim {

inline uint32_t le16(const void *p) {
    const auto *b = static_cast<const uint8_t *>(p);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8;
}
inline uint32_t le24(const void *p) {
    const auto *b = static_cast<const uint8_t *>(p);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16;
}
inline uint32_t le32(const void *p) {
    const auto *b = static_cast<const uint8_t *>(p);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
inline uint32_t be16(const void *p) {
    const auto *b = static_cast<const uint8_t *>(p);
    return uint32_t(b[0]) << 8 | uint32_t(b[1]);
}
inline uint32_t be32(const void *p) {
    const auto *b = static_cast<const uint8_t *>(p);
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
}

} // namespace prim
