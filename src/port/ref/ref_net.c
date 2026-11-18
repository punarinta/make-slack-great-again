/* port_net.h over POSIX sockets: HTTP/1.1 and WebSocket clients on plain
 * TCP. No TLS — https:// and wss:// answer PORT_E_UNSUPPORTED (and the
 * capability bit is clear), which is all the test suites need from a
 * reference port; a real port puts its OS's TLS under the same calls.
 *
 * Every wait is a poll on the socket plus the cancel's pipe, bounded by the
 * request's deadline, so a cancel or an abort ends a blocked exchange at
 * once and an idle WebSocket reader sleeps without periodic wake-ups. */
#define _GNU_SOURCE /* pipe2, MSG_NOSIGNAL, SOCK_CLOEXEC */
#include "ref_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

enum { kMaxHead = 64 * 1024, kIdleMs = 30000, kMaxIdle = 16, kCloseWaitMs = 3000 };

static int64_t nowMs(void) {
    return port_time_monotonic_ns() / 1000000;
}

/* ── Cancel ────────────────────────────────────────────────────────────── */

struct port_cancel {
    atomic_int flag;
    int        pipe[2]; /* written once by set(): wakes a poll at once */
};

int port_cancel_create(port_cancel **out) {
    struct port_cancel *c = (struct port_cancel *)calloc(1, sizeof *c);
    if (!c)
        return PORT_E_NOMEM;
    atomic_init(&c->flag, 0);
    if (pipe2(c->pipe, O_CLOEXEC | O_NONBLOCK) != 0)
        c->pipe[0] = c->pipe[1] = -1; /* waits then poll the flag every 250 ms */
    *out = c;
    return PORT_OK;
}

void port_cancel_set(port_cancel *c) {
    if (atomic_exchange(&c->flag, 1) == 0 && c->pipe[1] >= 0) {
        const char b  = 1;
        ssize_t    rc = write(c->pipe[1], &b, 1);
        (void)rc;
    }
}

int port_cancel_is_set(const port_cancel *c) {
    return c && atomic_load(&((port_cancel *)c)->flag);
}

void port_cancel_destroy(port_cancel *c) {
    if (!c)
        return;
    if (c->pipe[0] >= 0) {
        close(c->pipe[0]);
        close(c->pipe[1]);
    }
    free(c);
}

/* ── Waiting ───────────────────────────────────────────────────────────── */

typedef struct waiter {
    int64_t      deadline; /* nowMs(); 0 = none */
    port_cancel *cancel;
    int          wake_fd; /* also wakes the wait (a WebSocket abort); -1 none */
    atomic_int  *aborted; /* checked with the cancel */
} waiter;

static int stopped(const waiter *w) {
    return (w->cancel && atomic_load(&w->cancel->flag)) || (w->aborted && atomic_load(w->aborted));
}

/* PORT_OK when fd is ready for `events`, else TIMEOUT / CANCELLED. */
static int waitFd(int fd, short events, const waiter *w) {
    for (;;) {
        if (stopped(w))
            return PORT_E_CANCELLED;
        struct pollfd p[3];
        int           n = 0;
        p[n++]          = (struct pollfd){fd, events, 0};
        if (w->cancel && w->cancel->pipe[0] >= 0)
            p[n++] = (struct pollfd){w->cancel->pipe[0], POLLIN, 0};
        if (w->wake_fd >= 0)
            p[n++] = (struct pollfd){w->wake_fd, POLLIN, 0};
        int timeout = -1;
        if (w->deadline) {
            const int64_t left = w->deadline - nowMs();
            if (left <= 0)
                return PORT_E_TIMEOUT;
            timeout = left > 1000000 ? 1000000 : (int)left;
        }
        /* Without a pipe to wake on, look at the flag four times a second. */
        if (w->cancel && w->cancel->pipe[0] < 0 && (timeout < 0 || timeout > 250))
            timeout = 250;
        const int r = poll(p, (nfds_t)n, timeout);
        if (r < 0 && errno != EINTR)
            return PORT_E_CONNECT;
        if (r > 0 && p[0].revents)
            return PORT_OK;
        /* A wake or a cancel: loop to re-check the flags (wake pipes are
         * level-triggered: whoever wrote them also set a flag). */
        if (r > 0 && w->wake_fd >= 0 && p[n - 1].revents) {
            char    drain[64];
            ssize_t got;
            do /* the pipe is non-blocking: empty it, or every poll would return */
                got = read(w->wake_fd, drain, sizeof drain);
            while (got > 0);
            if (!stopped(w))
                return PORT_E_AGAIN;
        }
    }
}

