// net — HTTPS requests and WebSockets for the app.
//
// One small API over each OS's own stack where it has one: WinHTTP on
// Windows, NSURLSession on macOS. Linux has no system HTTP library that a
// static binary can rely on, so there it is our own HTTP/1.1 + WebSocket
// client over mbedTLS (TLS 1.2/1.3 client only) with the system CA bundle.
//
// Threading: every call here is made on the UI thread (the one running
// plat::App). Blocking I/O happens on worker threads; callbacks always run
// later on the UI thread through App::post — never re-entrantly from inside
// the call that started the work. Destroying a Client or WebSocket cancels
// its work: no callback runs after the destructor returns.
//
// What it deliberately does not do: HTTP/2, proxies, compression (no
// Accept-Encoding is sent, so servers answer uncompressed), a persistent
// cookie jar. Redirects are followed here (not by the OS stack) so that a
// Cookie header set by the caller survives the whole chain, together with
// any cookies the redirecting responses set (the Slack boot page 302-chains
// and only the last hop carries the token).
#pragma once

#include "base/str.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace net {

struct Header {
    std::string name, value;
};

struct Response;

// Work on a request's answer that runs on the Client's worker thread instead
// of the UI thread (Request::handler).
class Handler {
public:
    virtual ~Handler() = default;
    // With streamBody set: a 2xx answer's body in order as it arrives (an
    // update's tens of megabytes to a file), instead of into Response::body;
    // the other answers (redirects, errors) still fill Response::body. False
    // stops the exchange with error "sink". Nothing has come before a
    // transport retries, so it never hears a byte twice.
    virtual bool body(const char *data, size_t n) {
        (void)data, (void)n;
        return false;
    }
    // The final answer (after redirects), just before `done` is posted:
    // parse it here. Not for a cancelled request.
    virtual void finished(Response &r) { (void)r; }
    bool         streamBody = false;
};

struct Request {
    std::string         method = "GET";
    std::string         url; // http:// or https://
    std::vector<Header> headers;
    std::string         body;                 // sent as is (set Content-Type yourself)
    int                 timeoutMs    = 30000; // per hop: connect + send + the whole answer
    int                 maxRedirects = 5;     // 0: a 3xx is returned as the response
    // Download progress (an update's tens of megabytes): the answer's body
    // bytes so far and its whole size (0 when the server doesn't say), on
    // the UI thread, a few times a second at most and never after `done`.
    std::function<void(int64_t received, int64_t total)> onProgress;
    // Worker-side work on the answer (see Handler); the caller keeps its own
    // reference to read the outcome in `done`.
    std::shared_ptr<Handler>                             handler;
};

struct Response {
    int                 status = 0; // 0: no HTTP answer, see error
    // Empty when the server answered (whatever the status). Otherwise one of:
    // "url", "dns", "connect", "tls", "timeout", "protocol", "too_many_redirects",
    // "cancelled", "unsupported" (+ ": detail" for the log).
    std::string         error;
    std::vector<Header> headers; // of the last hop, in arrival order
    std::string         body;
    std::string         url; // the last hop's URL (after redirects)

    bool             ok() const { return status >= 200 && status < 300; }
    // The first header with this name (ASCII case-insensitive); "" if absent.
    std::string_view header(std::string_view name) const;
};

using RequestId = uint64_t;

class Client {
public:
    explicit Client(plat::App &app);
    ~Client(); // cancels everything in flight; no callback runs afterwards
    Client(const Client &)            = delete;
    Client &operator=(const Client &) = delete;

    // Starts the request; `done` runs on the UI thread with the answer or
    // the failure. Never 0.
    RequestId send(Request req, std::function<void(Response)> done);
    // `done` of that request never runs. Unknown / finished ids are ignored.
    void      cancel(RequestId id);

    struct Impl;

private:
    std::shared_ptr<Impl> _impl;
};

// A WebSocket client (RFC 6455): text messages, pings answered for you.
// ws:// and wss:// (wss needs TLS: everywhere except where the platform
// layer says "unsupported").
class WebSocket {
public:
    explicit WebSocket(plat::App &app);
    ~WebSocket(); // closes the socket; no callback runs afterwards
    WebSocket(const WebSocket &)            = delete;
    WebSocket &operator=(const WebSocket &) = delete;

