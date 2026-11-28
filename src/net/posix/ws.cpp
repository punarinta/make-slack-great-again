// RFC 6455 WebSocket client for the POSIX transport.
//
// One mbedtls_ssl_context must never be used from two threads, yet send()
// comes from the UI thread while recv() blocks on the reader thread. So the
// reader thread does all socket I/O: send() and close() only frame the data,
// queue it under a mutex and poke a wake pipe; recv() polls the socket and
// that pipe together and flushes the queue between reads.
#include "base/crypto.h"
#include "base/str.h"
#include "net/posix/posix.h"
#include "net/transport.h"

#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <unistd.h>

namespace net::detail {

namespace {

constexpr size_t  kMaxMessage = 32 * 1024 * 1024;
constexpr size_t  kMaxQueued  = 64 * 1024 * 1024; // unsent data beyond this: send() fails
constexpr int64_t kCloseWait  = 5000;             // for the peer's answer to our close

enum Op : uint8_t { Cont = 0, TextOp = 1, BinOp = 2, CloseOp = 8, PingOp = 9, PongOp = 10 };

// A masked client frame (every client frame must be masked).
std::string frame(uint8_t op, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 14);
    out += char(0x80 | op);
    const size_t n = payload.size();
    if (n < 126) {
        out += char(0x80 | n);
    } else if (n <= 0xffff) {
        out += char(0x80 | 126);
        out += char(n >> 8);
        out += char(n);
    } else {
        out += char(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            out += char(uint64_t(n) >> (i * 8));
    }
    uint8_t mask[4];
    crypto::randomBytes(mask, 4);
    out.append(reinterpret_cast<const char *>(mask), 4);
    const size_t at = out.size();
    out.append(payload);
    for (size_t i = 0; i < n; ++i)
        out[at + i] = char(out[at + i] ^ mask[i & 3]);
    return out;
}

std::string closePayload(int code, std::string_view reason = {}) {
    std::string p;
    if (code > 0) {
        p += char(code >> 8);
        p += char(code);
        p += reason.substr(0, 123);
    }
    return p;
}

class PosixWs final : public WsConn {
public:
    PosixWs() {
        if (!wakePipe(_wake))
            _wake[0] = _wake[1] = -1;
    }
    ~PosixWs() override {
        _stream.reset(); // before the pipe: nothing polls it any more
        for (int fd : _wake)
            if (fd >= 0)
                ::close(fd);
    }

    bool connect(
        const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
    ) override;
    bool recv(WsMessage &msg) override;
    bool send(std::string_view data, bool text) override {
        return enqueue(frame(text ? TextOp : BinOp, data), 0);
    }
    bool ping(std::string_view payload) override {
        return enqueue(frame(PingOp, payload.substr(0, 125)), 0);
    }
    void close(int code) override {
        enqueue(frame(CloseOp, closePayload(code)), code ? code : 1000);
    }
    void abort() override {
        _aborted.store(true);
        wake();
    }

private:
    void wake() {
        if (_wake[1] >= 0) {
            const char               c = 1;
            [[maybe_unused]] ssize_t r = ::write(_wake[1], &c, 1);
        }
    }
    // Any thread. closeCode != 0: this is our close frame.
    bool enqueue(std::string bytes, int closeCode) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_closing || _finished || _queue.size() + bytes.size() > kMaxQueued)
                return false;
            _queue += bytes;
            if (closeCode)
                _closing = true;
        }
        wake();
        return true;
    }
    bool finish(WsMessage &msg, int code, std::string reason) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _finished = true;
        }
        msg      = WsMessage();
        msg.kind = WsMessage::Closed;
        msg.code = code;
        msg.data = std::move(reason);
        _stream.reset();
        return false;
    }
    // Writes what it can of _out without blocking. False: the stream died.
    bool flush() {
        while (_outPos < _out.size()) {
            const long r = _stream->tryWrite(_out.data() + _outPos, _out.size() - _outPos);
            if (r == Stream::Again)
                break;
            if (r <= 0)
                return false;
            _outPos += size_t(r);
        }
        if (_outPos == _out.size()) {
            _out.clear();
            _outPos = 0;
        }
        return true;
    }
    // One frame from _in, if whole: 1 handled, 0 need more, -1 → *msg done.
    int  parse(WsMessage &msg, bool *gotMessage);
    // Our close went out (or the peer's answer is due): wait for the
    // deadline before giving up on a clean close.
    bool flushBlocking(int64_t until);

    std::unique_ptr<Stream> _stream;
    int                     _wake[2] = {-1, -1};
    std::atomic<bool>       _aborted{false};

    std::mutex  _mutex; // guards the three below
    std::string _queue; // framed, waiting for the reader thread
    bool        _closing  = false;
    bool        _finished = false;

    // Reader thread only.
    std::string _out;
    size_t      _outPos = 0;
    std::string _in;
    size_t      _inPos = 0;
    std::string _msg;
    int         _msgOp         = -1; // opcode of a fragmented message in progress
    bool        _closeSent     = false;
    int64_t     _closeDeadline = 0;
};