/* ── Connections ───────────────────────────────────────────────────────── */

typedef struct url {
    char host[256];
    int  port;
    int  secure;
    char target[8192];
    char authority[300];
} url;

static int parseUrl(const char *s, url *u, int ws) {
    memset(u, 0, sizeof *u);
    const char *rest;
    if (!strncmp(s, ws ? "ws://" : "http://", ws ? 5 : 7))
        rest = s + (ws ? 5 : 7);
    else if (!strncmp(s, ws ? "wss://" : "https://", ws ? 6 : 8))
        rest = s + (ws ? 6 : 8), u->secure = 1;
    else
        return PORT_E_INVAL;
    const char  *slash = strpbrk(rest, "/?");
    const size_t alen  = slash ? (size_t)(slash - rest) : strlen(rest);
    if (!alen || alen >= sizeof u->authority)
        return PORT_E_INVAL;
    memcpy(u->authority, rest, alen);
    const char *h = u->authority, *hend, *colon;
    if (*h == '[') { /* an IPv6 literal */
        hend = strchr(h, ']');
        if (!hend)
            return PORT_E_INVAL;
        ++h;
        colon = hend[1] == ':' ? hend + 1 : NULL;
    } else {
        colon = strrchr(h, ':');
        hend  = colon ? colon : h + strlen(h);
    }
    if ((size_t)(hend - h) >= sizeof u->host)
        return PORT_E_INVAL;
    memcpy(u->host, h, (size_t)(hend - h));
    u->port = colon ? atoi(colon + 1) : u->secure ? 443 : 80;
    snprintf(
        u->target, sizeof u->target, "%s%s", slash && *slash == '?' ? "/" : "", slash ? slash : "/"
    );
    return u->port > 0 && u->port < 65536 ? PORT_OK : PORT_E_INVAL;
}

static int openSocket(const url *u, const waiter *w, char *detail, size_t cap) {
    char port[8];
    snprintf(port, sizeof port, "%d", u->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;
    const int g       = getaddrinfo(u->host, port, &hints, &res);
    if (g != 0) {
        ref_detail(detail, cap, gai_strerror(g));
        return PORT_E_DNS;
    }
    int rc = PORT_E_CONNECT, fd = -1;
    ref_detail(detail, cap, "unreachable");
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {
            ref_detail(detail, cap, errno == ECONNREFUSED ? "refused" : "failed");
            close(fd);
            fd = -1;
            continue;
        }
        rc            = waitFd(fd, POLLOUT, w);
        int       err = 0;
        socklen_t len = sizeof err;
        if (rc == PORT_OK && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err)) {
            ref_detail(detail, cap, err == ECONNREFUSED ? "refused" : "failed");
            rc = PORT_E_CONNECT;
        }
        if (rc != PORT_OK) {
            close(fd);
            fd = -1;
            if (rc == PORT_E_TIMEOUT || rc == PORT_E_CANCELLED)
                break;
        }
    }
    freeaddrinfo(res);
    if (fd < 0)
        return rc == PORT_OK ? PORT_E_CONNECT : rc;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int writeAll(int fd, const char *p, size_t n, const waiter *w) {
    while (n > 0) {
        const ssize_t s = send(fd, p, n, MSG_NOSIGNAL);
        if (s > 0) {
            p += s;
            n -= (size_t)s;
            continue;
        }
        if (s < 0 && errno == EINTR)
            continue;
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc;
            while ((rc = waitFd(fd, POLLOUT, w)) == PORT_E_AGAIN) {
            }
            if (rc != PORT_OK)
                return rc;
            continue;
        }
        return PORT_E_CONNECT;
    }
    return PORT_OK;
}

/* > 0 bytes, 0 at the end, or an error. */
static int64_t readSome(int fd, char *buf, size_t n, const waiter *w) {
    for (;;) {
        const ssize_t r = recv(fd, buf, n, 0);
        if (r >= 0)
            return r;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return PORT_E_CONNECT;
        /* A wake (PORT_E_AGAIN) goes back to the caller to re-check its state. */
        const int rc = waitFd(fd, POLLIN, w);
        if (rc != PORT_OK)
            return rc;
    }
}

/* A growable byte buffer with a read position. */
typedef struct buffer {
    char  *data;
    size_t len, cap, pos;
} buffer;

