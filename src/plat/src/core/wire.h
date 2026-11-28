// The pieces of the single-instance messages a later launch sends to the
// running one: little-endian u32s and u32-length-prefixed byte strings. Each
// backend keeps its own framing around them (magic, version, length): a new
// secondary must stay readable for a primary that is already running an
// older build, so none of those formats may change.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace plat::core {

void     putU32(std::string &out, uint32_t v);
uint32_t getU32(const char *p); // reads 4 bytes
// u32 length, then the bytes.
void     putString(std::string &out, std::string_view s);
// Takes one putString() string off the front of `in`; false (`in` untouched)
// when it is truncated.
bool     takeString(std::string_view &in, std::string *out);

} // namespace plat::core
