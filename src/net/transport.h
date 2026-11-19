// The per-OS seam under net.h (internal to the module). The common code
// (client.cpp, websocket.cpp) owns threads, queues, redirects, cookies and
// delivery to the UI thread; a transport only does blocking I/O on the
// thread it is called from:
//
//   posix/  our own HTTP/1.1 + WebSocket over sockets + mbedTLS (Linux)
//   win/    WinHTTP (HTTP and its WebSocket API), the OS's TLS
//   mac/    NSURLSession (data tasks, NSURLSessionWebSocketTask), the OS's TLS
//   port/   the C port contract (src/port, port_net.h): the port's sockets and TLS
#pragma once

#include "net/net.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace net::detail {

// What every request says it is when the caller sets no User-Agent: the
// same on every OS, and the one msga has always sent.
inline constexpr const char *kUserAgent = "Mozilla/5.0";

// The stack of every thread the module starts (Client workers, WebSocket
// readers, DNS lookups): a TLS handshake plus certificate parsing, or
// glibc's NSS modules, fit with room; base::kThreadStack is more than they
// need, and a burst of requests starts several.
inline constexpr size_t kThreadStack = 256 * 1024;

// A request's cancel flag plus an OS object that set() signals, so a
// transport blocked on the request wakes at once instead of polling the
// flag: a self-pipe (POSIX), a manual-reset event (Windows), a dispatch
// semaphore that the exchange also waits on for its answer (macOS), a
// port_cancel (a port build). Made on the UI thread with the request, set
// there by Client::cancel or the Client's destructor. If the OS object
// can't be made, waits fall back to looking at the flag every 250 ms.
class Cancel {
public:
    Cancel();
    ~Cancel();
    Cancel(const Cancel &)            = delete;
    Cancel &operator=(const Cancel &) = delete;

    void                     set(); // any thread; idempotent
    bool                     isSet() const { return _flag.load(std::memory_order_acquire); }
    const std::atomic<bool> &flag() const { return _flag; }
    // The OS object: an fd to poll (-1: none), a HANDLE (0: none), a
    // dispatch_semaphore_t or a port_cancel * (0: none).
    intptr_t                 os() const { return _os; }

private:
    std::atomic<bool> _flag{false};
    intptr_t          _os = -1;
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(MSGA_PORT)
    int _osWake = -1; // the self-pipe's write end (_os is its read end)
#endif
};

// One HTTP exchange — no redirect following, no cookie handling, no
// decompression (the request carries no Accept-Encoding; a stack that adds
// one must also decode). Blocks the calling worker thread. Fills
// resp.status/headers/body, or resp.error (see Response::error) with
// status 0. Honours req.timeoutMs; returns promptly with error "cancelled"
// once `cancel` is set. resp.url is set by the caller. May keep connections
// alive between calls (thread-safe pool).
//
// req.body may be moved out while the OS stack still needs it (WinHTTP
// reads it asynchronously); it is back in req whenever the server answered,
// so a 307/308 can send it again. After a failure it may be gone.
//
// `progress`, when set, hears the body as it arrives (bytes so far, the
// Content-Length or 0) on the calling worker thread, as often as the
// transport likes: client.cpp throttles it. req.onProgress is never set.
//
// A 2xx answer's body goes to bodySink(req, status) instead of resp.body
// when that is not null (Handler::streamBody): in order, on this thread; its
// false ends the exchange with error "sink". `progress` counts those bytes.
using Progress = std::function<void(int64_t received, int64_t total)>;
inline Handler *bodySink(const Request &req, int status) {
    return req.handler && req.handler->streamBody && status >= 200 && status < 300
               ? req.handler.get()
               : nullptr;
}
void perform(
    const Url &url, Request &req, Response &resp, const Cancel &cancel, const Progress &progress
);
// The Content-Length header's value; 0 when absent or not a number.
int64_t          contentLength(const std::vector<Header> &headers);
// A Content-Length value: digits only (surrounding spaces allowed), at most
// 2^50. False otherwise.
bool             parseContentLength(std::string_view value, int64_t *n);
// The first header called `name` (ASCII case-insensitive); "" if none.
std::string_view headerValue(const std::vector<Header> &headers, std::string_view name);
// The "Name: value" lines of a response head after its status line, up to
// the blank line (CRLF-separated; obsolete folded lines are joined on). False
// for a line that is not a header; *out then holds those before it.
bool             parseHeaderLines(std::string_view lines, std::vector<Header> *out);

// For tests: how long an idle worker thread of a Client made after the call
// waits for work before it ends (30 s).
void setWorkerIdleMs(int ms);

// Drops idle kept-alive connections (Client destructor of the last client).
void closeIdleConnections();
// net::releaseCaches(): idle connections, then (POSIX, when no stream is
// open) the TLS sessions and settings, the CA store and the DNS cache.
void releaseCaches();

struct WsMessage {
    enum Kind : uint8_t { Text, Binary, Closed } kind = Text;
    std::string data;     // Text / Binary payload; Closed: the reason
    int         code = 0; // Closed: the peer's close code, 1006 when dropped
};

// One WebSocket connection. connect() and recv() run on the socket's own
// reader thread; send() and close() may be called from any thread while
// recv() blocks there (implementations serialise writes themselves).
class WsConn {
public:
    virtual ~WsConn() = default;
    // The opening handshake. False + *error ("dns", "connect", "tls",
    // "handshake", "timeout", "unsupported", optionally ": detail").
    virtual bool connect(
        const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
    )                                                   = 0;
    // Blocks until a whole message (fragments joined, pings answered,
    // pongs dropped) or the end of the connection: then kind = Closed and
    // false is returned. Never returns Closed twice.
    virtual bool recv(WsMessage &msg)                   = 0;
    virtual bool send(std::string_view data, bool text) = 0;
    // A ping control frame (≤ 125 bytes of payload); its pong is dropped.
    // False when the stack can't send one (WinHTTP: it sends its own
    // keepalives instead).
    virtual bool ping(std::string_view payload)         = 0;
    // Starts the closing handshake; recv() then returns Closed.
    virtual void close(int code)                        = 0;
    // Tears the connection down now (destructor path): a blocked recv()
    // returns Closed promptly.
    virtual void abort()                                = 0;
};
std::unique_ptr<WsConn> makeWsConn();

// net::freeLoopbackPort, per OS (Winsock needs its own start-up).
int loopbackPort();

} // namespace net::detail