static int bufReserve(buffer *b, size_t more) {
    if (b->len + more <= b->cap)
        return PORT_OK;
    size_t cap = b->cap ? b->cap : 16384;
    while (cap < b->len + more)
        cap *= 2;
    char *d = (char *)realloc(b->data, cap);
    if (!d)
        return PORT_E_NOMEM;
    b->data = d;
    b->cap  = cap;
    return PORT_OK;
}

static int bufAppend(buffer *b, const char *p, size_t n) {
    const int rc = bufReserve(b, n);
    if (rc == PORT_OK && n) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
    return rc;
}

/* Reads more into b: > 0, 0 at the end, or an error. */
static int64_t fill(int fd, buffer *b, const waiter *w) {
    if (bufReserve(b, 16384) != PORT_OK)
        return PORT_E_NOMEM;
    const int64_t r = readSome(fd, b->data + b->len, b->cap - b->len, w);
    if (r > 0)
        b->len += (size_t)r;
    return r;
}

/* ── Kept-alive connections ────────────────────────────────────────────── */

typedef struct idle_conn {
    char    key[300];
    int     fd;
    int64_t since;
} idle_conn;
static pthread_mutex_t g_poolMutex = PTHREAD_MUTEX_INITIALIZER;
static idle_conn       g_pool[kMaxIdle];
static int             g_poolCount;

/* True when the peer closed (or sent something unasked) while it sat idle. */
static int idleDead(int fd) {
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, 0) != 0;
}

static int takeIdle(const char *key) {
    int fd = -1;
    pthread_mutex_lock(&g_poolMutex);
    for (int i = g_poolCount - 1; i >= 0 && fd < 0; --i) {
        if (strcmp(g_pool[i].key, key) != 0)
            continue;
        const int     cand = g_pool[i].fd;
        const int64_t age  = nowMs() - g_pool[i].since;
        g_pool[i]          = g_pool[--g_poolCount];
        if (age < kIdleMs && !idleDead(cand))
            fd = cand;
        else
            close(cand);
    }
    pthread_mutex_unlock(&g_poolMutex);
    return fd;
}

static void putIdle(const char *key, int fd) {
    pthread_mutex_lock(&g_poolMutex);
    if (g_poolCount == kMaxIdle) {
        close(g_pool[0].fd);
        g_pool[0] = g_pool[--g_poolCount];
    }
    idle_conn *c = &g_pool[g_poolCount++];
    snprintf(c->key, sizeof c->key, "%s", key);
    c->fd    = fd;
    c->since = nowMs();
    pthread_mutex_unlock(&g_poolMutex);
}

void port_net_release(void) {
    pthread_mutex_lock(&g_poolMutex);
    for (int i = 0; i < g_poolCount; ++i)
        close(g_pool[i].fd);
    g_poolCount = 0;
    pthread_mutex_unlock(&g_poolMutex);
}

int port_net_loopback_port(void) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len     = sizeof a;
    int       port    = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

/* ── Response heads ────────────────────────────────────────────────────── */

enum { kMaxHeaders = 128 };
typedef struct head {
    int         status, minor;
    port_header h[kMaxHeaders];
    size_t      count;
    size_t      len; /* bytes of the head, the blank line included */
} head;

static int ieq(const char *a, size_t n, const char *b) {
    return strlen(b) == n && strncasecmp(a, b, n) == 0;
}

static const port_header *findHeader(const head *h, const char *name) {
    for (size_t i = 0; i < h->count; ++i)
        if (ieq(h->h[i].name, h->h[i].name_len, name))
            return &h->h[i];
    return NULL;
}

static int hasToken(const port_header *h, const char *token) {
    const size_t t = strlen(token);
    for (size_t i = 0; h && i + t <= h->value_len; ++i)
        if (strncasecmp(h->value + i, token, t) == 0)
            return 1;
    return 0;
}

/* Parses "HTTP/1.x NNN ...\r\n" + header lines in place (the pointers point
 * into the buffer). */
static int parseHead(char *text, size_t len, head *h) {
    if (len < 12 || strncmp(text, "HTTP/1.", 7) != 0 || text[8] != ' ')
        return PORT_E_PROTOCOL;
    h->minor  = text[7] - '0';
    h->status = atoi(text + 9);
    h->count  = 0;
    if (h->status < 100 || h->status > 999)
        return PORT_E_PROTOCOL;
    char *line = strstr(text, "\r\n");
    char *end  = text + len;
    while (line && line + 2 < end) {
        line += 2;
        char *eol = strstr(line, "\r\n");
        if (!eol || eol == line)
            break;
        if (*line == ' ' || *line == '\t') { /* an obsolete folded line: join on */
            if (h->count)
                h->h[h->count - 1].value_len = (size_t)(eol - h->h[h->count - 1].value);
            line = eol;
            continue;
        }
        char *colon = memchr(line, ':', (size_t)(eol - line));
        if (!colon || h->count == kMaxHeaders)
            return PORT_E_PROTOCOL;
        char *v = colon + 1, *ve = eol;
        while (v < ve && (*v == ' ' || *v == '\t'))
            ++v;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            --ve;
        h->h[h->count++] = (port_header){line, (size_t)(colon - line), v, (size_t)(ve - v)};
        line             = eol;
    }
    return PORT_OK;
}

