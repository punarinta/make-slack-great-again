// The Windows transport (transport.h) on WinHTTP: HTTP exchanges and its
// WebSocket API, with Schannel doing TLS and the system proxy settings
// applied — no TLS library of our own.
//
// WinHTTP runs in asynchronous mode. The synchronous API cannot be cancelled
// from the thread that is blocked in it, and closing a handle from another
// thread races with that thread's next call on it. Async keeps every handle
// owned by one thread: that thread starts an operation, then waits on an
// event the status callback sets, together with a wake event (the request's
// Cancel, a WebSocket's abort) and the deadline. Giving up means closing the
// handle from the owning thread and returning at once: how soon WinHTTP
// really abandons the operation varies (Wine waits for the server), so
// everything it may still touch — the context, events, read buffers, the
// request body, queued WebSocket sends — lives in a heap block that the
// owner and the handle share; HANDLE_CLOSING (the last callback a handle
// gets) drops the handle's reference.
#include "base/str.h"
#include "base/utf8.h"
#include "base/winstr.h"
#include "net/transport.h"

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>

#include <deque>
#include <mutex>

namespace net::detail {

namespace {

constexpr DWORD  kChunk         = 64 * 1024; // one ReadData / WebSocketReceive
constexpr DWORD  kWsKeepAliveMs = 20000;     // see session()
constexpr DWORD  kSlice         = 250; // how often a wait looks at `cancel` without a wake event
constexpr size_t kMaxQueued = 64 * 1024 * 1024; // unsent WebSocket data beyond this: send() fails

using base::wide;

// WinHTTP hands response headers back widened byte by byte, so code units
// below 0x100 are the raw bytes (UTF-8 passes through untouched); anything
// above is encoded as UTF-8.
std::string headerBytes(const wchar_t *s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const unsigned c = s[i];
        if (c < 0x100)
            out += char(c);
        else
            utf8::append(out, c);
    }
    return out;
}

// A WinHTTP error as a Response::error reason (+ the code for the log).
// `cert` is what the SECURE_FAILURE notification said was wrong, if it came.
// Certificate faults get the same names as in the posix transport.
std::string failure(DWORD e, DWORD cert = 0) {
    const char *why = "protocol";
    switch (e) {
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "cancelled";
    case ERROR_WINHTTP_TIMEOUT:
        return "timeout";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        why = "dns";
        break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        why = "connect";
        break;
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        return "tls: hostname mismatch";
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        return "tls: certificate expired";
    case ERROR_WINHTTP_SECURE_INVALID_CA:
        return "tls: untrusted certificate";
    case ERROR_WINHTTP_SECURE_FAILURE:
        if (cert & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID)
            return "tls: hostname mismatch";
        if (cert & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID)
            return "tls: certificate expired";
        if (cert &
            (WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA | WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT))
            return "tls: untrusted certificate";
        if (cert & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED)
            return "tls: certificate revoked";
        why = "tls";
        break;
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        why = "tls";
        break;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        why = "url";
        break;
    }
    return str::concat({why, ": winhttp ", str::number(int64_t(e))});
}

// Memory WinHTTP may use: what a handle's context points at. The callback
// dispatches on `ws`. Freed once the owner and every handle it was given to
// have let go.
struct Ctx {
    explicit Ctx(bool isWs) : ws(isWs) {}
    virtual ~Ctx() = default;
    void retain() { refs.fetch_add(1); }
    void release() {
        if (refs.fetch_sub(1) == 1)
            delete this;
    }
    const bool       ws;
    std::atomic<int> refs{1}; // the owner's
};

HANDLE autoEvent() {
    return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

// An HTTP request handle's context: one async call in flight at a time.
struct Op final : Ctx {
    Op() : Ctx(false) {}
    ~Op() override { CloseHandle(done); }
    HANDLE       done   = autoEvent();
    DWORD        status = 0; // the WINHTTP_CALLBACK_STATUS_* that ended the call
    DWORD        error  = 0; // REQUEST_ERROR
    DWORD        bytes  = 0; // READ_COMPLETE
    DWORD        cert   = 0; // SECURE_FAILURE flags (they come before the REQUEST_ERROR)
    std::string  body;       // the request body, sent from here
    std::wstring headers;
    char         buf[kChunk]; // ReadData lands here
};

// A WebSocket handle's context: one receive and one send/shutdown can be in
// flight together, each with its own event.
struct WsCtx final : Ctx {
    struct Out {
        std::string data;
        bool        text;
    };
    WsCtx() : Ctx(true) {}
    ~WsCtx() override {
        CloseHandle(readEv);
        CloseHandle(writeEv);
        CloseHandle(wakeEv);
    }
    HANDLE          readEv    = autoEvent();
    HANDLE          writeEv   = autoEvent();
    HANDLE          wakeEv    = autoEvent(); // a send, close() or abort() for the reader
    DWORD           readBytes = 0, readType = 0, readErr = 0, writeErr = 0;
    char            buf[kChunk]; // WebSocketReceive lands here
    // Shared with send/close/abort. The front of `out` is what WinHTTP is
    // sending (a deque: pushing more never moves it).
    std::mutex      mutex;
    std::deque<Out> out;
    size_t          queued    = 0; // bytes in `out`
    int             closeCode = 0; // what close() asked for
    bool            ended     = false;
};

// Runs on WinHTTP's threads, sometimes inline in the call that started the
// operation: it only records the result and signals, never takes a lock.
void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void *info, DWORD infoLen) {
    auto *ctx = reinterpret_cast<Ctx *>(context);
    if (!ctx)
        return; // the connect handle (no context)
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        ctx->release();
        return;
    }
    if (!ctx->ws) {
        auto &op = *static_cast<Op *>(ctx);
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            op.bytes = infoLen; // info is our buffer; the length is the byte count
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            op.error = static_cast<WINHTTP_ASYNC_RESULT *>(info)->dwError;
            break;
        case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
            op.cert = *static_cast<DWORD *>(info);
            return;
        default:
            return;
        }
        op.status = status;
        SetEvent(op.done);
        return;
    }
    auto &w = *static_cast<WsCtx *>(ctx);
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
        const auto *st = static_cast<WINHTTP_WEB_SOCKET_STATUS *>(info);
        w.readBytes    = st->dwBytesTransferred;
        w.readType     = DWORD(st->eBufferType);
        w.readErr      = 0;
        SetEvent(w.readEv);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE:
        w.writeErr = 0;
        SetEvent(w.writeEv);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto *r   = static_cast<WINHTTP_WEB_SOCKET_ASYNC_RESULT *>(info);
        const DWORD err = r->AsyncResult.dwError ? r->AsyncResult.dwError : ERROR_GEN_FAILURE;
        if (r->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) {
            w.readErr = err;
            SetEvent(w.readEv);
        } else {
            w.writeErr = err;
            SetEvent(w.writeEv);
        }
        break;
    }
    }
}

