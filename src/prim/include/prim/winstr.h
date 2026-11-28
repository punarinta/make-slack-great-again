// UTF-8 ↔ UTF-16 at the Windows API boundary (Windows only; see prim's
// CMakeLists.txt): base declares them as base::wide/narrow, plat as
// win32::toWide/toUtf8.
#pragma once

#ifdef _WIN32

#include <string>
#include <string_view>

namespace prim {

// UTF-8 → UTF-16 ("" stays empty).
std::wstring wide(std::string_view s);
// UTF-16 → UTF-8. A wchar_t* converts: up to its NUL.
std::string  narrow(std::wstring_view w);

} // namespace prim

#endif
