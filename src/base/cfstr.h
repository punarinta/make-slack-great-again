// CFString ↔ UTF-8 (macOS only), for base's CoreFoundation code.
#pragma once

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <string_view>

namespace base {

// A new CFString (the caller releases it); null when `s` isn't UTF-8.
CFStringRef cfString(std::string_view s);
// A CFString's text as UTF-8; "" for null.
std::string fromCFString(CFStringRef s);

} // namespace base

#endif