// One session for the process: WinHTTP pools connections per session, so
// keep-alive works across requests and threads. Never closed.
HINTERNET session() {
    static const HINTERNET s = [] {
        // Automatic proxy (WPAD/PAC + per-user settings) exists since 8.1;
        // older systems fall back to the machine-wide setting.
        HINTERNET h = WinHttpOpen(
            L"Mozilla/5.0", // kUserAgent
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            WINHTTP_FLAG_ASYNC
        );
        if (!h)
            h = WinHttpOpen(
                L"Mozilla/5.0", // kUserAgent
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                WINHTTP_FLAG_ASYNC
            );
        if (!h)
            return h;
        WinHttpSetStatusCallback(
            h,
            callback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES |
                WINHTTP_CALLBACK_FLAG_SECURE_FAILURE | WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE,
            0
        );
        // The WebSocket keepalive (WinWsConn::ping): as often as the old
        // app's Socket Mode pinged. Its default is 30 s, its floor 15 s.
        DWORD keepAlive = kWsKeepAliveMs;
        WinHttpSetOption(
            h, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof keepAlive
        );
        // TLS 1.2+ only; a system without 1.3 refuses the flag.
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        return h;
    }();
    return s;
}

ULONGLONG deadlineAfter(int timeoutMs) {
    return timeoutMs > 0 ? GetTickCount64() + ULONGLONG(timeoutMs) : ~ULONGLONG(0);
}

