#include "base/cfstr.h"

namespace base {

CFStringRef cfString(std::string_view s) {
    return CFStringCreateWithBytes(
        nullptr,
        reinterpret_cast<const UInt8 *>(s.data()),
        CFIndex(s.size()),
        kCFStringEncodingUTF8,
        false
    );
}

std::string fromCFString(CFStringRef s) {
    if (!s)
        return {};
    if (const char *p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8))
        return p;
    const CFIndex max =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out(size_t(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8))
        return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

} // namespace base
