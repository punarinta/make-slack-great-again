/* port_sys.h — threads, locks, clocks, files, environment, processes and
 * secrets. Every function here may be called from any thread (a handle from
 * one thread at a time unless said otherwise).
 *
 * Not here on purpose: atomics (the compiler's), thread-local storage (the
 * compiler's), "once" (the core builds it from atomics and port_mutex), and
 * DNS (inside port_net.h's transports: the core never resolves names
 * itself). */
#ifndef PORT_PORT_SYS_H
#define PORT_PORT_SYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Threads ───────────────────────────────────────────────────────────────
 * A joinable thread running fn(arg). stack_bytes is a request (0 = the
 * port's default); the core asks for 256 KiB to 1 MiB and needs at least
 * 256 KiB for a TLS handshake. Every port_thread is joined or detached
 * exactly once; both free the handle. */
typedef struct port_thread port_thread;
int      port_thread_start(port_thread **out, void (*fn)(void *arg), void *arg, size_t stack_bytes);
int      port_thread_join(port_thread *t);
void     port_thread_detach(port_thread *t);
/* A small number unique among the live threads, for logs and assertions. */
uint64_t port_thread_id(void);
/* Logical CPUs (sizes the core's worker pools); 1 when unknown. */
int      port_cpu_count(void);

/* A non-recursive mutex. Unlocking from another thread is undefined. */
typedef struct port_mutex port_mutex;
int                       port_mutex_create(port_mutex **out);
void                      port_mutex_destroy(port_mutex *m);
void                      port_mutex_lock(port_mutex *m);
void                      port_mutex_unlock(port_mutex *m);
int                       port_mutex_trylock(port_mutex *m); /* PORT_OK or PORT_E_BUSY */

/* A condition variable used with one port_mutex. Waits may wake spuriously;
 * the core always re-checks its predicate. */
typedef struct port_cond port_cond;
int                      port_cond_create(port_cond **out);
void                     port_cond_destroy(port_cond *c);
void                     port_cond_wait(port_cond *c, port_mutex *m);
/* PORT_OK when woken, PORT_E_TIMEOUT after timeout_ms (monotonic). */
int                      port_cond_wait_ms(port_cond *c, port_mutex *m, uint32_t timeout_ms);
void                     port_cond_signal(port_cond *c);
void                     port_cond_broadcast(port_cond *c);

/* ── Time ──────────────────────────────────────────────────────────────── */
/* Nanoseconds from an arbitrary start; never goes back, never jumps. */
int64_t port_time_monotonic_ns(void);
/* UTC microseconds since 1970-01-01. */
int64_t port_time_wall_us(void);
/* Seconds east of UTC in the user's time zone at the UTC instant `utc_secs`
 * (DST included). 0 when the OS has no zone. */
int32_t port_time_local_offset(int64_t utc_secs);
void    port_sleep_ms(uint32_t ms);

/* Cryptographically secure random bytes. PORT_OK or an error (never a
 * partial fill). */
int port_random(void *buf, size_t n);

/* ── Environment ───────────────────────────────────────────────────────────
 * Variables are the core's way to take settings from its launcher and to
 * pass them to children; a port without them returns PORT_E_NOENT. */
/* Length of the value (buf gets as much as fits, NUL-terminated), or
 * PORT_E_NOENT when unset. */
int  port_env_get(const char *name, char *buf, size_t cap);
/* value NULL unsets. */
int  port_env_set(const char *name, const char *value);
/* Calls fn once per "NAME=value" (as one string). */
void port_env_list(void (*fn)(void *user, const char *name_value), void *user);

/* ── Well-known paths ──────────────────────────────────────────────────────
 * Absolute, no trailing '/'. A port with one app folder (PEOS's
 * /apps/<name>) answers CONFIG/DATA/STATE with it and CACHE with a folder
 * under it. The core creates missing folders itself. */
enum {
    PORT_PATH_HOME = 0,
    PORT_PATH_CONFIG,
    PORT_PATH_DATA,
    PORT_PATH_CACHE,
    PORT_PATH_STATE,
    PORT_PATH_TEMP,
    PORT_PATH_DESKTOP,
    PORT_PATH_DOCUMENTS,
    PORT_PATH_DOWNLOADS,
    PORT_PATH_PICTURES,
    PORT_PATH_EXECUTABLE, /* the running program's file */
    PORT_PATH_CWD,        /* the working directory, for relative paths */
    PORT_PATH_COUNT
};
/* Length of the path (buf gets as much as fits, NUL-terminated), or
 * PORT_E_NOENT when the OS has no such place. */
int port_path_get(int which, char *buf, size_t cap);

/* ── Files ─────────────────────────────────────────────────────────────── */
enum {
    PORT_O_READ   = 1,
    PORT_O_WRITE  = 2,
    PORT_O_CREATE = 4,
    PORT_O_TRUNC  = 8,
    PORT_O_EXCL   = 16, /* with CREATE: fail with PORT_E_EXIST if it exists */
    PORT_O_APPEND = 32
};
typedef struct port_file port_file;
/* mode: POSIX permission bits for a created file (0600 for secrets); a port
 * without permissions ignores it. */
int     port_file_open(const char *path, uint32_t flags, uint32_t mode, port_file **out);
/* Bytes read (0 at the end) or an error. Short reads are allowed. */
int64_t port_file_read(port_file *f, void *buf, size_t n);
/* At `offset`, not moving the position used by port_file_read. */
int64_t port_file_pread(port_file *f, void *buf, size_t n, int64_t offset);
/* Bytes written or an error. Short writes are allowed. */
int64_t port_file_write(port_file *f, const void *buf, size_t n);
/* Durable on disk when it returns PORT_OK. */
int     port_file_sync(port_file *f);
int64_t port_file_size(port_file *f);
/* Closes and frees; PORT_OK when everything written has been kept. */
int     port_file_close(port_file *f);

enum { PORT_KIND_NONE = 0, PORT_KIND_FILE, PORT_KIND_DIR, PORT_KIND_LINK, PORT_KIND_OTHER };
typedef struct port_stat_info {
    uint32_t size;
    uint32_t kind;     /* PORT_KIND_* */
    int64_t  bytes;    /* a regular file's length */
    int64_t  mtime_us; /* last modification, UTC microseconds */
    int64_t  btime_us; /* creation where the file system keeps it, else mtime_us */
    uint32_t mode;     /* POSIX permission bits; 0644 / 0755 where there are none */
} port_stat_info;
/* follow_links 0 looks at a link itself (kind PORT_KIND_LINK). */
int port_stat(const char *path, int follow_links, port_stat_info *out);
/* One directory, not its parents (the core walks them). PORT_E_EXIST when
 * something is there already. */
int port_mkdir(const char *path, uint32_t mode);
/* A file, a link, or an empty directory. */
int port_remove(const char *path);
/* Moves `from` over `to`, replacing it. With PORT_CAP2_ATOMIC_RENAME a reader
 * of `to` sees the old or the new file, never neither — the core's
 * crash-safe writes (temp file + sync + rename) depend on it. */
int port_rename(const char *from, const char *to);
/* Sets the modification time to now. */
int port_touch(const char *path);

typedef struct port_dir port_dir;
typedef struct port_dir_entry {
    uint32_t    size;
    const char *name;  /* valid until the next port_dir_next / close */
    uint32_t    kind;  /* PORT_KIND_*, links followed; NONE when it can't be looked at */
    int64_t     bytes; /* regular files */
    int64_t     mtime_us;
    int         hidden; /* a dot file, or the OS's hidden flag */
} port_dir_entry;
int  port_dir_open(const char *path, port_dir **out);
/* 1 = *out filled, 0 = no more entries, < 0 = error. "." and ".." are never
 * listed. */
int  port_dir_next(port_dir *d, port_dir_entry *out);
void port_dir_close(port_dir *d);

/* ── Processes ─────────────────────────────────────────────────────────────
 * Children the core starts: a browser for sign-in, short commands whose
 * output it reads, the claude CLI. PORT_CAP_PROCESS says spawn works at all,
 * PORT_CAP_PROCESS_PIPES that the PIPE flags do, PORT_CAP_PROCESS_KILL that
 * port_proc_kill does, PORT_CAP_PTY that PORT_SPAWN_PTY does. */
enum {
    PORT_SPAWN_STDIN_PIPE   = 1,  /* else the null device */
    PORT_SPAWN_STDOUT_PIPE  = 2,  /* else the null device */
    PORT_SPAWN_STDERR_PIPE  = 4,  /* else the null device ... */
    PORT_SPAWN_STDERR_MERGE = 8,  /* ... or into the stdout pipe */
    PORT_SPAWN_NEW_GROUP    = 16, /* its own session/group, so kill reaches its children */
    PORT_SPAWN_SEARCH_PATH  = 32, /* exe is a bare name looked up the OS's way */
    PORT_SPAWN_PTY          = 64  /* stdin/stdout are a terminal (PORT_CAP_PTY) */
};
typedef struct port_spawn_desc {
    uint32_t           size;
    const char        *exe;  /* a path (or a name with PORT_SPAWN_SEARCH_PATH) */
    const char *const *argv; /* the arguments after argv[0] */
    size_t             argc;
    const char        *cwd; /* NULL = ours */
    /* "NAME=value" set / "NAME=" unset on top of our environment; the last
     * word on a name wins. */
    const char *const *env;
    size_t             envc;
    uint32_t           flags; /* PORT_SPAWN_* */
    uint16_t           pty_cols, pty_rows;
} port_spawn_desc;
enum { PORT_STDOUT = 1, PORT_STDERR = 2 };
typedef struct port_proc port_proc;
int                      port_proc_spawn(const port_spawn_desc *d, port_proc **out);
/* From a stdout/stderr pipe: bytes (> 0), 0 at its end, PORT_E_TIMEOUT after
 * timeout_ms (0 = don't wait, -1 = forever), or an error. */
int64_t port_proc_read(port_proc *p, int stream, void *buf, size_t n, int32_t timeout_ms);
/* To the stdin pipe: bytes written (maybe fewer), PORT_E_TIMEOUT, or
 * PORT_E_CLOSED when the child closed its end. Never raises a signal. */
int64_t port_proc_write(port_proc *p, const void *buf, size_t n, int32_t timeout_ms);
int     port_proc_close_stdin(port_proc *p);
/* PORT_OK with *code (exit status, or 128 + signal where there are
 * signals), or PORT_E_TIMEOUT while it runs (timeout_ms as for read). */
int     port_proc_wait(port_proc *p, int32_t timeout_ms, int *code);
/* force 0: ask it to end (SIGTERM to the group); 1: end it now. */
int     port_proc_kill(port_proc *p, int force);
int64_t port_proc_pid(port_proc *p);
/* Terminal size of a PORT_SPAWN_PTY child. */
int     port_proc_resize(port_proc *p, uint16_t cols, uint16_t rows);
/* Frees the handle and closes our pipe ends; the child keeps running. */
void    port_proc_release(port_proc *p);

/* ── Secrets ───────────────────────────────────────────────────────────────
 * A keychain (PORT_CAP_SECRETS). Keys are the core's own ("msga/workspace/
 * T0123/auth"); a port that must prefix them (PEOS: "<component>.") does so
 * itself. Values are bytes, at most port_info.secret_max of them; the core
 * keeps longer ones in its own settings file. Calls may block on a prompt
 * (unlocking the keychain): the core makes them off the UI thread or at
 * load/save time only. */
/* PORT_OK with *len set (buf gets as much as fits), PORT_E_RANGE when cap
 * was too small (*len = what is needed), PORT_E_NOENT when absent,
 * PORT_E_DENIED when the user refused to unlock. */
int port_secret_read(const char *key, void *buf, size_t cap, size_t *len);
int port_secret_write(const char *key, const void *data, size_t len);
int port_secret_remove(const char *key); /* PORT_OK also when absent */

#ifdef __cplusplus
}
#endif

#endif