// Gives `ctx` to handle `h` as its context; the handle holds a reference
// until its HANDLE_CLOSING.
bool attach(HINTERNET h, Ctx *ctx) {
    DWORD_PTR v = reinterpret_cast<DWORD_PTR>(ctx);
    if (!WinHttpSetOption(h, WINHTTP_OPTION_CONTEXT_VALUE, &v, sizeof v))
        return false;
    ctx->retain();
    return true;
}

// A connect handle plus one request on it, owned by the calling thread.
struct Exchange {
    Op       *op      = new Op;
    HINTERNET connect = nullptr, request = nullptr;
    HANDLE    wake = nullptr; // wakes a wait early: the request's Cancel, a WebSocket's abort

    ~Exchange() {
        closeRequest();
        if (connect)
            WinHttpCloseHandle(connect);
        op->release();
    }
    // Never waits: whatever is in flight finishes (or not) against *op.
    void closeRequest() {
        if (request)
            WinHttpCloseHandle(request);
        request = nullptr;
    }
    // Waits for the call in flight; "" when it succeeded, else the reason.
    std::string await(ULONGLONG deadline, const std::atomic<bool> &cancel) {
        HANDLE evs[2] = {op->done, wake};
        for (;;) {
            if (cancel.load()) {
                closeRequest();
                return "cancelled";
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                closeRequest();
                return "timeout";
            }
            // With a wake event whoever sets `cancel` signals it: no polling.
            const ULONGLONG left = deadline - now;
            const ULONGLONG most = wake ? ULONGLONG(INFINITE - 1) : kSlice;
            const DWORD     slice =
                deadline == ~ULONGLONG(0) && wake ? INFINITE : DWORD(left < most ? left : most);
            if (WaitForMultipleObjects(wake ? 2 : 1, evs, FALSE, slice) == WAIT_OBJECT_0)
                return op->status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR
                           ? failure(op->error, op->cert)
                           : "";
        }
    }
    // Connects, sends the request and waits for the response headers.
    // *body (may be null) is moved into the Op, where WinHTTP reads it from
    // until the response has begun; it is moved back once it has.
    std::string start(
        const Url                 &url,
        std::string_view           method,
        const std::vector<Header> &headers,
        std::string               *body,
        int                        timeoutMs,
        bool                       upgrade,
        ULONGLONG                  deadline,
        const std::atomic<bool>   &cancel
    ) {
        const HINTERNET s = session();
        if (!s)
            return failure(GetLastError());
        connect = WinHttpConnect(s, wide(url.host).c_str(), INTERNET_PORT(url.port), 0);
        if (!connect)
            return failure(GetLastError());
        // The target is already percent-encoded (Url keeps it as given).
        request = WinHttpOpenRequest(
            connect,
            wide(method).c_str(),
            wide(url.target).c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            (url.secure() ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_ESCAPE_DISABLE |
                WINHTTP_FLAG_ESCAPE_DISABLE_QUERY
        );
        if (!request)
            return failure(GetLastError());
        if (!attach(request, op))
            return failure(GetLastError());
        // Redirects and cookies are the common layer's job (client.cpp).
        DWORD off = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        // Our own deadline covers the whole exchange; these only stop WinHTTP
        // from giving up earlier (0 = never). A WebSocket's receive timeout
        // would carry over to the open socket, so it stays infinite.
        const int t = timeoutMs > 0 ? timeoutMs : 0;
        WinHttpSetTimeouts(request, t, t, t, upgrade ? 0 : t);
        if (upgrade && !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            return failure(GetLastError());
        if (upgrade) { // the session's keepalive, in case this Windows reads it per request
            DWORD keepAlive = kWsKeepAliveMs;
            WinHttpSetOption(
                request, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof keepAlive
            );
        }
        for (const auto &h : headers) {
            op->headers += wide(h.name);
            op->headers += L": ";
            op->headers += wide(h.value);
            op->headers += L"\r\n";
        }
        if (body)
            op->body = std::move(*body);
        if (!WinHttpSendRequest(
                request,
                op->headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : op->headers.c_str(),
                DWORD(op->headers.size()),
                op->body.empty() ? nullptr : op->body.data(),
                DWORD(op->body.size()),
                DWORD(op->body.size()),
                reinterpret_cast<DWORD_PTR>(op)
            ))
            return failure(GetLastError());
        if (std::string e = await(deadline, cancel); !e.empty())
            return e;
        if (!WinHttpReceiveResponse(request, nullptr))
            return failure(GetLastError());
        std::string e = await(deadline, cancel);
        if (e.empty() && body) // WinHTTP is done with it
            *body = std::move(op->body);
        return e;
    }
    DWORD status() const {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &code,
            &len,
            WINHTTP_NO_HEADER_INDEX
        );
        return code;
    }
    // Every header line in arrival order (repeated Set-Cookie stay separate).
    void headers(std::vector<Header> &out) const {
        DWORD len = 0;
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr,
            &len,
            WINHTTP_NO_HEADER_INDEX
        );
        std::wstring raw(len / sizeof(wchar_t) + 1, L'\0');
        if (!WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX,
                raw.data(),
                &len,
                WINHTTP_NO_HEADER_INDEX
            ))
            return;
        const std::string      text = headerBytes(raw.data(), len / sizeof(wchar_t));
        const size_t           eol  = text.find("\r\n"); // past the status line
        const std::string_view rest =
            eol == std::string::npos ? std::string_view() : std::string_view(text).substr(eol + 2);
        parseHeaderLines(rest, &out); // WinHTTP validated them: nothing is dropped
    }
    // The whole body (WinHTTP has already undone any chunked encoding), into
    // `body` or, when there is one, `sink` (bodySink).
    std::string read(
        std::string             &body,
        Handler                 *sink,
        ULONGLONG                deadline,
        const std::atomic<bool> &cancel,
        const Progress          &progress,
        int64_t                  total
    ) {
        if (total > 0 && !sink) // one allocation for the whole body (a sane size of one)
            body.reserve(size_t(std::min<int64_t>(total, int64_t(64) << 20)));
        int64_t got = 0;
        for (;;) {
            if (!WinHttpReadData(request, op->buf, kChunk, nullptr))
                return failure(GetLastError());
            if (std::string e = await(deadline, cancel); !e.empty())
                return e;
            if (op->bytes == 0)
                return {};
            if (!sink)
                body.append(op->buf, op->bytes);
            else if (!sink->body(op->buf, op->bytes))
                return "sink";
            got += int64_t(op->bytes);
            if (progress)
                progress(got, total);
        }
    }
};

