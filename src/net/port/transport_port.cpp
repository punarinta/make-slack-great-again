// The transport (transport.h) over the port contract's network calls
// (port/port_net.h): one blocking exchange per worker thread, the answer
// streamed back through callbacks. Name resolution, sockets and TLS are the
// port's; redirects, cookies, threads and delivery stay in client.cpp and
// websocket.cpp as on every OS.
#include "net/transport.h"

#include "port/port.h"

#include <cstring>

namespace net::detail {

namespace {

port_cancel *portCancel(const Cancel &c) {
    return reinterpret_cast<port_cancel *>(c.os());
}

std::vector<port_header> portHeaders(const std::vector<Header> &headers) {
    std::vector<port_header> out;
    out.reserve(headers.size());
    for (const Header &h : headers)
        out.push_back({h.name.data(), h.name.size(), h.value.data(), h.value.size()});
    return out;
}

// The module's error words ("dns", "timeout" …, see Response::error) for a
// port code, with the port's detail after a colon for the log.
std::string errorText(int rc, const char *detail) {
    const char *word = "connect";
    switch (rc) {
    case PORT_E_DNS:
        word = "dns";
        break;
    case PORT_E_TLS:
        word = "tls";
        break;
    case PORT_E_TIMEOUT:
        word = "timeout";
        break;
    case PORT_E_CANCELLED:
        return "cancelled";
    case PORT_E_PROTOCOL:
        word = "protocol";
        break;
    case PORT_E_UNSUPPORTED:
    case PORT_E_NOSYS:
        word = "unsupported";
        break;
    case PORT_E_INVAL:
        word = "url";
        break;
    default:
        break;
    }
    if (!detail || !*detail || rc == PORT_E_TIMEOUT)
        return word;
    return std::string(word) + ": " + detail;
}

struct Exchange {
    const Request  *req;
    Response       *resp;
    Handler        *sink = nullptr;
    const Progress *progress;
    int64_t         got = 0, total = 0;
    bool            sinkFailed = false;
};

int onHead(void *user, int status, const port_header *h, size_t n) {
    auto &x        = *static_cast<Exchange *>(user);
    x.resp->status = status;
    x.resp->headers.clear();
    for (size_t i = 0; i < n; ++i)
        x.resp->headers.push_back(
            {std::string(h[i].name, h[i].name_len), std::string(h[i].value, h[i].value_len)}
        );
    x.sink  = bodySink(*x.req, status);
    x.total = contentLength(x.resp->headers);
    return 0;
}

int onBody(void *user, const void *data, size_t n) {
    auto &x = *static_cast<Exchange *>(user);
    if (x.sink) {
        if (!x.sink->body(static_cast<const char *>(data), n)) {
            x.sinkFailed = true;
            return 1;
        }
    } else {
        x.resp->body.append(static_cast<const char *>(data), n);
    }
    x.got += int64_t(n);
    if (*x.progress)
        (*x.progress)(x.got, x.total);
    return 0;
}

} // namespace

// The port's cancel object is the OS object here: an exchange blocked inside
// the port watches it.
Cancel::Cancel() {
    port_cancel *c = nullptr;
    _os            = port_cancel_create(&c) == PORT_OK ? reinterpret_cast<intptr_t>(c) : 0;
}

Cancel::~Cancel() {
    port_cancel_destroy(portCancel(*this));
}

void Cancel::set() {
    _flag.store(true, std::memory_order_release);
    if (port_cancel *c = portCancel(*this))
        port_cancel_set(c);
}

void perform(
    const Url &url, Request &req, Response &resp, const Cancel &cancel, const Progress &progress
) {
    const std::string              target  = url.str();
    const std::vector<port_header> headers = portHeaders(req.headers);
    port_http_request              r{};
    r.size         = sizeof r;
    r.method       = req.method.c_str();
    r.url          = target.c_str();
    r.headers      = headers.data();
    r.header_count = headers.size();
    r.body         = req.body.data();
    r.body_len     = req.body.size();
    r.timeout_ms   = req.timeoutMs;
    r.cancel       = portCancel(cancel);

    resp = Response();
    Exchange          x{&req, &resp, nullptr, &progress};
    port_http_handler h{};
    h.size                = sizeof h;
    h.user                = &x;
    h.on_head             = onHead;
    h.on_body             = onBody;
    char      detail[256] = "";
    const int rc          = port_http_perform(&r, &h, detail, sizeof detail);
    if (rc == PORT_OK)
        return;
    resp = Response();
    if (x.sinkFailed)
        resp.error = "sink";
    else if (cancel.isSet())
        resp.error = "cancelled";
    else
        resp.error = errorText(rc, detail);
}

void closeIdleConnections() {
    port_net_release();
}

void releaseCaches() {
    port_net_release();
}

int loopbackPort() {
    return port_net_loopback_port();
}

namespace {

class PortWs final : public WsConn {
public:
    // The cancel exists before connect() runs: abort() may come from another
    // thread at any time.
    PortWs() { port_cancel_create(&_cancel); }
    ~PortWs() override {
        port_ws_destroy(_ws);
        port_cancel_destroy(_cancel);
    }

