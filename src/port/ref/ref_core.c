/* The reference port: the whole contract in plain C11 over POSIX, headless
 * (in-memory surfaces, input only through port_test.h), HTTP and WebSocket
 * over plain TCP (no TLS). It exists to prove the contract: msga's test
 * suites run on it (-DMSGA_PLATFORM=port), and a new port can start from
 * it. Not for users. */
#define _POSIX_C_SOURCE 200809L
#include "ref_internal.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const port_info kInfo = {
    sizeof(port_info),
    PORT_ABI_VERSION,
    "ref",
    /* Everything an in-memory desktop can fake; no TLS, no PTY. */
    PORT_CAP_WINDOWS | PORT_CAP_WINDOW_POSITION | PORT_CAP_SYSTEM_DECOR | PORT_CAP_ALWAYS_ON_TOP |
        PORT_CAP_DRAG_SOURCE | PORT_CAP_DROP_TARGET | PORT_CAP_CLIPBOARD |
        PORT_CAP_PRIMARY_SELECTION | PORT_CAP_CURSORS | PORT_CAP_MONITORS | PORT_CAP_TRAY |
        PORT_CAP_TRAY_MENU | PORT_CAP_TRAY_ICON | PORT_CAP_BADGE | PORT_CAP_NOTIFY |
        PORT_CAP_NOTIFY_IMAGES | PORT_CAP_NOTIFY_ACTIONS | PORT_CAP_NOTIFY_WITHDRAW |
        PORT_CAP_SINGLE_INSTANCE | PORT_CAP_URL_SCHEME | PORT_CAP_FILE_DIALOG |
        PORT_CAP_NETWORK_STATUS | PORT_CAP_POWER_EVENTS | PORT_CAP_OPEN_URL | PORT_CAP_PROCESS |
        PORT_CAP_PROCESS_PIPES | PORT_CAP_PROCESS_KILL | PORT_CAP_SECRETS | PORT_CAP_TEST_HOOKS,
#ifdef __linux__ /* the directory watcher is inotify */
    PORT_CAP2_ATOMIC_RENAME | PORT_CAP2_FILE_WATCH,
#else
    PORT_CAP2_ATOMIC_RENAME,
#endif
    0,
};

static int     g_single = -1; /* -1: not looked at yet */
static int32_t g_surface_w, g_surface_h;

int ref_single_surface(int32_t *w, int32_t *h) {
    if (g_single < 0) {
        const char *v  = getenv("MSGA_PORT_REF_SURFACE");
        int         sw = 0, sh = 0;
        /* "auto": each window finds the surface the size it asked for (the
         * test suites' fixed-size windows), everything else as above. */
        g_single =
            v && (!strcmp(v, "auto") || (sscanf(v, "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0));
        g_surface_w = sw;
        g_surface_h = sh;
    }
    if (g_single && w)
        *w = g_surface_w;
    if (g_single && h)
        *h = g_surface_h;
    return g_single;
}

/* One surface: no windows of its own to place, decorate or stack. */
static uint32_t caps(void) {
    const uint32_t windowed = PORT_CAP_WINDOWS | PORT_CAP_WINDOW_POSITION | PORT_CAP_SYSTEM_DECOR |
                              PORT_CAP_ALWAYS_ON_TOP;
    return ref_single_surface(NULL, NULL) ? kInfo.caps & ~windowed : kInfo.caps;
}

int port_init(port_info *info) {
    if (!info || info->size < sizeof(uint32_t))
        return PORT_E_INVAL;
    const uint32_t n = info->size < sizeof kInfo ? info->size : (uint32_t)sizeof kInfo;
    memcpy(info, &kInfo, n);
    if (n >= offsetof(port_info, caps) + sizeof info->caps)
        info->caps = caps();
    info->size = n;
    return PORT_OK;
}

void port_shutdown(void) {}

uint32_t port_caps(void) {
    return caps();
}

uint32_t port_caps2(void) {
    return kInfo.caps2;
}

const char *port_strerror(int err) {
    switch (err) {
    case PORT_OK:
        return "ok";
    case PORT_E_NOSYS:
        return "not supported";
    case PORT_E_INVAL:
        return "invalid argument";
    case PORT_E_NOMEM:
        return "out of memory";
    case PORT_E_NOENT:
        return "not found";
    case PORT_E_EXIST:
        return "exists";
    case PORT_E_ACCES:
        return "permission denied";
    case PORT_E_NOTDIR:
        return "not a directory";
    case PORT_E_ISDIR:
        return "is a directory";
    case PORT_E_NOTEMPTY:
        return "directory not empty";
    case PORT_E_NOSPC:
        return "no space";
    case PORT_E_IO:
        return "i/o error";
    case PORT_E_RANGE:
        return "buffer too small";
    case PORT_E_AGAIN:
        return "would block";
    case PORT_E_TIMEOUT:
        return "timeout";
    case PORT_E_CANCELLED:
        return "cancelled";
    case PORT_E_CLOSED:
        return "closed";
    case PORT_E_BUSY:
        return "busy";
    case PORT_E_DENIED:
        return "denied";
    case PORT_E_DNS:
        return "dns";
    case PORT_E_CONNECT:
        return "connect";
    case PORT_E_TLS:
        return "tls";
    case PORT_E_PROTOCOL:
        return "protocol";
    case PORT_E_UNSUPPORTED:
        return "unsupported";
    case PORT_E_ABORTED:
        return "aborted";
    default:
        return "failed";
    }
}

void port_log_write(const char *line, size_t len) {
    /* One write per line, so lines from several threads never interleave. */
    char  stack[1024];
    char *buf = len + 1 <= sizeof stack ? stack : (char *)malloc(len + 1);
    if (!buf)
        return;
    memcpy(buf, line, len);
    buf[len]   = '\n';
    ssize_t rc = write(2, buf, len + 1);
    (void)rc;
    if (buf != stack)
        free(buf);
}

int ref_errno(int e) {
    switch (e) {
    case 0:
        return PORT_OK;
    case ENOENT:
        return PORT_E_NOENT;
    case EEXIST:
        return PORT_E_EXIST;
    case EACCES:
    case EPERM:
    case EROFS:
        return PORT_E_ACCES;
    case ENOTDIR:
        return PORT_E_NOTDIR;
    case EISDIR:
        return PORT_E_ISDIR;
    case ENOTEMPTY:
        return PORT_E_NOTEMPTY;
    case ENOSPC:
        return PORT_E_NOSPC;
    case ENOMEM:
        return PORT_E_NOMEM;
    case EINVAL:
        return PORT_E_INVAL;
    case EAGAIN:
        return PORT_E_AGAIN;
    case ETIMEDOUT:
        return PORT_E_TIMEOUT;
    case EPIPE:
        return PORT_E_CLOSED;
    case EIO:
        return PORT_E_IO;
    case EBUSY:
        return PORT_E_BUSY;
    case ENOSYS:
        return PORT_E_NOSYS;
    default:
        return PORT_E_FAIL;
    }
}

int ref_copy_out(const char *s, char *buf, size_t cap) {
    const size_t n = strlen(s);
    if (buf && cap) {
        const size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(buf, s, k);
        buf[k] = 0;
    }
    return (int)n;
}

char *ref_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (p) {
        if (n)
            memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

void ref_detail(char *buf, size_t cap, const char *msg) {
    if (buf && cap)
        ref_copy_out(msg, buf, cap);
}