class WinWsConn final : public WsConn {
public:
    ~WinWsConn() override {
        teardown();
        _ctx->release();
    }

    bool connect(
        const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
    ) override {
        _x.wake            = _ctx->wakeEv;
        const ULONGLONG dl = deadlineAfter(timeoutMs);
        std::string     e  = _x.start(url, "GET", headers, nullptr, timeoutMs, true, dl, _aborted);
        if (e.empty()) {
            const DWORD status = _x.status();
            if (status != 101) {
                e = str::concat({"handshake: status ", str::number(int64_t(status))});
            } else {
                _ws =
                    WinHttpWebSocketCompleteUpgrade(_x.request, reinterpret_cast<DWORD_PTR>(_ctx));
                if (_ws)
                    _ctx->retain(); // the socket handle's, until its HANDLE_CLOSING
                else
                    e = str::concat({"handshake: ", failure(GetLastError())});
            }
        }
        _x.closeRequest(); // the socket handle lives on without it
        if (!e.empty() && error)
            *error = e;
        return e.empty();
    }

    bool recv(WsMessage &msg) override {
        WsCtx &c     = *_ctx;
        msg          = WsMessage();
        bool reading = false;
        for (;;) {
            if (_aborted.load() || !_ws)
                return finish(msg, 1006);
            if (!reading) {
                if (WinHttpWebSocketReceive(_ws, c.buf, kChunk, nullptr, nullptr) != NO_ERROR)
                    return finish(msg, 1006);
                reading = true;
            }
            pump();
            DWORD timeout = INFINITE;
            if (_closeDeadline) {
                const ULONGLONG now = GetTickCount64();
                timeout             = now >= _closeDeadline ? 0 : DWORD(_closeDeadline - now);
            }
            HANDLE      evs[3] = {c.readEv, c.writeEv, c.wakeEv};
            const DWORD w      = WaitForMultipleObjects(3, evs, FALSE, timeout);
            if (w == WAIT_TIMEOUT) // the peer never answered our close frame
                return finish(msg, 1006);
            if (w == WAIT_OBJECT_0 + 1) {
                writeDone();
                continue;
            }
            if (w != WAIT_OBJECT_0)
                continue; // woken for a send, a close or an abort: see above
            reading = false;
            if (c.readErr)
                return finish(msg, 1006);
            msg.data.append(c.buf, c.readBytes);
            switch (c.readType) {
            case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
                msg.kind = WsMessage::Text;
                return true;
            case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
                msg.kind = WsMessage::Binary;
                return true;
            case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
                return peerClosed(msg);
            default:
                continue; // a fragment: keep reading into the same message
            }
        }
    }

