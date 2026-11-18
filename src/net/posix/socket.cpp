// Sockets for the POSIX transport: name lookup, non-blocking connect and the
// Stream every exchange runs over (see posix.h).
#include "base/str.h"
#include "base/thread.h"
#include "net/posix/posix.h"
#include "net/transport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <climits>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace net::detail {

// tls.cpp
struct TlsConn;
TlsConn *tlsNew(int fd, const Url &url, std::string *error);
void     tlsFree(TlsConn *c);
long     tlsHandshake(TlsConn *c, short *want, std::string *error);
long     tlsRead(TlsConn *c, char *buf, size_t n, short *want, std::string *error);
long     tlsWrite(TlsConn *c, const char *buf, size_t n, short *want, std::string *error);
bool     tlsPending(const TlsConn *c);

// The read end is readable from set() on, so a waitFd with it as the wake
// fd returns Cancelled at once (and every later wait sees the flag first).
Cancel::Cancel() {
    int fds[2];
    if (wakePipe(fds)) {
        _os     = fds[0];
        _osWake = fds[1];
    }
}

Cancel::~Cancel() {
    if (_os >= 0)
        ::close(int(_os));
    if (_osWake >= 0)
        ::close(_osWake);
}

void Cancel::set() {
    _flag.store(true, std::memory_order_release);
    if (_osWake >= 0)
        wakeSignal(_osWake);
}

bool wakePipe(int fds[2]) {
    return ::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0;
}

void wakeSignal(int fd) {
    // EAGAIN: the pipe is full of wake bytes already, so the reader wakes.
    const char                     b = 1;
    [[maybe_unused]] const ssize_t r = ::write(fd, &b, 1);
}

Wait waitFd(int fd, short events, const Waiter &w, bool returnOnWake) {
    for (;;) {
        if (w.cancel && w.cancel->load(std::memory_order_relaxed))
            return Wait::Cancelled;
        // A wake fd interrupts at once (whoever sets the cancel flag also
        // writes it), so such a wait can block until something happens; only
        // a bare cancel flag needs polling, in 250 ms slices.
        int slice = w.wakeFd >= 0 ? -1 : 250;
        if (w.deadline) {
            const int64_t left = w.deadline - nowMs();
            if (left <= 0)
                return Wait::Timeout;
            if (slice < 0 || left < slice)
                slice = int(std::min<int64_t>(left, INT_MAX));
        }
        pollfd    p[2] = {{fd, events, 0}, {w.wakeFd, POLLIN, 0}};
        const int n    = poll(p, w.wakeFd >= 0 ? 2 : 1, slice);
        if (n < 0 && errno != EINTR)
            return Wait::Ready; // let the I/O call report what is wrong
        if (n <= 0)
            continue;
        if (w.wakeFd >= 0 && p[1].revents) {
            char drain[64];
            while (::read(w.wakeFd, drain, sizeof drain) > 0) {
            }
            if (w.cancel && w.cancel->load(std::memory_order_relaxed))
                return Wait::Cancelled;
            if (returnOnWake)
                return Wait::Woken;
        }
        if (fd >= 0 && p[0].revents)
            return Wait::Ready;
    }
}