    // Connects; onOpen or onClosed follows. Extra headers go into the
    // upgrade request (Origin, Cookie, Authorization…).
    void open(std::string url, std::vector<Header> headers = {}, int timeoutMs = 15000);
    bool isOpen() const;
    // Queued in order; ignored unless open.
    void sendText(std::string_view text);
    // A keepalive ping (control frame, ≤ 125 bytes of payload), queued with
    // the messages; its pong is not reported. False when not open or the OS
    // stack has no call for it: WinHTTP, which instead sends a keepalive
    // (an unsolicited pong) every 20 s by itself. NSURLSession picks its own
    // payload.
    bool ping(std::string_view payload = {});
    // Starts the closing handshake; onClosed follows.
    void close(int code = 1000);

    std::function<void()>                 onOpen;
    std::function<void(std::string text)> onText;   // a whole (reassembled) message
    std::function<void(std::string data)> onBinary; // unset: binary messages are dropped
    // Once per open(): the close code (1000 normal, 1006 dropped) and reason,
    // or code 0 and an error ("dns", "connect", "tls", "handshake", "timeout",
    // "unsupported" …) when it never opened.
    std::function<void(int code, std::string reason)> onClosed;

    struct Impl;

private:
    std::shared_ptr<Impl> _impl;
};

// At exit, after every Client and WebSocket is gone: frees the process-wide
// connection state (kept-alive connections, TLS sessions, the CA store, the
// DNS cache), so that leak checkers see what is really left.
void releaseCaches();

// ── Helpers (pure, any thread) ─────────────────────────────────────────────

struct Url {
    std::string scheme; // lower case: "http", "https", "ws", "wss"
    std::string host;   // lower case, no brackets around an IPv6 literal
    int         port = 0;
    std::string target; // path + query, "/" when empty; never a fragment
    bool        secure() const { return scheme == "https" || scheme == "wss"; }
    int         defaultPort() const { return secure() ? 443 : 80; }
    // Parses an absolute URL; false when it is not one of the four schemes.
    bool        parse(std::string_view url);
    // An absolute or relative reference (a Location header) against this URL.
    std::string resolve(std::string_view ref) const;
    std::string str() const;       // "scheme://host[:port]target"
    std::string authority() const; // "host[:port]" (the Host header): IPv6 in brackets,
                                   // the port only when not the scheme's default
};

// RFC 3986 unreserved characters pass, everything else becomes %XX; and
// back ('+' is left alone). base/str.h's, under the names net callers know.
using str::percentDecode;
using str::percentEncode;
// application/x-www-form-urlencoded: "a=1&b=x%20y".
std::string formEncode(std::initializer_list<std::pair<std::string_view, std::string_view>> kv);
// The value of `name` in a query string or form body ("a=1&b=2"), decoded
// ('+' as space); "" if absent.
std::string queryValue(std::string_view query, std::string_view name);

// A multipart/form-data body (RFC 7578), built in memory: fields and files
// in order, then body() closes it. The boundary must not occur in any part
// (a random one never occurs in practice; callers pick it).
class Multipart {
public:
    explicit Multipart(std::string boundary) : _boundary(std::move(boundary)) {}
    void reserve(size_t bytes) { _body.reserve(bytes); }
    void field(std::string_view name, std::string_view value);
    // A file part. In the file name a '"' becomes '_' and a line break ' ',
    // so it can't break out of its header; "" mime is application/octet-stream.
    void file(
        std::string_view name,
        std::string_view fileName,
        std::string_view mime,
        std::string_view data
    );
    // The Content-Type header value: "multipart/form-data; boundary=…".
    std::string contentType() const;
    // The finished body (the closing boundary appended); the builder is spent.
    std::string body();

private:
    std::string _boundary, _body;
};

// A TCP port on 127.0.0.1 that was free a moment ago (bind to 0, read it,
// close); 0 on failure. For handing a port to a child process (DevTools).
int freeLoopbackPort();

} // namespace net