    bool send(std::string_view data, bool text) override {
        std::lock_guard<std::mutex> lock(_ctx->mutex);
        if (_ctx->ended || _ctx->closeCode || _ctx->queued + data.size() > kMaxQueued)
            return false;
        _ctx->out.push_back({std::string(data), text});
        _ctx->queued += data.size();
        SetEvent(_ctx->wakeEv); // the reader thread makes the WinHTTP call
        return true;
    }

    // WinHTTP's WebSocket API has no call that sends a ping. It keeps the
    // connection alive by itself instead: an unsolicited pong (RFC 6455's
    // one-way heartbeat) every kWsKeepAliveMs, set on the session.
    bool ping(std::string_view) override { return false; }

    void close(int code) override {
        std::lock_guard<std::mutex> lock(_ctx->mutex);
        if (!_ctx->closeCode)
            _ctx->closeCode = code > 0 ? code : 1000;
        SetEvent(_ctx->wakeEv);
    }

    void abort() override {
        _aborted.store(true);
        SetEvent(_ctx->wakeEv);
    }

private:
    // Starts the next queued send, or the close frame once the queue is
    // empty. Reader thread only, so every call on _ws is made by one thread.
    // WinHTTP allows one send (or shutdown) in flight at a time.
    void pump() {
        if (_writing || !_ws)
            return;
        std::lock_guard<std::mutex> lock(_ctx->mutex);
        if (!_ctx->out.empty()) {
            WsCtx::Out &o    = _ctx->out.front();
            const auto  type = o.text ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                                      : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            if (WinHttpWebSocketSend(_ws, type, o.data.data(), DWORD(o.data.size())) == NO_ERROR) {
                _writing = Sending;
            } else {
                _ctx->queued -= o.data.size();
                _ctx->out.pop_front(); // a broken socket: the pending receive ends it
            }
        } else if (_ctx->closeCode && !_shutdownSent) {
            shutdown(USHORT(_ctx->closeCode));
            _closeDeadline = GetTickCount64() + 5000;
        }
    }
    void shutdown(USHORT code) {
        _shutdownSent = true;
        if (WinHttpWebSocketShutdown(_ws, code, nullptr, 0) == NO_ERROR)
            _writing = ShuttingDown;
    }
    void writeDone() {
        if (_writing == Sending) {
            std::lock_guard<std::mutex> lock(_ctx->mutex);
            _ctx->queued -= _ctx->out.front().data.size();
            _ctx->out.pop_front();
        }
        _writing = Idle;
    }
    // The peer's close frame: answer it (after a send in flight, briefly),
    // then report the peer's code and reason.
    bool peerClosed(WsMessage &msg) {
        USHORT code = 0;
        char   reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH];
        DWORD  len = 0;
        if (WinHttpWebSocketQueryCloseStatus(_ws, &code, reason, sizeof reason, &len) != NO_ERROR)
            code = 1005, len = 0;
        if (_writing && WaitForSingleObject(_ctx->writeEv, 1000) == WAIT_OBJECT_0)
            writeDone();
        if (!_writing && !_shutdownSent) {
            shutdown(code);
            if (_writing && WaitForSingleObject(_ctx->writeEv, 1000) == WAIT_OBJECT_0)
                writeDone();
        }
        finish(msg, code);
        msg.data.assign(reason, len);
        return false;
    }
    bool finish(WsMessage &msg, int code) {
        msg.kind = WsMessage::Closed;
        msg.code = code;
        msg.data.clear();
        teardown();
        return false;
    }
    // Closing the handle cancels what is in flight; that may still complete
    // into *_ctx, which the handle keeps alive until its HANDLE_CLOSING.
    void teardown() {
        {
            std::lock_guard<std::mutex> lock(_ctx->mutex);
            _ctx->ended = true;
        }
        if (_ws)
            WinHttpCloseHandle(_ws);
        _ws = nullptr;
    }

    enum Writing : uint8_t { Idle, Sending, ShuttingDown };

    WsCtx            *_ctx = new WsCtx;
    Exchange          _x; // the upgrade request (closed once upgraded) and the connect handle
    HINTERNET         _ws            = nullptr;
    // Reader thread only.
    Writing           _writing       = Idle;
    bool              _shutdownSent  = false;
    ULONGLONG         _closeDeadline = 0;
    std::atomic<bool> _aborted{false};
};

} // namespace