/* Reads until a whole head is in b and parses it. *got: a byte arrived. */
static int readHead(int fd, buffer *b, const waiter *w, head *h, int *got) {
    size_t scanned = b->pos;
    for (;;) {
        for (size_t i = scanned > b->pos + 3 ? scanned - 3 : b->pos; i + 4 <= b->len; ++i)
            if (!memcmp(b->data + i, "\r\n\r\n", 4)) {
                b->data[i + 2] = 0; /* NUL-terminate the head for parsing */
                h->len         = i + 4 - b->pos;
                return parseHead(b->data + b->pos, i + 2 - b->pos, h);
            }
        if (b->len - b->pos > kMaxHead)
            return PORT_E_PROTOCOL;
        scanned         = b->len;
        const int64_t r = fill(fd, b, w);
        if (r < 0)
            return (int)r;
        if (r == 0)
            return b->len == b->pos ? PORT_E_CLOSED : PORT_E_PROTOCOL;
        *got = 1;
    }
}

/* ── HTTP ──────────────────────────────────────────────────────────────── */

typedef struct body_reader {
    int                      fd;
    buffer                  *b;
    const waiter            *w;
    const port_http_handler *h;
} body_reader;

static int deliver(body_reader *r, const char *p, size_t n) {
    if (n && r->h->on_body && r->h->on_body(r->h->user, p, n) != 0)
        return PORT_E_ABORTED;
    return PORT_OK;
}

/* n bytes of body (or until the end with n < 0). */
static int takeBody(body_reader *r, int64_t n) {
    for (;;) {
        size_t have = r->b->len - r->b->pos;
        if (n >= 0 && (int64_t)have > n)
            have = (size_t)n;
        if (have) {
            const int rc = deliver(r, r->b->data + r->b->pos, have);
            if (rc != PORT_OK)
                return rc;
            r->b->pos += have;
            if (n >= 0)
                n -= (int64_t)have;
        }
        if (n == 0)
            return PORT_OK;
        r->b->len = r->b->pos = 0; /* everything taken: reuse the space */
        const int64_t got     = fill(r->fd, r->b, r->w);
        if (got == 0)
            return n < 0 ? PORT_OK : PORT_E_PROTOCOL;
        if (got < 0)
            return (int)got;
    }
}

/* One CRLF-terminated line from the buffer (for chunk sizes, trailers). */
static int takeLine(body_reader *r, char *out, size_t cap) {
    for (;;) {
        for (size_t i = r->b->pos; i + 1 < r->b->len; ++i)
            if (r->b->data[i] == '\r' && r->b->data[i + 1] == '\n') {
                const size_t n = i - r->b->pos;
                if (n >= cap)
                    return PORT_E_PROTOCOL;
                memcpy(out, r->b->data + r->b->pos, n);
                out[n]    = 0;
                r->b->pos = i + 2;
                return PORT_OK;
            }
        if (r->b->len - r->b->pos > 4096)
            return PORT_E_PROTOCOL;
        const int64_t got = fill(r->fd, r->b, r->w);
        if (got <= 0)
            return got == 0 ? PORT_E_PROTOCOL : (int)got;
    }
}

static int takeChunked(body_reader *r) {
    char line[4096];
    for (;;) {
        int rc = takeLine(r, line, sizeof line);
        if (rc != PORT_OK)
            return rc;
        char                    *end = NULL;
        const unsigned long long n   = strtoull(line, &end, 16);
        if (end == line)
            return PORT_E_PROTOCOL;
        if (n == 0) {
            do /* trailers, up to the blank line */
                rc = takeLine(r, line, sizeof line);
            while (rc == PORT_OK && line[0]);
            return rc;
        }
        if ((rc = takeBody(r, (int64_t)n)) != PORT_OK ||
            (rc = takeLine(r, line, sizeof line)) != PORT_OK)
            return rc;
        if (line[0])
            return PORT_E_PROTOCOL;
    }
}

/* What one attempt on a connection came to. */
enum { DONE, KEPT, UNSENT, RETRY, FAILED };

