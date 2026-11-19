// net::WebSocket: one reader thread per socket runs the transport's
// blocking connect/recv and posts every event to the UI thread.
#include "base/thread.h"
#include "net/net.h"
#include "net/transport.h"
#include "plat/plat.h"

namespace net {

struct WebSocket::Impl : std::enable_shared_from_this<WebSocket::Impl> {
    explicit Impl(plat::App &a) : app(a) {}

    plat::App &app;
    WebSocket *owner = nullptr; // UI thread; null once the WebSocket is gone
    bool       open  = false;   // UI thread
    uint64_t   epoch = 0;       // UI thread: bumps per open(), stale events are dropped

    base::Mutex                     mutex; // guards conn against the destructor
    std::unique_ptr<detail::WsConn> conn;
    base::Thread                    reader;

    // Every reader event goes through this one closure type: a single heap
    // allocation per message and a single handler body.
    enum Kind : uint8_t { Opened, Text, Binary, Closed };
    void post(uint64_t ep, Kind kind, int code = 0, std::string data = {}) {
        std::weak_ptr<Impl> weak = weak_from_this();
        app.post([weak, ep, kind, code, data = std::move(data)]() mutable {
            auto self = weak.lock();
            if (!self || !self->owner || self->epoch != ep)
                return;
            WebSocket &ws = *self->owner;
            switch (kind) {
            case Opened:
                self->open = true;
                if (auto f = ws.onOpen)
                    f();
                break;
            case Text:
                if (auto f = ws.onText)
                    f(std::move(data));
                break;
            case Binary:
                if (auto f = ws.onBinary)
                    f(std::move(data));
                break;
            case Closed:
                self->open = false;
                if (auto f = ws.onClosed)
                    f(code, std::move(data));
                break;
            }
        });
    }
    void stop() {
        {
            base::Lock lock(mutex);
            if (conn)
                conn->abort();
        }
        reader.join();
        base::Lock lock(mutex);
        conn.reset();
    }
};

WebSocket::WebSocket(plat::App &app) : _impl(std::make_shared<Impl>(app)) {
    _impl->owner = this;
}

WebSocket::~WebSocket() {
    _impl->owner = nullptr;
    _impl->stop();
}

void WebSocket::open(std::string url, std::vector<Header> headers, int timeoutMs) {
    Impl &d = *_impl;
    d.stop();
    d.open            = false;
    const uint64_t ep = ++d.epoch;
    Url            u;
    if (!u.parse(url) || (u.scheme != "ws" && u.scheme != "wss")) {
        d.post(ep, Impl::Closed, 0, "url");
        return;
    }
    {
        base::Lock lock(d.mutex);
        d.conn = detail::makeWsConn();
    }
    detail::WsConn *conn = d.conn.get();
    Impl           *raw  = &d; // stop() joins the reader before Impl or conn go
    auto read = [raw, conn, ep, u = std::move(u), headers = std::move(headers), timeoutMs] {
        std::string err;
        if (!conn->connect(u, headers, timeoutMs, &err)) {
            raw->post(ep, Impl::Closed, 0, std::move(err));
            return;
        }
        raw->post(ep, Impl::Opened);
        detail::WsMessage msg;
        while (conn->recv(msg)) {
            if (msg.kind == detail::WsMessage::Text)
                raw->post(ep, Impl::Text, 0, std::move(msg.data));
            else if (msg.kind == detail::WsMessage::Binary)
                raw->post(ep, Impl::Binary, 0, std::move(msg.data));
            msg = detail::WsMessage();
        }
        raw->post(ep, Impl::Closed, msg.code ? msg.code : 1006, std::move(msg.data));
    };
    d.reader.start(std::move(read), detail::kThreadStack);
}

bool WebSocket::isOpen() const {
    return _impl->open;
}

void WebSocket::sendText(std::string_view text) {
    Impl &d = *_impl;
    if (!d.open)
        return;
    base::Lock lock(d.mutex);
    if (d.conn)
        d.conn->send(text, true);
}

bool WebSocket::ping(std::string_view payload) {
    Impl &d = *_impl;
    if (!d.open)
        return false;
    base::Lock lock(d.mutex);
    return d.conn && d.conn->ping(payload);
}

void WebSocket::close(int code) {
    Impl      &d = *_impl;
    base::Lock lock(d.mutex);
    if (d.conn)
        d.conn->close(code);
}

} // namespace net
