// UTF-8 ↔ UTF-16 at the Windows API boundary (Windows only). Everything in
// msga is UTF-8; the W functions take and give UTF-16.
#pragma once

#ifdef _WIN32

#include "prim/winstr.h"

#include <string>
#include <string_view>

namespace base {

// UTF-8 → UTF-16 ("" stays empty). prim's, shared with plat, as is narrow.
using prim::wide;
// The same with every '/' as '\': a path for the file API (long "\\?\"
// paths and some shell calls take no '/').
std::wstring widePath(std::string_view s);
// UTF-16 → UTF-8. A wchar_t* converts: up to its NUL.
using prim::narrow;

} // namespace base

#endif