namespace {

std::string sysError(std::string_view what, int err) {
    return str::concat({what, ": ", std::strerror(err)});
}

std::string waitError(Wait r) {
    return r == Wait::Cancelled ? "cancelled" : "timeout";
}

// One address to connect to.
struct Addr {
    sockaddr_storage sa;
    socklen_t        len = 0;
};

// What a host name resolved to, kept kDnsTtlMs: a reconnect (keep-alive
// expired, a WebSocket recycled, a burst of requests) skips the lookup
// thread. getaddrinfo doesn't report the record's TTL; a minute is about
// the short TTL that CDN and load-balancer names use. A name whose
// addresses all failed to connect is dropped, so a moved host is looked up
// again at once.
constexpr int64_t kDnsTtlMs    = 60000;
constexpr size_t  kDnsMaxHosts = 16;

struct DnsEntry {
    std::string       key; // "host:port"
    std::vector<Addr> addrs;
    int64_t           expires = 0;
};

struct DnsCache {
    std::mutex            mutex;
    std::vector<DnsEntry> entries;
};

DnsCache &dnsCache() {
    return immortal<DnsCache>();
}

std::atomic<int64_t> g_dnsLookups{0};

bool dnsCached(const std::string &key, std::vector<Addr> *out) {
    DnsCache                   &c = dnsCache();
    std::lock_guard<std::mutex> lock(c.mutex);
    const int64_t               now = nowMs();
    for (size_t i = 0; i < c.entries.size(); ++i)
        if (c.entries[i].key == key) {
            if (c.entries[i].expires <= now) {
                c.entries.erase(c.entries.begin() + ptrdiff_t(i));
                return false;
            }
            *out = c.entries[i].addrs;
            return true;
        }
    return false;
}

void dnsStore(const std::string &key, const std::vector<Addr> &addrs) {
    DnsCache                   &c = dnsCache();
    std::lock_guard<std::mutex> lock(c.mutex);
    const int64_t               now = nowMs();
    for (size_t i = 0; i < c.entries.size();)
        if (c.entries[i].key == key || c.entries[i].expires <= now)
            c.entries.erase(c.entries.begin() + ptrdiff_t(i));
        else
            ++i;
    if (c.entries.size() >= kDnsMaxHosts)
        c.entries.erase(c.entries.begin()); // the oldest
    c.entries.push_back({key, addrs, now + kDnsTtlMs});
}

void dnsForget(const std::string &key) {
    DnsCache                   &c = dnsCache();
    std::lock_guard<std::mutex> lock(c.mutex);
    for (size_t i = 0; i < c.entries.size(); ++i)
        if (c.entries[i].key == key) {
            c.entries.erase(c.entries.begin() + ptrdiff_t(i));
            return;
        }
}

void appendAddrs(const addrinfo *list, std::vector<Addr> *out) {
    for (const addrinfo *ai = list; ai; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(sockaddr_storage))
            continue;
        Addr a;
        std::memcpy(&a.sa, ai->ai_addr, ai->ai_addrlen);
        a.len = socklen_t(ai->ai_addrlen);
        out->push_back(a);
    }
}

// getaddrinfo cannot be interrupted, and a dead resolver can block it for
// tens of seconds. It runs on its own detached thread so the caller can give
// up on time; the thread writes `wake` (a self-pipe whose read end is `fd`)
// when the answer is in, so the caller sleeps on `fd`, its cancel fd and the
// deadline together. Both
// sides hold a reference; the last one out frees the state (and an answer
// nobody waits for any more).
struct Lookup {
    std::mutex       mutex;
    std::string      host, port;
    addrinfo        *result = nullptr;
    int              rc     = 0;
    int              fd     = -1;
    int              wake   = -1;
    std::atomic<int> refs{2};

    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
            return;
        if (result)
            freeaddrinfo(result);
        if (fd >= 0)
            ::close(fd);
        if (wake >= 0)
            ::close(wake);
        delete this;
    }
};

void lookupThread(Lookup *lk) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res     = nullptr;
    const int rc      = getaddrinfo(lk->host.c_str(), lk->port.c_str(), &hints, &res);
    {
        std::lock_guard<std::mutex> lock(lk->mutex);
        lk->rc     = rc;
        lk->result = rc == 0 ? res : nullptr;
    }
    wakeSignal(lk->wake);
    lk->release();
}