// A manual-reset event: once set it stays signalled, so every later wait
// returns at once and sees the flag.
Cancel::Cancel() : _os(intptr_t(CreateEventW(nullptr, TRUE, FALSE, nullptr))) {}

Cancel::~Cancel() {
    if (_os)
        CloseHandle(HANDLE(_os));
}

void Cancel::set() {
    _flag.store(true, std::memory_order_release);
    if (_os)
        SetEvent(HANDLE(_os));
}

void perform(
    const Url &url, Request &req, Response &resp, const Cancel &cancel, const Progress &progress
) {
    Exchange        x;
    const ULONGLONG deadline = deadlineAfter(req.timeoutMs);
    x.wake                   = HANDLE(cancel.os()); // null: sliced waits
    std::string e            = x.start(
        url, req.method, req.headers, &req.body, req.timeoutMs, false, deadline, cancel.flag()
    );
    if (e.empty()) {
        resp.status = int(x.status());
        x.headers(resp.headers);
        e = x.read(
            resp.body,
            bodySink(req, resp.status),
            deadline,
            cancel.flag(),
            progress,
            contentLength(resp.headers)
        );
    }
    if (!e.empty()) {
        resp.status = 0;
        resp.headers.clear();
        resp.body.clear();
        resp.error = std::move(e);
    }
}

// WinHTTP owns the pool (per session; idle connections expire by
// themselves); there is no call to drop them short of closing the session,
// which other clients may still be using.
void closeIdleConnections() {}

void releaseCaches() {}

std::unique_ptr<WsConn> makeWsConn() {
    return std::make_unique<WinWsConn>();
}

int loopbackPort() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started)
        return 0;
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return 0;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len              = sizeof addr;
    int port             = 0;
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        port = ntohs(addr.sin_port);
    closesocket(s);
    return port;
}

} // namespace net::detail