static int exchange(
    int                      fd,
    const char              *headText,
    size_t                   headLen,
    const port_http_request *req,
    const port_http_handler *h,
    const waiter            *w,
    int                     *err,
    char                    *detail,
    size_t                   cap
) {
    int rc = writeAll(fd, headText, headLen, w);
    if (rc == PORT_OK && req->body_len)
        rc = writeAll(fd, (const char *)req->body, req->body_len, w);
    if (rc != PORT_OK) {
        *err = rc;
        return rc == PORT_E_TIMEOUT || rc == PORT_E_CANCELLED ? FAILED : UNSENT;
    }
    buffer b;
    memset(&b, 0, sizeof b);
    head hd;
    for (;;) {
        int got = 0;
        rc      = readHead(fd, &b, w, &hd, &got);
        if (rc != PORT_OK) {
            free(b.data);
            *err = rc == PORT_E_CLOSED ? PORT_E_CONNECT : rc;
            ref_detail(detail, cap, rc == PORT_E_CLOSED ? "closed" : "bad response head");
            return !got && rc != PORT_E_TIMEOUT && rc != PORT_E_CANCELLED ? RETRY : FAILED;
        }
        if (hd.status >= 200 || hd.status == 101)
            break;
        b.pos += hd.len; /* 100 Continue, 103 Early Hints: not the answer */
    }
    b.pos += hd.len;
    int keep = hd.minor >= 1 && !hasToken(findHeader(&hd, "connection"), "close");
    if (h->on_head && h->on_head(h->user, hd.status, hd.h, hd.count) != 0) {
        free(b.data);
        *err = PORT_E_ABORTED;
        return FAILED;
    }
    body_reader r = {fd, &b, w, h};
    const int   bodyless =
        !strcmp(req->method, "HEAD") || hd.status == 204 || hd.status == 304 || hd.status < 200;
    const port_header *te = findHeader(&hd, "transfer-encoding");
    const port_header *cl = findHeader(&hd, "content-length");
    if (bodyless) {
        rc = PORT_OK;
    } else if (te) {
        if (!hasToken(te, "chunked")) {
            ref_detail(detail, cap, "unsupported transfer-encoding");
            rc = PORT_E_PROTOCOL;
        } else {
            rc = takeChunked(&r);
        }
    } else if (cl) {
        char num[32];
        if (cl->value_len == 0 || cl->value_len >= sizeof num) {
            rc = PORT_E_PROTOCOL;
        } else {
            memcpy(num, cl->value, cl->value_len);
            num[cl->value_len]  = 0;
            char           *end = NULL;
            const long long n   = strtoll(num, &end, 10);
            rc                  = (*end || n < 0) ? PORT_E_PROTOCOL : takeBody(&r, n);
        }
        if (rc == PORT_E_PROTOCOL)
            ref_detail(detail, cap, "bad content-length");
    } else {
        keep = 0;
        rc   = takeBody(&r, -1); /* until the server closes */
    }
    /* Bytes beyond the answer would answer nothing: don't reuse. */
    keep = keep && rc == PORT_OK && b.pos == b.len;
    free(b.data);
    if (rc != PORT_OK) {
        *err = rc;
        return FAILED;
    }
    return keep ? KEPT : DONE;
}

static int hasHeader(const port_http_request *req, const char *name) {
    for (size_t i = 0; i < req->header_count; ++i)
        if (ieq(req->headers[i].name, req->headers[i].name_len, name))
            return 1;
    return 0;
}

static int buildHead(const port_http_request *req, const url *u, buffer *out) {
    char line[9000];
    int  rc = PORT_OK;
#define PUT(...)                                                                                   \
    do {                                                                                           \
        const int n_ = snprintf(line, sizeof line, __VA_ARGS__);                                   \
        if (rc == PORT_OK)                                                                         \
            rc = n_ < 0 || (size_t)n_ >= sizeof line ? PORT_E_RANGE                                \
                                                     : bufAppend(out, line, (size_t)n_);           \
    } while (0)
    PUT("%s %s HTTP/1.1\r\n", req->method, u->target);
    if (!hasHeader(req, "host"))
        PUT("Host: %s\r\n", u->authority);
    if (!hasHeader(req, "connection"))
        PUT("Connection: keep-alive\r\n");
    /* Servers answer 411 to a body-less POST without a length. */
    if (!hasHeader(req, "content-length") &&
        (req->body_len || !strcmp(req->method, "POST") || !strcmp(req->method, "PUT") ||
         !strcmp(req->method, "PATCH")))
        PUT("Content-Length: %zu\r\n", req->body_len);
#undef PUT
    for (size_t i = 0; i < req->header_count && rc == PORT_OK; ++i) {
        const port_header *hd = &req->headers[i];
        rc                    = bufAppend(out, hd->name, hd->name_len);
        if (rc == PORT_OK)
            rc = bufAppend(out, ": ", 2);
        if (rc == PORT_OK)
            rc = bufAppend(out, hd->value, hd->value_len);
        if (rc == PORT_OK)
            rc = bufAppend(out, "\r\n", 2);
    }
    return rc == PORT_OK ? bufAppend(out, "\r\n", 2) : rc;
}

