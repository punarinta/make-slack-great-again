#include "app/mrkdwn/link_labels.h"

#include "base/str.h"

#include <vector>

namespace mrkdwn {

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // …

// "https://a/b" → "a/b" (any scheme: letters, then letters/digits/+.-, "://").
std::string_view schemeless(std::string_view url) {
    const size_t at = url.find("://");
    if (at == std::string_view::npos || at == 0)
        return url;
    auto alpha = [](char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; };
    if (!alpha(url[0]))
        return url;
    for (size_t i = 1; i < at; ++i) {
        const char c = url[i];
        if (!alpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '.' && c != '-')
            return url;
    }
    return url.substr(at + 3);
}

bool endsWithNoCase(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && str::iequals(s.substr(s.size() - suffix.size()), suffix);
}

} // namespace

bool isGiphyMediaUrl(std::string_view url) {
    std::string_view rest = schemeless(url);
    if (rest.size() == url.size())
        return false; // no scheme: not a URL
    const size_t     slash = rest.find_first_of("/?#");
    std::string_view host  = rest.substr(0, slash);
    if (const size_t at = host.rfind('@'); at != std::string_view::npos)
        host.remove_prefix(at + 1);
    if (const size_t colon = host.find(':'); colon != std::string_view::npos)
        host = host.substr(0, colon);
    if (!(endsWithNoCase(host, "giphy.com") && (host.size() == 9 || host[host.size() - 10] == '.')))
        return false;
    std::string_view path =
        slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
    path = path.substr(0, path.find_first_of("?#"));
    return path.compare(0, 7, "/media/") == 0 || endsWithNoCase(path, ".gif") ||
           endsWithNoCase(path, ".webp");
}

bool isUrlLabel(std::string_view label, std::string_view url) {
    const std::string_view l = str::trim(label);
    if (l.empty() || l == url)
        return true;
    std::string_view bare = schemeless(url);
    if (l == bare || (!bare.empty() && bare.back() == '/' && l == bare.substr(0, bare.size() - 1)))
        return true;
    return isShortenedUrlLabel(l, url);
}

bool isShortenedUrlLabel(std::string_view label, std::string_view url) {
    if (url.empty() || (label.find(kEllipsis) == std::string_view::npos &&
                        label.find("...") == std::string_view::npos))
        return false; // nothing cut (most labels): no copy made
    // "..." reads as "…" too.
    std::string l;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label.compare(i, 3, "...") == 0) {
            l += kEllipsis;
            i += 2;
        } else {
            l += label[i];
        }
    }
    if (l.find(kEllipsis) == std::string::npos)
        return false;
    std::vector<std::string_view> pieces;
    std::string_view              rest = l;
    for (size_t at; (at = rest.find(kEllipsis)) != std::string_view::npos;) {
        if (at)
            pieces.push_back(rest.substr(0, at));
        rest.remove_prefix(at + kEllipsis.size());
    }
    if (!rest.empty())
        pieces.push_back(rest);
    const std::string_view u = schemeless(url);
    if (pieces.empty() || u.compare(0, pieces[0].size(), pieces[0]) != 0)
        return false;
    size_t from = pieces[0].size();
    for (size_t i = 1; i < pieces.size(); ++i) {
        const size_t at = u.find(pieces[i], from);
        if (at == std::string_view::npos)
            return false;
        from = at + pieces[i].size();
    }
    return true;
}

std::string expandedLabel(std::string_view url, size_t maxChars) {
    const std::string_view u     = schemeless(url);
    // Characters, not bytes: count UTF-8 lead bytes.
    size_t                 chars = 0, cut = std::string_view::npos;
    for (size_t i = 0; i < u.size(); ++i)
        if ((u[i] & 0xc0) != 0x80) {
            if (chars == (maxChars > 1 ? maxChars - 1 : 1))
                cut = i;
            ++chars;
        }
    if (chars <= maxChars || cut == std::string_view::npos)
        return std::string(u);
    return std::string(u.substr(0, cut)) + std::string(kEllipsis);
}

} // namespace mrkdwn
