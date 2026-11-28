#include "core/strings.h"

#include <algorithm>

namespace plat::core {

namespace {

bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

} // namespace

bool validScheme(std::string_view s) {
    if (s.empty() || !isAlpha(s[0]))
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return isAlpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    });
}

void rememberScheme(std::vector<std::string> &schemes, std::string_view scheme) {
    std::string s = asciiLower(scheme);
    if (std::find(schemes.begin(), schemes.end(), s) == schemes.end())
        schemes.push_back(std::move(s));
}

bool isSchemeUrl(std::string_view arg, const std::vector<std::string> &schemes) {
    const size_t colon = arg.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    const std::string s = asciiLower(arg.substr(0, colon));
    return std::find(schemes.begin(), schemes.end(), s) != schemes.end();
}

std::vector<std::string>
schemeUrls(const std::vector<std::string> &args, const std::vector<std::string> &schemes) {
    std::vector<std::string> out;
    for (const auto &a : args)
        if (isSchemeUrl(a, schemes))
            out.push_back(a);
    return out;
}

} // namespace plat::core
