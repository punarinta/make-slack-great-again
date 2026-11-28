#include "core/transfer.h"

#include "core/strings.h"

namespace plat::core {

using prim::iequals;

bool isTextMime(std::string_view m) {
    if (m == "UTF8_STRING" || m == "STRING" || m == "TEXT")
        return true;
    const size_t semi = m.find(';');
    if (!iequals(trim(m.substr(0, semi)), "text/plain"))
        return false;
    if (semi == std::string_view::npos)
        return true;
    const std::string_view param = trim(m.substr(semi + 1));
    const size_t           eq    = param.find('=');
    if (eq == std::string_view::npos || !iequals(trim(param.substr(0, eq)), "charset"))
        return false;
    std::string_view cs = trim(param.substr(eq + 1));
    if (cs.size() >= 2 && cs.front() == '"' && cs.back() == '"')
        cs = cs.substr(1, cs.size() - 2);
    return iequals(cs, "utf-8") || iequals(cs, "utf8");
}

std::vector<std::string> parseUriList(std::string_view list) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start < list.size()) {
        size_t end = list.find('\n', start);
        if (end == std::string_view::npos)
            end = list.size();
        std::string_view line = list.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0' || line.back() == ' '))
            line.remove_suffix(1);
        if (!line.empty() && line[0] != '#')
            out.emplace_back(line);
        start = end + 1;
    }
    return out;
}

DropAction preferredAction(uint32_t allowed) {
    return (allowed & ActCopy)   ? DropAction::Copy
           : (allowed & ActMove) ? DropAction::Move
           : (allowed & ActLink) ? DropAction::Link
                                 : DropAction::None;
}

std::string fileUri(std::string_view absPath) {
    return "file://" + percentEncode(absPath, "/");
}

std::string pathFromFileUri(std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    if (uri.substr(0, scheme.size()) != scheme)
        return {};
    uri.remove_prefix(scheme.size());
    if (uri.substr(0, 9) == "localhost")
        uri.remove_prefix(9);
    if (uri.empty() || uri[0] != '/')
        return {}; // file://otherhost/… is not ours to open
    std::string out = percentDecode(uri);
    if (out.find('\0') != std::string::npos)
        return {}; // no path can hold a NUL
    return out;
}

} // namespace plat::core