    bool connect(
        const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
    ) override {
        const std::string              target      = url.str();
        const std::vector<port_header> hs          = portHeaders(headers);
        char                           detail[256] = "";
        port_ws                       *ws          = nullptr;
        const int                      rc          = port_ws_connect(
            target.c_str(), hs.data(), hs.size(), timeoutMs, _cancel, &ws, detail, sizeof detail
        );
        if (rc != PORT_OK) {
            // A refused upgrade is a "handshake" failure in the module's words.
            *error = rc != PORT_E_PROTOCOL ? errorText(rc, detail)
                     : detail[0]           ? std::string("handshake: ") + detail
                                           : std::string("handshake");
            return false;
        }
        _ws = ws;
        _open.store(true, std::memory_order_release);
        return true;
    }

    bool recv(WsMessage &msg) override {
        if (!_ws)
            return closed(msg, 1006, {});
        port_ws_message m{};
        m.size = sizeof m;
        if (port_ws_recv(_ws, &m) != PORT_OK)
            return closed(msg, 1006, {});
        const std::string data(m.data ? m.data : "", m.len);
        port_ws_message_free(&m);
        if (m.kind == PORT_WS_CLOSED)
            return closed(msg, m.close_code, data);
        msg.kind = m.kind == PORT_WS_TEXT ? WsMessage::Text : WsMessage::Binary;
        msg.data = data;
        return true;
    }

    bool send(std::string_view data, bool text) override {
        return _open.load(std::memory_order_acquire) &&
               port_ws_send(_ws, data.data(), data.size(), text ? 1 : 0) == PORT_OK;
    }

    bool ping(std::string_view payload) override {
        return _open.load(std::memory_order_acquire) &&
               port_ws_ping(_ws, payload.data(), payload.size()) == PORT_OK;
    }

    void close(int code) override {
        if (_open.load(std::memory_order_acquire))
            port_ws_close(_ws, code);
    }

    // Before connect() returns (the destructor path), only the cancel can
    // stop it; after, the port's abort.
    void abort() override {
        if (_cancel)
            port_cancel_set(_cancel);
        if (_open.load(std::memory_order_acquire))
            port_ws_abort(_ws);
    }

private:
    bool closed(WsMessage &msg, int code, std::string reason) {
        msg.kind = WsMessage::Closed;
        msg.code = code;
        msg.data = std::move(reason);
        return false;
    }

    port_ws          *_ws     = nullptr;
    port_cancel      *_cancel = nullptr;
    std::atomic<bool> _open{false};
};

} // namespace

std::unique_ptr<WsConn> makeWsConn() {
    return std::make_unique<PortWs>();
}

} // namespace net::detail
