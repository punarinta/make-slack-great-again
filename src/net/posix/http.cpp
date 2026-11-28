// HTTP/1.1 for the POSIX transport: one exchange per perform() over a
// Stream, with a small keep-alive pool of idle connections (see transport.h).
#include "base/str.h"
#include "net/posix/posix.h"
#include "net/transport.h"

#include <cstring>
#include <mutex>

namespace net::detail {

namespace {

constexpr size_t  kMaxHead   = 64 * 1024;
constexpr size_t  kMaxBody   = 64 * 1024 * 1024;
constexpr int64_t kIdleMs    = 30000; // what most servers keep an idle connection for
constexpr size_t  kMaxIdle   = 8;
constexpr size_t  kReadChunk = 16 * 1024;

// ── Keep-alive pool ─────────────────────────────────────────────────────────

struct Idle {
    std::string             key; // "scheme://host:port"
    std::unique_ptr<Stream> stream;
    int64_t                 since = 0;
};

struct Pool {
    std::mutex        mutex;
    std::vector<Idle> idle;
};

Pool &pool() {
    return immortal<Pool>();
}

// Expired streams are moved out under the lock and closed outside it (a TLS
// close writes to the socket).
void dropExpired(std::vector<Idle> &idle, std::vector<std::unique_ptr<Stream>> *out) {
    const int64_t now = nowMs();
    for (size_t i = 0; i < idle.size();)
        if (now - idle[i].since > kIdleMs) {
            out->push_back(std::move(idle[i].stream));
            idle.erase(idle.begin() + i);
        } else {
            ++i;
        }
}

std::unique_ptr<Stream> takeIdle(const std::string &key) {
    std::vector<std::unique_ptr<Stream>> dead;
    std::unique_ptr<Stream>              found;
    {
        Pool                       &p = pool();
        std::lock_guard<std::mutex> lock(p.mutex);
        dropExpired(p.idle, &dead);
        for (size_t i = p.idle.size(); i-- > 0;) // the most recently used first
            if (p.idle[i].key == key) {
                found = std::move(p.idle[i].stream);
                p.idle.erase(p.idle.begin() + i);
                break;
            }
    }
    if (found && found->idleDead())
        found.reset();
    return found;
}

void putIdle(const std::string &key, std::unique_ptr<Stream> s) {
    std::vector<std::unique_ptr<Stream>> dead;
    Pool                                &p = pool();
    std::lock_guard<std::mutex>          lock(p.mutex);
    dropExpired(p.idle, &dead);
    if (p.idle.size() >= kMaxIdle) {
        dead.push_back(std::move(p.idle.front().stream));
        p.idle.erase(p.idle.begin());
    }
    p.idle.push_back({key, std::move(s), nowMs()});
}

// ── Reading ─────────────────────────────────────────────────────────────────

// Appends what the stream has (blocking) to *buf. > 0, 0 at the end, < 0.
// Read on the stack, so *buf grows by what came, not by a zero-filled chunk.
long fill(Stream &s, std::string *buf, const Waiter &w, std::string *error) {
    char       chunk[kReadChunk];
    const long r = s.read(chunk, sizeof chunk, w, error);
    if (r > 0)
        buf->append(chunk, size_t(r));
    return r;
}

bool parseHead(std::string_view text, Head *head) {
    // "HTTP/1.1 200 OK"
    size_t           eol  = text.find("\r\n");
    std::string_view line = text.substr(0, eol);
    if (line.size() < 12 || !str::startsWith(line, "HTTP/1.") || line[8] != ' ')
        return false;
    head->minor  = line[7] - '0';
    head->status = 0;
    for (int i = 9; i < 12; ++i) {
        if (line[i] < '0' || line[i] > '9')
            return false;
        head->status = head->status * 10 + (line[i] - '0');
    }
    head->headers.clear();
    return eol == std::string_view::npos || parseHeaderLines(text.substr(eol + 2), &head->headers);
}

// Takes bytes from the front of *buf, reading more as needed.
struct Body {
    Stream         &s;
    std::string    *buf;
    size_t          pos = 0;
    const Waiter   &w;
    std::string    *error;
    // The caller's progress hook (perform()'s) and the Content-Length.
    const Progress *progress = nullptr;
    int64_t         total    = 0;
    // A streamed body's handler (bodySink) and what it heard so far.
    Handler        *sink     = nullptr;
    size_t          streamed = 0;