int port_http_perform(
    const port_http_request *req, const port_http_handler *h, char *detail, size_t cap
) {
    ref_detail(detail, cap, "");
    url u;
    if (parseUrl(req->url, &u, 0) != PORT_OK)
        return PORT_E_INVAL;
    if (u.secure) {
        ref_detail(detail, cap, "no TLS in the reference port");
        return PORT_E_UNSUPPORTED;
    }
    waiter w = {req->timeout_ms > 0 ? nowMs() + req->timeout_ms : 0, req->cancel, -1, NULL};
    char   key[300];
    snprintf(key, sizeof key, "%s:%d", u.host, u.port);
    buffer headText;
    memset(&headText, 0, sizeof headText);
    int err = buildHead(req, &u, &headText);
    if (err != PORT_OK) {
        free(headText.data);
        return err;
    }
    const int idempotent = !strcmp(req->method, "GET") || !strcmp(req->method, "HEAD");
    /* A kept-alive connection the server has meanwhile dropped fails before
     * a byte of the answer: then once more on a fresh one. */
    for (int attempt = 0; attempt < 2; ++attempt) {
        int       fd     = attempt == 0 ? takeIdle(key) : -1;
        const int reused = fd >= 0;
        if (fd < 0) {
            fd = openSocket(&u, &w, detail, cap);
            if (fd < 0) {
                err = fd;
                break;
            }
        }
        err         = PORT_OK;
        const int o = exchange(fd, headText.data, headText.len, req, h, &w, &err, detail, cap);
        if (reused && (o == UNSENT || (o == RETRY && idempotent))) {
            close(fd);
            continue;
        }
        if (o == KEPT && !port_cancel_is_set(req->cancel))
            putIdle(key, fd);
        else
            close(fd);
        if (o == DONE || o == KEPT)
            err = PORT_OK;
        break;
    }
    free(headText.data);
    if (err != PORT_OK && port_cancel_is_set(req->cancel))
        err = PORT_E_CANCELLED;
    return err;
}

/* ── WebSocket ─────────────────────────────────────────────────────────── */

struct port_ws {
    int             fd;
    int             wake[2]; /* abort() writes it: a blocked recv returns at once */
    atomic_int      aborted;
    pthread_mutex_t write_mutex; /* sends from several threads stay whole frames */
    buffer          in;
    buffer          message; /* a fragmented message so far */
    int             message_kind;
    int             close_sent, closed;
    int64_t         close_deadline;
};

static int sendFrame(port_ws *ws, int op, const void *data, size_t n) {
    unsigned char hdr[14];
    size_t        k = 0;
    hdr[k++]        = (unsigned char)(0x80 | op);
    if (n < 126) {
        hdr[k++] = (unsigned char)(0x80 | n);
    } else if (n < 65536) {
        hdr[k++] = 0x80 | 126;
        hdr[k++] = (unsigned char)(n >> 8);
        hdr[k++] = (unsigned char)n;
    } else {
        hdr[k++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i)
            hdr[k++] = (unsigned char)((uint64_t)n >> (8 * i));
    }
    unsigned char mask[4];
    port_random(mask, sizeof mask);
    memcpy(hdr + k, mask, 4);
    k += 4;
    char *frame = (char *)malloc(k + n);
    if (!frame)
        return PORT_E_NOMEM;
    memcpy(frame, hdr, k);
    for (size_t i = 0; i < n; ++i)
        frame[k + i] = (char)(((const unsigned char *)data)[i] ^ mask[i & 3]);
    /* Bounded, so a peer that stops reading can't hang a sender for good. */
    const waiter w = {nowMs() + 30000, NULL, -1, &ws->aborted};
    pthread_mutex_lock(&ws->write_mutex);
    const int rc = writeAll(ws->fd, frame, k + n, &w);
    pthread_mutex_unlock(&ws->write_mutex);
    free(frame);
    return rc;
}

