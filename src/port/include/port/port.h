/* port.h — the port contract: everything msga's C++ core needs from an OS,
 * as plain C. See docs/port-contract.md for the design and how to write a
 * port.
 *
 * A port is one set of C files, compiled by the target OS's own C compiler,
 * that defines every function declared under include/port/. msga's C++ core
 * (cross-compiled for the target) calls only these functions; it never
 * includes an OS header in a port build.
 *
 * Rules every header follows:
 *   - Pure C11 that also compiles as C++; only <stddef.h> and <stdint.h>
 *     (freestanding headers) are included. No OS headers, no C++ types.
 *   - Opaque handles (struct port_x *), created and destroyed by the port.
 *   - Strings are UTF-8, NUL-terminated unless a length is passed with them.
 *     Paths use '/' as the separator.
 *   - Memory: the caller owns every buffer it passes in. Strings a port
 *     returns go into caller buffers (a getter returns the full length, so a
 *     too-small buffer can be retried), or are valid only for the duration
 *     of the callback that hands them over, or are freed with the matching
 *     port_*_free function. Nothing is ever freed with the C library's free.
 *   - Errors are integers: PORT_OK (0) or a negative PORT_E_* code. Functions
 *     that return a count or a length return it as a non-negative value.
 *   - Callbacks take a `void *user` first, the value the caller registered.
 *   - Every struct that crosses the boundary starts with `uint32_t size`, set
 *     to sizeof(struct) by whoever fills it. The reader only looks at the
 *     fields that fit in that size, so a newer core works with an older port
 *     (and the other way round) within one major ABI version.
 *   - Every function exists in every port. Something the OS can't do returns
 *     PORT_E_NOSYS (or does nothing, for void functions), and the matching
 *     capability bit is clear: the core asks port_caps() first and degrades
 *     (no tray, no drag and drop, one surface instead of windows...) instead
 *     of compiling per-OS code.
 *
 * Threading: see each header. In short, port_app_* / port_window_* and the
 * other UI calls belong to the thread that created the port_app; port_app_wake
 * and everything in port_sys.h and port_net.h may be called from any thread.
 */
#ifndef PORT_PORT_H
#define PORT_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Versioning ────────────────────────────────────────────────────────────
 * Major: incompatible changes (a function's meaning, a field's type or
 * position). Minor: additions (functions, trailing struct fields, capability
 * bits, event types). The core refuses a port whose major differs or whose
 * minor is lower than the one it was built against (port_check_abi). */
#define PORT_ABI_MAJOR 1
#define PORT_ABI_MINOR 2 /* 1: PORT_PATH_FONTS, PORT_PATH_RESOURCES, port_pid_*; 2: port_font_* */
#define PORT_ABI_VERSION ((PORT_ABI_MAJOR << 16) | PORT_ABI_MINOR)

/* ── Errors ──────────────────────────────────────────────────────────────── */
enum {
    PORT_OK            = 0,
    PORT_E_FAIL        = -1, /* anything not listed below */
    PORT_E_NOSYS       = -2, /* not supported by this port (see the capability) */
    PORT_E_INVAL       = -3, /* bad argument */
    PORT_E_NOMEM       = -4,
    PORT_E_NOENT       = -5, /* no such file, key, variable */
    PORT_E_EXIST       = -6,
    PORT_E_ACCES       = -7, /* permission denied */
    PORT_E_NOTDIR      = -8,
    PORT_E_ISDIR       = -9,
    PORT_E_NOTEMPTY    = -10,
    PORT_E_NOSPC       = -11,
    PORT_E_IO          = -12,
    PORT_E_RANGE       = -13, /* a caller buffer is too small; retry with more */
    PORT_E_AGAIN       = -14, /* would block (a zero timeout) */
    PORT_E_TIMEOUT     = -15,
    PORT_E_CANCELLED   = -16, /* a port_cancel was set */
    PORT_E_CLOSED      = -17, /* the other end has gone (pipe, socket) */
    PORT_E_BUSY        = -18,
    PORT_E_DENIED      = -19, /* the user or a privacy setting refused */
    /* Network. */
    PORT_E_DNS         = -40,
    PORT_E_CONNECT     = -41,
    PORT_E_TLS         = -42,
    PORT_E_PROTOCOL    = -43,
    PORT_E_UNSUPPORTED = -44, /* e.g. https/wss on a port without TLS */
    PORT_E_ABORTED     = -45  /* a callback asked to stop */
};

/* A short English name for an error code ("timeout"), for logs. */
const char *port_strerror(int err);

/* ── Capabilities ──────────────────────────────────────────────────────────
 * What the port can do. The core checks these instead of #ifdef'ing per OS;
 * a clear bit means the matching functions return PORT_E_NOSYS. Macros, not
 * an enum: bit 31 is outside the range ISO C allows an enumerator. */