// False with *error ("dns: …", "cancelled", "timeout") when there is none.
bool resolve(const Url &url, const Waiter &w, std::vector<Addr> *out, std::string *error) {
    const std::string port = str::number(url.port);
    addrinfo          hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo *res     = nullptr;
    if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &res) == 0) {
        appendAddrs(res, out); // an IP literal: nothing to wait for
        freeaddrinfo(res);
        return !out->empty();
    }
    const std::string key = str::concat({url.host, ":", port});
    if (dnsCached(key, out))
        return true;

    auto *lk = new Lookup;
    lk->host = url.host;
    lk->port = port;
    if (int fds[2]; wakePipe(fds)) {
        lk->fd   = fds[0];
        lk->wake = fds[1];
    } else {
        *error = sysError("dns", errno);
        delete lk;
        return false;
    }
    base::Thread thread;
    if (!thread.start([lk] { lookupThread(lk); }, kThreadStack)) {
        ::close(lk->fd);
        ::close(lk->wake);
        delete lk;
        *error = "dns: no thread";
        return false;
    }
    thread.detach(); // a lookup that outlives the wait ends on its own
    g_dnsLookups.fetch_add(1, std::memory_order_relaxed);
    const Wait r  = waitFd(lk->fd, POLLIN, w);
    int        rc = 0;
    if (r == Wait::Ready) {
        std::lock_guard<std::mutex> lock(lk->mutex);
        rc         = lk->rc;
        res        = lk->result;
        lk->result = nullptr; // ours now
    } else {
        *error = waitError(r);
    }
    lk->release();
    if (r != Wait::Ready)
        return false;
    if (rc != 0) {
        *error = str::concat({"dns: ", gai_strerror(rc)});
        return false;
    }
    appendAddrs(res, out);
    freeaddrinfo(res);
    if (out->empty()) {
        *error = "dns: no address";
        return false;
    }
    dnsStore(key, *out);
    return true;
}

// One non-blocking connect; returns the fd or -1 with *error.
int connectOne(const Addr &ai, const Waiter &w, std::string *error) {
    const int fd = ::socket(ai.sa.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = sysError("connect", errno);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&ai.sa), ai.len) == 0)
        return fd;
    if (errno != EINPROGRESS) {
        *error = sysError("connect", errno);
        ::close(fd);
        return -1;
    }
    const Wait r = waitFd(fd, POLLOUT, w);
    if (r != Wait::Ready) {
        *error = waitError(r);
        ::close(fd);
        return -1;
    }
    int       err = 0;
    socklen_t len = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        *error = sysError("connect", err);
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int64_t dnsLookups() {
    return g_dnsLookups.load(std::memory_order_relaxed);
}

void releaseDns() {
    DnsCache                   &c = dnsCache();
    std::lock_guard<std::mutex> lock(c.mutex);
    std::vector<DnsEntry>().swap(c.entries);
}

namespace {
std::atomic<int> g_liveStreams{0};
}

int liveStreams() {
    return g_liveStreams.load(std::memory_order_acquire);
}

Stream::Stream() {
    g_liveStreams.fetch_add(1, std::memory_order_relaxed);
}

Stream::~Stream() {
    if (_tls)
        tlsFree(_tls);
    if (_fd >= 0)
        ::close(_fd);
    g_liveStreams.fetch_sub(1, std::memory_order_release);
}

bool Stream::open(const Url &url, const Waiter &w, std::string *error) {
    std::vector<Addr> addrs;
    if (!resolve(url, w, &addrs, error))
        return false;
    for (size_t i = 0; i < addrs.size(); ++i) {
        // A black-holed address (broken IPv6) must not eat the whole
        // timeout while another one would answer.
        Waiter one = w;
        if (i + 1 < addrs.size()) {
            const int64_t cap = nowMs() + 4000;
            if (!one.deadline || one.deadline > cap)
                one.deadline = cap;
        }
        _fd = connectOne(addrs[i], one, error);
        if (_fd >= 0 || *error == "cancelled" ||
            (*error == "timeout" && one.deadline == w.deadline))
            break;
    }
    if (_fd < 0) {
        if (*error != "cancelled") // the host may have moved: look it up afresh next time
            dnsForget(str::concat({url.host, ":", str::number(url.port)}));
        return false;
    }
    if (!url.secure())
        return true;
    _tls = tlsNew(_fd, url, error);
    if (!_tls)
        return false;
    for (;;) {
        const long r = tlsHandshake(_tls, &_want, error);
        if (r == 0)
            return true;
        if (r == Fail)
            return false;
        const Wait wr = waitFd(_fd, _want, w);
        if (wr != Wait::Ready) {
            *error = waitError(wr);
            return false;
        }
    }
}