bool PosixWs::connect(
    const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
) {
    Waiter w;
    w.cancel   = &_aborted;
    w.wakeFd   = _wake[0];
    w.deadline = timeoutMs > 0 ? nowMs() + timeoutMs : 0;
    if (_wake[0] < 0) {
        *error = "connect: no pipe";
        return false;
    }
    _stream = std::make_unique<Stream>();
    if (!_stream->open(url, w, error))
        return false;

    uint8_t nonce[16];
    crypto::randomBytes(nonce, sizeof nonce);
    const std::string key =
        crypto::base64(std::string_view(reinterpret_cast<const char *>(nonce), sizeof nonce));
    std::string req = str::concat(
        {"GET ",
         url.target,
         " HTTP/1.1\r\nHost: ",
         url.authority(),
         "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: ",
         key,
         "\r\nSec-WebSocket-Version: 13\r\n"}
    );
    for (const auto &h : headers)
        req += str::concat({h.name, ": ", h.value, "\r\n"});
    req += "\r\n";
    if (!_stream->writeAll(req, w, error))
        return false;

    Head       head;
    bool       gotAny = false;
    const long len    = readHead(*_stream, &_in, w, &head, error, &gotAny);
    if (len < 0) {
        if (str::startsWith(*error, "protocol"))
            *error = "handshake: " + error->substr(10);
        return false;
    }
    if (head.status != 101) {
        *error = str::concat({"handshake: status ", str::number(head.status)});
        return false;
    }
    const std::string expect = crypto::base64(
        crypto::bytes(crypto::sha1(str::concat({key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"})))
    );
    if (!iequals(headerValue(head.headers, "upgrade"), "websocket") ||
        headerValue(head.headers, "sec-websocket-accept") != expect) {
        *error = "handshake: bad accept";
        return false;
    }
    _inPos = size_t(len); // frames may have come in with the head
    return true;
}

int PosixWs::parse(WsMessage &msg, bool *gotMessage) {
    const auto  *p     = reinterpret_cast<const uint8_t *>(_in.data()) + _inPos;
    const size_t avail = _in.size() - _inPos;
    if (avail < 2)
        return 0;
    const bool    fin = p[0] & 0x80;
    const uint8_t op  = p[0] & 0x0f;
    if ((p[0] & 0x70) || (p[1] & 0x80)) // reserved bits, or a masked server frame
        return finish(msg, 1002, "protocol"), -1;
    uint64_t n   = p[1] & 0x7f;
    size_t   hdr = 2;
    if (n == 126) {
        if (avail < 4)
            return 0;
        n   = uint64_t(p[2]) << 8 | p[3];
        hdr = 4;
    } else if (n == 127) {
        if (avail < 10)
            return 0;
        n = 0;
        for (int i = 0; i < 8; ++i)
            n = n << 8 | p[2 + i];
        hdr = 10;
    }
    if (op >= 8 && (n > 125 || !fin))
        return finish(msg, 1002, "protocol"), -1;
    if (n > kMaxMessage || _msg.size() + n > kMaxMessage) {
        _out += frame(CloseOp, closePayload(1009));
        flushBlocking(nowMs() + 1000);
        return finish(msg, 1009, "message too big"), -1;
    }
    if (avail < hdr + n)
        return 0;
    const std::string_view payload(_in.data() + _inPos + hdr, size_t(n));
    _inPos += hdr + size_t(n);

    switch (op) {
    case TextOp:
    case BinOp:
    case Cont:
        if ((op == Cont) != (_msgOp >= 0))
            return finish(msg, 1002, "protocol"), -1;
        if (op != Cont)
            _msgOp = op;
        _msg.append(payload);
        if (fin) {
            msg      = WsMessage();
            msg.kind = _msgOp == TextOp ? WsMessage::Text : WsMessage::Binary;
            msg.data = std::move(_msg);
            _msg.clear();
            _msgOp      = -1;
            *gotMessage = true;
        }
        return 1;
    case PingOp:
        _out += frame(PongOp, payload);
        return 1;
    case PongOp:
        return 1;
    case CloseOp: {
        const int code =
            payload.size() >= 2 ? (uint8_t(payload[0]) << 8 | uint8_t(payload[1])) : 1005;
        std::string reason(payload.size() > 2 ? payload.substr(2) : std::string_view());
        if (!_closeSent) { // the peer started it: answer with its code, then go
            _out += frame(CloseOp, payload.substr(0, payload.size() >= 2 ? 2 : 0));
            flushBlocking(nowMs() + 1000);
        }
        return finish(msg, code, std::move(reason)), -1;
    }
    default:
        return finish(msg, 1002, "protocol"), -1;
    }
}

bool PosixWs::flushBlocking(int64_t until) {
    Waiter w;
    w.deadline = until;
    w.cancel   = &_aborted;
    w.wakeFd   = _wake[0]; // abort() interrupts at once, not at a 250 ms slice
    while (!_out.empty()) {
        if (!flush())
            return false;
        if (_out.empty())
            break;
        if (waitFd(_stream->fd(), _stream->wantEvents(), w) != Wait::Ready)
            return false;
    }
    return true;
}

bool PosixWs::recv(WsMessage &msg) {
    if (!_stream)
        return finish(msg, 1006, "");
    for (;;) {
        if (_aborted.load())
            return finish(msg, 1006, "");
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_queue.empty()) {
                if (_closing && !_closeSent) {
                    _closeSent     = true; // the close frame is the last thing queued
                    _closeDeadline = nowMs() + kCloseWait;
                }
                _out += _queue;
                _queue.clear();
            }
        }
        // Whole frames first: one read may have brought several.
        bool got = false;
        for (;;) {
            const int r = parse(msg, &got);
            if (r < 0)
                return false;
            if (r == 0 || got)
                break;
        }
        if (_inPos == _in.size()) {
            _in.clear();
            _inPos = 0;
        } else if (_inPos > 64 * 1024) {
            _in.erase(0, _inPos);
            _inPos = 0;
        }
        if (!flush())
            return finish(msg, 1006, "");
        if (got)
            return true;
        if (_closeSent && nowMs() >= _closeDeadline)
            return finish(msg, 1006, "");

        if (!_stream->pending()) {
            short events = POLLIN;
            if (!_out.empty())
                events |= _stream->wantEvents() ? _stream->wantEvents() : POLLOUT;
            Waiter w;
            w.cancel     = &_aborted;
            w.wakeFd     = _wake[0];
            w.deadline   = _closeSent ? _closeDeadline : 0;
            const Wait r = waitFd(_stream->fd(), events, w, true);
            if (r == Wait::Cancelled || r == Wait::Timeout)
                continue; // handled at the top of the loop
            if (r == Wait::Woken)
                continue; // something was queued
        }
        const size_t old = _in.size();
        _in.resize(old + 16 * 1024);
        const long r = _stream->tryRead(&_in[old], 16 * 1024);
        _in.resize(old + size_t(r > 0 ? r : 0));
        if (r == 0 || r == Stream::Fail)
            return finish(msg, 1006, ""); // gone without a close frame
    }
}

} // namespace

std::unique_ptr<WsConn> makeWsConn() {
    return std::make_unique<PosixWs>();
}

} // namespace net::detail
