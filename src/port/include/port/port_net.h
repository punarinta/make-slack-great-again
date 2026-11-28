/* port_net.h — HTTP/1.1 exchanges and WebSockets, blocking, on the core's
 * worker threads.
 *
 * The core (net/client.cpp, net/websocket.cpp) owns the threads, queues,
 * redirects, cookies, decompression policy and delivery to the UI thread; a
 * port only does one blocking exchange at a time per calling thread. Name
 * resolution, TCP, TLS (with certificate and host name checks against the
 * OS's trust store) and socket readiness all live inside the port: the core
 * never sees a file descriptor, so a port is free to use its own event
 * mechanism (PEOS: netconn / http_client).
 *
 * Any thread may call any function here; one port_ws may be used by its
 * reader thread (recv) and any other thread (send, ping, close, abort) at
 * once — the port serialises writes. */
#ifndef PORT_PORT_NET_H
#define PORT_PORT_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A cancel flag a blocked exchange watches. Created with the request on the
 * UI thread, set from any thread; a set flag makes the exchange using it
 * return PORT_E_CANCELLED promptly (within ~250 ms at worst: a port without
 * a way to interrupt its wait may poll it at that period). */
typedef struct port_cancel port_cancel;
int                        port_cancel_create(port_cancel **out);
void                       port_cancel_set(port_cancel *c);
int                        port_cancel_is_set(const port_cancel *c);
void                       port_cancel_destroy(port_cancel *c);

typedef struct port_header {
    const char *name;
    size_t      name_len;
    const char *value;
    size_t      value_len;
} port_header;

typedef struct port_http_request {
    uint32_t           size;
    const char        *method; /* "GET", "POST", ... */
    const char        *url;    /* http:// or https:// (absolute, already parsed by the core) */
    const port_header *headers;
    size_t             header_count;
    const void        *body; /* sent as is; Content-Length is the port's to add */
    size_t             body_len;
    int32_t            timeout_ms; /* connect + send + the whole answer */
    port_cancel       *cancel;     /* may be NULL */
} port_http_request;

/* The answer, as it arrives, on the calling thread. Return 0 to go on,
 * nonzero to stop (the exchange then returns PORT_E_ABORTED). */
typedef struct port_http_handler {
    uint32_t size;
    void    *user;
    /* Once: the final status line and headers (in arrival order). */
    int (*on_head)(void *user, int status, const port_header *headers, size_t count);
    /* The body in order, de-chunked, never decompressed. */
    int (*on_body)(void *user, const void *data, size_t n);
} port_http_handler;

/* One exchange: no redirects followed, no cookies kept, no Accept-Encoding
 * added. A port may keep connections alive between calls and retry a request
 * on a kept-alive connection that turned out dead — but only when nothing of
 * the answer has reached on_head yet, and a non-idempotent method (POST)
 * only when the request was never sent.
 * PORT_OK once the whole answer has been delivered, else PORT_E_DNS,
 * PORT_E_CONNECT, PORT_E_TLS, PORT_E_TIMEOUT, PORT_E_CANCELLED,
 * PORT_E_PROTOCOL, PORT_E_UNSUPPORTED (https without TLS), PORT_E_ABORTED.
 * detail (may be NULL) gets a short reason for the log ("hostname
 * mismatch", "connection refused"). */
int port_http_perform(
    const port_http_request *req, const port_http_handler *h, char *detail, size_t detail_cap
);

/* ── WebSocket (RFC 6455, client) ────────────────────────────────────────── */
typedef struct port_ws port_ws;
enum { PORT_WS_TEXT = 1, PORT_WS_BINARY = 2, PORT_WS_CLOSED = 3 };
typedef struct port_ws_message {
    uint32_t size;
    uint32_t kind;       /* PORT_WS_* */
    int32_t  close_code; /* CLOSED: the peer's code, 1006 when the connection dropped */
    char    *data;       /* payload / CLOSED: the reason; free with port_ws_message_free */
    size_t   len;
} port_ws_message;
/* The opening handshake (ws:// or wss://), with the caller's extra headers
 * (Origin, Cookie...). PORT_OK with *out, or an error as for HTTP (detail
 * as there). */
int port_ws_connect(
    const char        *url,
    const port_header *headers,
    size_t             header_count,
    int32_t            timeout_ms,
    port_cancel       *cancel,
    port_ws          **out,
    char              *detail,
    size_t             detail_cap
);
/* Blocks for one whole message (fragments joined, pings answered, pongs
 * dropped). After a CLOSED message (returned once) it keeps returning
 * PORT_E_CLOSED. */
int  port_ws_recv(port_ws *ws, port_ws_message *out);
void port_ws_message_free(port_ws_message *m);
int  port_ws_send(port_ws *ws, const void *data, size_t n, int text);
/* A ping control frame (<= 125 bytes); PORT_E_NOSYS where the stack pings by
 * itself. */
int  port_ws_ping(port_ws *ws, const void *data, size_t n);
/* Starts the closing handshake; recv then returns CLOSED. */
int  port_ws_close(port_ws *ws, int code);
/* Tears the connection down now: a blocked recv returns CLOSED promptly. */
void port_ws_abort(port_ws *ws);
/* After recv has returned (or was never called). */
void port_ws_destroy(port_ws *ws);

/* A TCP port on 127.0.0.1 that was free a moment ago (for a child's
 * DevTools); 0 when unknown. */
int  port_net_loopback_port(void);
/* Drops kept-alive connections and caches (TLS sessions, DNS): at exit and
 * when the core goes idle. */
void port_net_release(void);

#ifdef __cplusplus
}
#endif

#endif