long sockSend(int fd, const void *buf, size_t n, int *err) {
    for (;;) {
#ifdef MSG_NOSIGNAL
        const ssize_t r = ::send(fd, buf, n, MSG_NOSIGNAL); // a closed peer must not SIGPIPE us
#else
        const ssize_t r = ::send(fd, buf, n, 0);
#endif
        if (r >= 0)
            return long(r);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Stream::Again;
        *err = errno;
        return Stream::Fail;
    }
}

long sockRecv(int fd, void *buf, size_t n, int *err) {
    for (;;) {
        const ssize_t r = ::recv(fd, buf, n, 0);
        if (r >= 0)
            return long(r);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return Stream::Again;
        *err = errno;
        return Stream::Fail;
    }
}

long Stream::tryRead(char *buf, size_t n) {
    if (_tls)
        return tlsRead(_tls, buf, n, &_want, &_error);
    int        err = 0;
    const long r   = sockRecv(_fd, buf, n, &err);
    if (r == Again)
        _want = POLLIN;
    else if (r == Fail)
        _error = sysError("connect", err);
    return r;
}

long Stream::tryWrite(const char *buf, size_t n) {
    if (_tls)
        return tlsWrite(_tls, buf, n, &_want, &_error);
    int        err = 0;
    const long r   = sockSend(_fd, buf, n, &err);
    if (r == Again)
        _want = POLLOUT;
    else if (r == Fail)
        _error = sysError("connect", err);
    return r;
}

bool Stream::pending() const {
    return _tls && tlsPending(_tls);
}

long Stream::read(char *buf, size_t n, const Waiter &w, std::string *error) {
    for (;;) {
        // The cancel flag and the deadline are checked on every call, also
        // when data is waiting; a poll only when a read would block.
        if (w.cancel && w.cancel->load(std::memory_order_relaxed)) {
            *error = waitError(Wait::Cancelled);
            return -1;
        }
        if (w.deadline && nowMs() >= w.deadline) {
            *error = waitError(Wait::Timeout);
            return -1;
        }
        const long got = tryRead(buf, n);
        if (got >= 0)
            return got;
        if (got == Fail) {
            *error = _error;
            return -1;
        }
        // POLLOUT when TLS wants to write first (rare).
        const Wait r = waitFd(_fd, _want == POLLOUT ? POLLOUT : POLLIN, w);
        if (r != Wait::Ready) {
            *error = waitError(r);
            return -1;
        }
    }
}

bool Stream::writeAll(std::string_view data, const Waiter &w, std::string *error) {
    size_t off = 0;
    while (off < data.size()) {
        const long r = tryWrite(data.data() + off, data.size() - off);
        if (r > 0) {
            off += size_t(r);
            continue;
        }
        if (r == Fail || r == 0) {
            *error = r == 0 ? "connect: closed" : _error;
            return false;
        }
        const Wait wr = waitFd(_fd, _want, w);
        if (wr != Wait::Ready) {
            *error = waitError(wr);
            return false;
        }
    }
    return true;
}

bool Stream::idleDead() {
    pollfd p{_fd, POLLIN, 0};
    if (poll(&p, 1, 0) == 0)
        return false;
    // Readable while idle: the peer closed, or (TLS 1.3) sent a session
    // ticket, which TLS swallows and reports as "nothing for you".
    char       c;
    const long r = tryRead(&c, 1);
    return r != Again;
}

} // namespace net::detail