    void report(size_t got) const {
        if (progress && *progress)
            (*progress)(int64_t(got), total);
    }
    size_t received(const std::string &out) const { return sink ? streamed : out.size(); }
    // n bytes to the sink.
    bool   put(const char *data, size_t n) {
        streamed += n;
        if (!sink->body(data, n)) {
            *error = "sink";
            return false;
        }
        report(streamed);
        return true;
    }
    // take() for a streamed body: what is buffered, then reads in chunks.
    bool stream(size_t n) {
        const size_t have = std::min(n, buf->size() - pos);
        if (have && !put(buf->data() + pos, have))
            return false;
        pos += have;
        n -= have;
        char chunk[kReadChunk];
        while (n) {
            const long r = s.read(chunk, std::min(n, sizeof chunk), w, error);
            if (r <= 0) {
                if (r == 0)
                    *error = "protocol: truncated body";
                return false;
            }
            if (!put(chunk, size_t(r)))
                return false;
            n -= size_t(r);
        }
        return true;
    }
    bool more() {
        if (pos > 0) {
            buf->erase(0, pos);
            pos = 0;
        }
        const long r = fill(s, buf, w, error);
        if (r == 0)
            *error = "protocol: truncated body";
        return r > 0;
    }
    bool line(std::string_view *out) {
        for (;;) {
            const size_t eol = buf->find("\r\n", pos);
            if (eol != std::string::npos) {
                *out = std::string_view(*buf).substr(pos, eol - pos);
                pos  = eol + 2;
                return true;
            }
            if (buf->size() - pos > kMaxHead) {
                *error = "protocol: line too long";
                return false;
            }
            if (!more())
                return false;
        }
    }
    // n body bytes appended to *out: what is buffered, then read straight
    // into its place, with no staging copy (a Content-Length body or a chunk).
    bool take(size_t n, std::string *out) {
        if (sink)
            return stream(n);
        const size_t have = std::min(n, buf->size() - pos);
        if (out->empty())
            out->reserve(n); // later chunks grow it geometrically
        out->append(*buf, pos, have);
        pos += have;
        size_t got = out->size();
        if (have)
            report(got);
        if (have == n)
            return true;
        const size_t end = got + (n - have);
        out->resize(end);
        while (got < end) {
            const long r = s.read(&(*out)[got], end - got, w, error);
            if (r <= 0) {
                if (r == 0)
                    *error = "protocol: truncated body";
                out->resize(got);
                return false;
            }
            got += size_t(r);
            report(got);
        }
        return true;
    }
    bool chunked(std::string *out) {
        for (;;) {
            std::string_view l;
            if (!line(&l))
                return false;
            size_t size = 0, digits = 0;
            for (char c : l) {
                const int v = str::hexDigit(c);
                if (v < 0)
                    break; // ";ext" or spaces
                size = size * 16 + size_t(v);
                if (++digits > 12)
                    break;
            }
            if (digits == 0 || digits > 12 || received(*out) + size > kMaxBody) {
                *error =
                    digits && digits <= 12 ? "protocol: body too large" : "protocol: bad chunk";
                return false;
            }
            if (size == 0) { // trailers, then an empty line
                while (line(&l))
                    if (l.empty())
                        return true;
                return false;
            }
            if (!take(size, out) || !line(&l))
                return false;
            if (!l.empty()) {
                *error = "protocol: bad chunk";
                return false;
            }
        }
    }
};

bool bodyless(int status, std::string_view method) {
    return method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200);
}

// Unsent: the request never fully left (a dead kept-alive socket), safe to
// send again whatever it is. Retry: sent, but no byte of an answer came — the
// server may have acted on it, so only an idempotent request is sent again
// (a re-sent POST is a duplicate Slack message: there is no idempotency key).
enum class Outcome : uint8_t { Done, Kept, Unsent, Retry, Failed };

// One request/response on `s`. Kept: the stream may be reused.
Outcome exchange(
    Stream          &s,
    std::string_view head,
    const Request   &req,
    Response        &resp,
    const Waiter    &w,
    std::string     *error,
    const Progress  &progress
) {
    // Small bodies go in the same write as the head (one TCP segment for a
    // typical form POST); big ones are sent straight from the request.
    const bool together = req.body.size() <= 16 * 1024;
    bool       ok       = together ? s.writeAll(str::concat({head, req.body}), w, error)
                                   : s.writeAll(head, w, error) && s.writeAll(req.body, w, error);
    if (!ok)
        return *error == "timeout" || *error == "cancelled" ? Outcome::Failed : Outcome::Unsent;

    std::string buf;
    Head        h;
    long        headLen;
    for (;;) {
        bool gotAny = false;
        headLen     = readHead(s, &buf, w, &h, error, &gotAny);
        if (headLen < 0)
            return !gotAny && *error != "timeout" && *error != "cancelled" ? Outcome::Retry
                                                                           : Outcome::Failed;
        if (h.status >= 200 || h.status == 101)
            break;
        buf.erase(0, size_t(headLen)); // 100 Continue, 103 Early Hints: not the answer
    }
    resp.status  = h.status;
    resp.headers = std::move(h.headers);

    bool                   keep = h.minor >= 1;
    const std::string_view conn = headerValue(resp.headers, "connection");
    for (size_t i = 0; i + 5 <= conn.size(); ++i)
        if (iequals(conn.substr(i, 5), "close"))
            keep = false;
    Body body{s, &buf, size_t(headLen), w, error};
    body.progress = &progress;
    body.total    = contentLength(resp.headers);
    body.sink     = bodySink(req, resp.status);
    if (bodyless(resp.status, req.method))
        return keep && body.pos == buf.size() ? Outcome::Kept : Outcome::Done;

    const std::string_view te = headerValue(resp.headers, "transfer-encoding");
    const std::string_view cl = headerValue(resp.headers, "content-length");
    if (!te.empty()) {
        bool chunked = false;
        for (size_t i = 0; i + 7 <= te.size(); ++i)
            chunked = chunked || iequals(te.substr(i, 7), "chunked");
        if (!chunked) {
            *error = "protocol: unsupported transfer-encoding";
            return Outcome::Failed;
        }
        if (!body.chunked(&resp.body))
            return Outcome::Failed;
    } else if (!cl.empty()) {
        int64_t n = 0;
        if (!parseContentLength(cl, &n)) {
            *error = "protocol: bad content-length";
            return Outcome::Failed;
        }
        if (uint64_t(n) > kMaxBody) {
            *error = "protocol: body too large";
            return Outcome::Failed;
        }
        if (!body.take(size_t(n), &resp.body))
            return Outcome::Failed;
    } else if (body.sink) {
        // Neither, streamed: until the server closes.
        if (body.pos < buf.size() && !body.put(buf.data() + body.pos, buf.size() - body.pos))
            return Outcome::Failed;
        char chunk[kReadChunk];
        for (;;) {
            const long r = s.read(chunk, sizeof chunk, w, error);
            if (r == 0)
                return Outcome::Done;
            if (r < 0 || !body.put(chunk, size_t(r)))
                return Outcome::Failed;
            if (body.streamed > kMaxBody) {
                *error = "protocol: body too large";
                return Outcome::Failed;
            }
        }
    } else {
        // Neither: the body runs until the server closes.
        resp.body.assign(buf, body.pos);
        for (;;) {
            const long r = fill(s, &resp.body, w, error);
            if (r > 0)
                body.report(resp.body.size());
            if (r == 0)
                return Outcome::Done;
            if (r < 0)
                return Outcome::Failed;
            if (resp.body.size() > kMaxBody) {
                *error = "protocol: body too large";
                return Outcome::Failed;
            }
        }
    }
    // Bytes beyond the answer would be a response to nothing: don't reuse.
    return keep && body.pos == buf.size() ? Outcome::Kept : Outcome::Done;
}

std::string requestHead(const Url &url, const Request &req) {
    std::string head = str::concat({req.method, " ", url.target, " HTTP/1.1\r\n"});
    bool        host = false, conn = false, length = false;
    for (const auto &h : req.headers) {
        host   = host || iequals(h.name, "host");
        conn   = conn || iequals(h.name, "connection");
        length = length || iequals(h.name, "content-length");
    }
    if (!host)
        head += str::concat({"Host: ", url.authority(), "\r\n"});
    if (!conn)
        head += "Connection: keep-alive\r\n";
    // Servers answer 411 to a body-less POST without a length.
    if (!length &&
        (!req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH"))
        head += str::concat({"Content-Length: ", str::number(int64_t(req.body.size())), "\r\n"});
    for (const auto &h : req.headers)
        head += str::concat({h.name, ": ", h.value, "\r\n"});
    head += "\r\n";
    return head;
}

} // namespace

long readHead(
    Stream &s, std::string *buf, const Waiter &w, Head *head, std::string *error, bool *gotAny
) {
    size_t scanned = 0;
    for (;;) {
        const size_t end = buf->find("\r\n\r\n", scanned > 3 ? scanned - 3 : 0);
        if (end != std::string::npos) {
            if (!parseHead(std::string_view(*buf).substr(0, end + 2), head)) {
                *error = "protocol: bad response head";
                return -1;
            }
            return long(end + 4);
        }
        if (buf->size() > kMaxHead) {
            *error = "protocol: response head too large";
            return -1;
        }
        scanned      = buf->size();
        const long r = fill(s, buf, w, error);
        if (r < 0)
            return -1;
        if (r == 0) {
            *error = buf->empty() ? "connect: closed" : "protocol: truncated head";
            return -1;
        }
        *gotAny = true;
    }
}

void perform(
    const Url &url, Request &req, Response &resp, const Cancel &cancel, const Progress &progress
) {
    Waiter w;
    w.cancel               = &cancel.flag();
    w.wakeFd               = int(cancel.os()); // -1 without one: sliced waits
    w.deadline             = req.timeoutMs > 0 ? nowMs() + req.timeoutMs : 0;
    const std::string key  = str::concat({url.scheme, "://", url.host, ":", str::number(url.port)});
    const std::string head = requestHead(url, req);
    std::string       error;
    // A kept-alive connection the server has meanwhile dropped fails before
    // a byte of the answer arrives: then once more on a fresh one.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::unique_ptr<Stream> s      = attempt == 0 ? takeIdle(key) : nullptr;
        const bool              reused = s != nullptr;
        if (!s) {
            s = std::make_unique<Stream>();
            if (!s->open(url, w, &error))
                break;
        }
        resp                     = Response();
        const Outcome o          = exchange(*s, head, req, resp, w, &error, progress);
        const bool    idempotent = req.method == "GET" || req.method == "HEAD";
        if (reused && (o == Outcome::Unsent || (o == Outcome::Retry && idempotent)))
            continue;
        if (o == Outcome::Kept && !cancel.isSet()) {
            putIdle(key, std::move(s));
            return;
        }
        if (o == Outcome::Done || o == Outcome::Kept)
            return;
        break;
    }
    if (cancel.isSet())
        error = "cancelled";
    resp       = Response();
    resp.error = error.empty() ? "connect" : error;
}

void closeIdleConnections() {
    std::vector<Idle> idle;
    {
        Pool                       &p = pool();
        std::lock_guard<std::mutex> lock(p.mutex);
        idle.swap(p.idle);
    }
}

void releaseCaches() {
    closeIdleConnections();
    // A stream still open (a Client or WebSocket that outlives the call) may
    // yet resolve, resume or verify through these: then they stay.
    if (liveStreams() != 0)
        return;
    releaseDns();
    releaseTls();
}

} // namespace net::detail