/* Surfaces and windows (port_app.h). Without PORT_CAP_WINDOWS the port
 * offers ONE surface filling the screen (or its app area): it cannot be
 * moved, resized or minimised by the app, and further windows the core
 * asks for share it (only the last one shown is attached). */
#define PORT_CAP_WINDOWS (1u << 0)
#define PORT_CAP_WINDOW_POSITION (1u << 1) /* position get/set, Moved events */
#define PORT_CAP_SYSTEM_DECOR (1u << 2)    /* the OS draws title bars when asked */
#define PORT_CAP_ALWAYS_ON_TOP (1u << 3)
#define PORT_CAP_DRAG_SOURCE (1u << 4)
#define PORT_CAP_DROP_TARGET (1u << 5)
#define PORT_CAP_CLIPBOARD (1u << 6)
#define PORT_CAP_PRIMARY_SELECTION (1u << 7)
#define PORT_CAP_CURSORS (1u << 8)     /* set_cursor changes the pointer shape */
#define PORT_CAP_IME_PREEDIT (1u << 9) /* TextPreedit events (else commits only) */
#define PORT_CAP_MONITORS (1u << 10)
#define PORT_CAP_TRAY (1u << 11)
#define PORT_CAP_TRAY_MENU (1u << 12)
#define PORT_CAP_TRAY_ICON (1u << 13) /* the app sets the tray picture */
#define PORT_CAP_BADGE (1u << 14)     /* an unread count (else a dot at most) */
#define PORT_CAP_NOTIFY (1u << 15)
#define PORT_CAP_NOTIFY_IMAGES (1u << 16)
#define PORT_CAP_NOTIFY_ACTIONS (1u << 17)
#define PORT_CAP_NOTIFY_WITHDRAW (1u << 18)
#define PORT_CAP_SINGLE_INSTANCE (1u << 19)
#define PORT_CAP_URL_SCHEME (1u << 20)
#define PORT_CAP_FILE_DIALOG (1u << 21)
#define PORT_CAP_NETWORK_STATUS (1u << 22) /* port_network_online + NetworkChanged */
#define PORT_CAP_POWER_EVENTS (1u << 23)   /* Suspending / Resumed */
#define PORT_CAP_OPEN_URL (1u << 24)
/* System (port_sys.h). */
#define PORT_CAP_PROCESS (1u << 25)       /* port_proc_spawn */
#define PORT_CAP_PROCESS_PIPES (1u << 26) /* stdin/stdout/stderr pipes to a child */
#define PORT_CAP_PROCESS_KILL (1u << 27)
#define PORT_CAP_PTY (1u << 28)
#define PORT_CAP_SECRETS (1u << 29) /* a keychain (else the core's settings file) */
/* Network (port_net.h). */
#define PORT_CAP_TLS (1u << 30)        /* https:// and wss:// */
#define PORT_CAP_TEST_HOOKS (1u << 31) /* port_test.h works (test builds only) */
/* The second word, port_caps2(). */
#define PORT_CAP2_AUDIO_PLAY (1u << 0)
#define PORT_CAP2_AUDIO_CAPTURE (1u << 1)
#define PORT_CAP2_AUDIO_SOUNDS (1u << 2)
#define PORT_CAP2_ATOMIC_RENAME (1u << 3) /* port_rename replaces atomically */
#define PORT_CAP2_FILE_WATCH (1u << 4)    /* port_watch_dir */
#define PORT_CAP2_FONTS (1u << 5)         /* port_font_info / port_font_data (minor 2) */

/* What port_init reports. */
typedef struct port_info {
    uint32_t    size;
    uint32_t    abi_version; /* PORT_ABI_VERSION the port was built against */
    const char *name;        /* "peos", "ref" — for logs and backendName() */
    uint32_t    caps;        /* PORT_CAP_* */
    uint32_t    caps2;       /* PORT_CAP2_* */
    /* The largest value port_secret_write takes (bytes); 0 = no limit. */
    uint32_t    secret_max;
} port_info;

/* Called once, first, on the thread that will run the app, before any other
 * port call. Fills *info (whose size the caller set). Calling it again
 * returns PORT_OK and the same info. */
int      port_init(port_info *info);
/* At exit, after everything the core made is destroyed. */
void     port_shutdown(void);
uint32_t port_caps(void);
uint32_t port_caps2(void);

/* The core's check: PORT_OK when a port reporting `abi_version` can serve a
 * core built against PORT_ABI_VERSION. Inline so both sides share it. */
static inline int port_check_abi(uint32_t abi_version) {
    return (abi_version >> 16) == PORT_ABI_MAJOR && (int)(abi_version & 0xffffu) >= PORT_ABI_MINOR
               ? PORT_OK
               : PORT_E_NOSYS;
}

/* Diagnostics: a line (without the newline) to wherever the OS keeps an
 * app's error output. Any thread. */
void port_log_write(const char *line, size_t len);

#ifdef __cplusplus
}
#endif

#include "port/port_sys.h"
#include "port/port_net.h"
#include "port/port_app.h"
#include "port/port_audio.h"

#endif
