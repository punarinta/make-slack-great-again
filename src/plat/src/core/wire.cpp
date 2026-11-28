#include "core/wire.h"

namespace plat::core {

void putU32(std::string &out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out += char((v >> (8 * i)) & 0xff);
}

uint32_t getU32(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(uint8_t(p[i])) << (8 * i);
    return v;
}

void putString(std::string &out, std::string_view s) {
    putU32(out, uint32_t(s.size()));
    out += s;
}

bool takeString(std::string_view &in, std::string *out) {
    if (in.size() < 4)
        return false;
    const uint32_t n = getU32(in.data());
    if (n > in.size() - 4)
        return false;
    out->assign(in.substr(4, n));
    in.remove_prefix(4 + size_t(n));
    return true;
}

} // namespace plat::core