int port_ws_connect(
    const char        *urlText,
    const port_header *headers,
    size_t             header_count,
    int32_t            timeout_ms,
    port_cancel       *cancel,
    port_ws          **out,
    char              *detail,
    size_t             cap
) {
    ref_detail(detail, cap, "");
    url u;
    if (parseUrl(urlText, &u, 1) != PORT_OK)
        return PORT_E_INVAL;
    if (u.secure) {
        ref_detail(detail, cap, "no TLS in the reference port");
        return PORT_E_UNSUPPORTED;
    }
    port_ws *ws = (port_ws *)calloc(1, sizeof *ws);
    if (!ws || pipe2(ws->wake, O_CLOEXEC | O_NONBLOCK) != 0) {
        free(ws);
        return PORT_E_NOMEM;
    }
    atomic_init(&ws->aborted, 0);
    pthread_mutex_init(&ws->write_mutex, NULL);
    waiter w = {timeout_ms > 0 ? nowMs() + timeout_ms : 0, cancel, ws->wake[0], &ws->aborted};
    ws->fd   = openSocket(&u, &w, detail, cap);
    int rc   = ws->fd < 0 ? ws->fd : PORT_OK;

    if (rc == PORT_OK) {
        unsigned char nonce[16];
        port_random(nonce, sizeof nonce);
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char key[25];
        for (int i = 0, k = 0; i < 16; i += 3) {
            const uint32_t v = (uint32_t)nonce[i] << 16 |
                               (i + 1 < 16 ? (uint32_t)nonce[i + 1] << 8 : 0) |
                               (i + 2 < 16 ? nonce[i + 2] : 0);
            key[k++]         = b64[v >> 18 & 63];
            key[k++]         = b64[v >> 12 & 63];
            key[k++]         = i + 1 < 16 ? b64[v >> 6 & 63] : '=';
            key[k++]         = i + 2 < 16 ? b64[v & 63] : '=';
        }
        key[24] = 0;
        buffer req;
        memset(&req, 0, sizeof req);
        char line[9000];
        snprintf(
            line,
            sizeof line,
            "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n",
            u.target,
            u.authority,
            key
        );
        rc = bufAppend(&req, line, strlen(line));
        for (size_t i = 0; i < header_count && rc == PORT_OK; ++i) {
            bufAppend(&req, headers[i].name, headers[i].name_len);
            bufAppend(&req, ": ", 2);
            bufAppend(&req, headers[i].value, headers[i].value_len);
            rc = bufAppend(&req, "\r\n", 2);
        }
        if (rc == PORT_OK)
            rc = bufAppend(&req, "\r\n", 2);
        if (rc == PORT_OK)
            rc = writeAll(ws->fd, req.data, req.len, &w);
        free(req.data);
        head hd;
        int  got = 0;
        if (rc == PORT_OK)
            rc = readHead(ws->fd, &ws->in, &w, &hd, &got);
        if (rc == PORT_OK && hd.status != 101) {
            char why[32];
            snprintf(why, sizeof why, "status %d", hd.status);
            ref_detail(detail, cap, why);
            rc = PORT_E_PROTOCOL;
        }
        /* The reference port does not check Sec-WebSocket-Accept (it would
         * need SHA-1); a real port's stack does. */
        if (rc == PORT_OK)
            ws->in.pos += hd.len;
        else if (rc == PORT_E_CLOSED)
            rc = PORT_E_PROTOCOL;
    }
    if (rc != PORT_OK) {
        if (port_cancel_is_set(cancel))
            rc = PORT_E_CANCELLED;
        if (ws->fd >= 0)
            close(ws->fd);
        close(ws->wake[0]);
        close(ws->wake[1]);
        pthread_mutex_destroy(&ws->write_mutex);
        free(ws->in.data);
        free(ws);
        return rc;
    }
    *out = ws;
    return PORT_OK;
}

static int finish(port_ws *ws, port_ws_message *m, int code, const char *reason, size_t n) {
    ws->closed    = 1;
    m->kind       = PORT_WS_CLOSED;
    m->close_code = code;
    m->data       = ref_strndup(reason, n);
    m->len        = n;
    return PORT_OK;
}

