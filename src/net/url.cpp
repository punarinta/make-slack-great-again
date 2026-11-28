#include "base/str.h"
#include "net/net.h"
#include "net/transport.h"

namespace net {

namespace {

// "/a/b/../c/./d" → "/a/c/d" (RFC 3986 remove_dot_segments, path only).
std::string removeDots(std::string_view path) {
    std::vector<std::string_view> segs;
    size_t                        i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string_view::npos)
            j = path.size();
        const std::string_view s = path.substr(i, j - i);
        if (s == "..") {
            if (!segs.empty())
                segs.pop_back();
        } else if (s != "." && !(s.empty() && j < path.size())) {
            segs.push_back(s);
        }
        i = j + 1;
    }
    std::string out;
    for (const auto s : segs) {
        out += '/';
        out += s;
    }
    const std::string_view last = path.substr(path.rfind('/') + 1);
    if ((last == "." || last == "..") && (out.empty() || out.back() != '/'))
        out += '/';
    return out.empty() ? "/" : out;
}

} // namespace

bool Url::parse(std::string_view u) {
    const size_t colon = u.find("://");
    if (colon == std::string_view::npos)
        return false;
    scheme = str::asciiLower(u.substr(0, colon));
    if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")
        return false;
    std::string_view rest = u.substr(colon + 3);
    const size_t     hash = rest.find('#');
    if (hash != std::string_view::npos)
        rest = rest.substr(0, hash);
    size_t pathAt = rest.find_first_of("/?");
    if (pathAt == std::string_view::npos)
        pathAt = rest.size();
    std::string_view authority = rest.substr(0, pathAt);
    if (const size_t at = authority.rfind('@'); at != std::string_view::npos)
        authority = authority.substr(at + 1); // userinfo is never used
    std::string_view portStr;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos)
            return false;
        host = str::asciiLower(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return false;
            portStr = authority.substr(close + 2);
        }
    } else {
        const size_t c = authority.rfind(':');
        host           = str::asciiLower(authority.substr(0, c));
        if (c != std::string_view::npos)
            portStr = authority.substr(c + 1);
    }
    if (host.empty())
        return false;
    port = defaultPort();
    if (!portStr.empty()) {
        int p = 0;
        for (char ch : portStr) {
            if (ch < '0' || ch > '9' || p > 65535)
                return false;
            p = p * 10 + (ch - '0');
        }
        if (p <= 0 || p > 65535)
            return false;
        port = p;
    }
    target = std::string(rest.substr(pathAt));
    if (target.empty() || target[0] == '?')
        target.insert(0, "/");
    return true;
}

std::string Url::authority() const {
    std::string out = host.find(':') != std::string::npos ? str::concat({"[", host, "]"}) : host;
    if (port != defaultPort())
        out += str::concat({":", str::number(port)});
    return out;
}

std::string Url::str() const {
    return str::concat({scheme, "://", authority(), target});
}

std::string Url::resolve(std::string_view ref) const {
    if (ref.find("://") != std::string_view::npos) {
        // Only an absolute URL if what precedes "://" is a scheme.
        const size_t c  = ref.find("://");
        bool         ok = c > 0;
        for (size_t i = 0; i < c && ok; ++i) {
            const char ch = ref[i];
            ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (i > 0 && ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'));
        }
        if (ok)
            return std::string(ref);
    }
    if (const size_t hash = ref.find('#'); hash != std::string_view::npos)
        ref = ref.substr(0, hash);
    if (str::startsWith(ref, "//"))
        return scheme + ":" + std::string(ref);
    Url out = *this;
    if (ref.empty())
        return out.str();
    if (ref[0] == '?') {
        out.target = target.substr(0, target.find('?')) + std::string(ref);
        return out.str();
    }
    std::string_view path = ref, query;
    if (const size_t q = ref.find('?'); q != std::string_view::npos) {
        path  = ref.substr(0, q);
        query = ref.substr(q);
    }
    std::string merged;
    if (path[0] == '/') {
        merged = std::string(path);
    } else {
        const std::string base = target.substr(0, target.find('?'));
        merged                 = base.substr(0, base.rfind('/') + 1) + std::string(path);
    }
    out.target = removeDots(merged) + std::string(query);
    return out.str();
}

std::string formEncode(std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
    std::string out;
    for (const auto &[k, v] : kv) {
        if (!out.empty())
            out += '&';
        out += percentEncode(k);
        out += '=';
        out += percentEncode(v);
    }
    return out;
}

void Multipart::field(std::string_view name, std::string_view value) {
    _body += str::concat({"--", _boundary, "\r\nContent-Disposition: form-data; name=\"", name});
    _body += str::concat({"\"\r\n\r\n", value, "\r\n"});
}

void Multipart::file(
    std::string_view name, std::string_view fileName, std::string_view mime, std::string_view data
) {
    std::string safe(fileName);
    for (char &c : safe)
        if (c == '"')
            c = '_';
        else if (c == '\r' || c == '\n')
            c = ' ';
    _body += str::concat(
        {"--",
         _boundary,
         "\r\nContent-Disposition: form-data; name=\"",
         name,
         "\"; filename=\"",
         safe,
         "\"\r\nContent-Type: ",
         mime.empty() ? std::string_view("application/octet-stream") : mime,
         "\r\n\r\n"}
    );
    _body += data;
    _body += "\r\n";
}

std::string Multipart::contentType() const {
    return str::concat({"multipart/form-data; boundary=", _boundary});
}

std::string Multipart::body() {
    _body += str::concat({"--", _boundary, "--\r\n"});
    return std::move(_body);
}

std::string queryValue(std::string_view query, std::string_view name) {
    if (!query.empty() && query[0] == '?')
        query.remove_prefix(1);
    str::Splitter pairs(query, '&');
    for (std::string_view pair; pairs.next(&pair);) {
        const size_t eq = pair.find('=');
        std::string  key(pair.substr(0, eq));
        for (char &c : key) // '+' means space in a form key, not only a value
            if (c == '+')
                c = ' ';
        if (percentDecode(key) != name)
            continue;
        std::string v(eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1));
        for (char &c : v)
            if (c == '+')
                c = ' ';
        return percentDecode(v);
    }
    return {};
}

int freeLoopbackPort() {
    return detail::loopbackPort();
}

} // namespace net
