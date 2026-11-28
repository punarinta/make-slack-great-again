#include "base/winstr.h"

namespace base {

std::wstring widePath(std::string_view s) {
    std::wstring w = wide(s);
    for (auto &c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

} // namespace base