int port_ws_recv(port_ws *ws, port_ws_message *out) {
    port_ws_message m;
    memset(&m, 0, sizeof m);
    m.size = sizeof m;
    if (ws->closed)
        return PORT_E_CLOSED;
    for (;;) {
        if (atomic_load(&ws->aborted))
            break;
        /* Whole frames from what has arrived. */
        buffer              *b  = &ws->in;
        const unsigned char *p  = (const unsigned char *)b->data + b->pos;
        const size_t         av = b->len - b->pos;
        if (av >= 2) {
            const int fin = p[0] & 0x80, op = p[0] & 0x0f, masked = p[1] & 0x80;
            uint64_t  n = p[1] & 0x7f;
            size_t    k = 2;
            if (n == 126 && av >= 4)
                n = (uint64_t)p[2] << 8 | p[3], k = 4;
            else if (n == 127 && av >= 10) {
                n = 0;
                for (int i = 0; i < 8; ++i)
                    n = n << 8 | p[2 + i];
                k = 10;
            } else if (n >= 126) {
                goto more;
            }
            if (masked)
                k += 4;
            if (n > 256u * 1024 * 1024) {
                *out = m;
                return finish(ws, out, 1009, "too big", 7);
            }
            if (av >= k + n) {
                const char *payload = (const char *)p + k;
                b->pos += k + (size_t)n;
                if (op == 0x8) { /* close: answer it (unless we started) and end */
                    const int code =
                        n >= 2 ? (unsigned char)payload[0] << 8 | (unsigned char)payload[1] : 1005;
                    if (!ws->close_sent)
                        sendFrame(ws, 0x8, payload, n >= 2 ? 2 : 0);
                    *out = m;
                    return finish(ws, out, code, n > 2 ? payload + 2 : "", n > 2 ? n - 2 : 0);
                }
                if (op == 0x9) { /* ping → pong */
                    sendFrame(ws, 0xA, payload, (size_t)n);
                    continue;
                }
                if (op == 0xA)
                    continue;
                if (op == 0x1 || op == 0x2)
                    ws->message_kind = op == 0x1 ? PORT_WS_TEXT : PORT_WS_BINARY,
                    ws->message.len  = 0;
                if (bufAppend(&ws->message, payload, (size_t)n) != PORT_OK) {
                    *out = m;
                    return finish(ws, out, 1009, "", 0);
                }
                if (!fin)
                    continue;
                m.kind = (uint32_t)ws->message_kind;
                m.data = ref_strndup(ws->message.data ? ws->message.data : "", ws->message.len);
                m.len  = ws->message.len;
                ws->message.len = 0;
                *out            = m;
                return PORT_OK;
            }
        }
    more:
        if (b->pos == b->len)
            b->pos = b->len = 0;
        if (ws->close_sent && nowMs() >= ws->close_deadline)
            break;
        waiter w = {ws->close_sent ? ws->close_deadline : 0, NULL, ws->wake[0], &ws->aborted};
        const int64_t r = fill(ws->fd, b, &w);
        if (r == PORT_E_AGAIN || r == PORT_E_TIMEOUT)
            continue;
        if (r <= 0)
            break; /* gone without a close frame */
    }
    *out = m;
    return finish(ws, out, 1006, "", 0);
}

void port_ws_message_free(port_ws_message *m) {
    free(m->data);
    m->data = NULL;
    m->len  = 0;
}

int port_ws_send(port_ws *ws, const void *data, size_t n, int text) {
    return ws->close_sent ? PORT_E_CLOSED : sendFrame(ws, text ? 0x1 : 0x2, data, n);
}

int port_ws_ping(port_ws *ws, const void *data, size_t n) {
    return n > 125 ? PORT_E_INVAL : sendFrame(ws, 0x9, data, n);
}

int port_ws_close(port_ws *ws, int code) {
    if (ws->close_sent)
        return PORT_OK;
    const unsigned char payload[2] = {(unsigned char)(code >> 8), (unsigned char)code};
    ws->close_deadline             = nowMs() + kCloseWaitMs;
    ws->close_sent                 = 1;
    const int  rc                  = sendFrame(ws, 0x8, payload, 2);
    const char b                   = 1; /* the reader re-arms its wait with the deadline */
    ssize_t    wr                  = write(ws->wake[1], &b, 1);
    (void)wr;
    return rc;
}

void port_ws_abort(port_ws *ws) {
    atomic_store(&ws->aborted, 1);
    const char b  = 1;
    ssize_t    rc = write(ws->wake[1], &b, 1);
    (void)rc;
    shutdown(ws->fd, SHUT_RDWR);
}

void port_ws_destroy(port_ws *ws) {
    if (!ws)
        return;
    close(ws->fd);
    close(ws->wake[0]);
    close(ws->wake[1]);
    pthread_mutex_destroy(&ws->write_mutex);
    free(ws->in.data);
    free(ws->message.data);
    free(ws);
}
