/* port_sys.h over POSIX: pthreads, clock_gettime, stdio-free file I/O,
 * posix_spawn with pipes, and a 0600 file per secret. */
#define _GNU_SOURCE /* pipe2, getrandom; everything else is POSIX.1-2008 */
#include "ref_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ── Threads ───────────────────────────────────────────────────────────── */

/* state: 0 running, 1 fn returned, 2 detached. Whoever moves it second (the
 * thread finishing, or port_thread_detach) frees the handle, so a detached
 * thread never touches freed memory and never leaks its handle. */
struct port_thread {
    pthread_t t;
    void (*fn)(void *);
    void      *arg;
    atomic_int state;
};

static void *trampoline(void *p) {
    struct port_thread *t = (struct port_thread *)p;
    t->fn(t->arg);
    int expected = 0;
    if (!atomic_compare_exchange_strong(&t->state, &expected, 1))
        free(t); /* detached while running: ours to free */
    return NULL;
}

int port_thread_start(port_thread **out, void (*fn)(void *), void *arg, size_t stack_bytes) {
    struct port_thread *t = (struct port_thread *)calloc(1, sizeof *t);
    if (!t)
        return PORT_E_NOMEM;
    t->fn  = fn;
    t->arg = arg;
    atomic_init(&t->state, 0);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes < 65536 ? 65536 : stack_bytes);
    const int rc = pthread_create(&t->t, &attr, trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return ref_errno(rc);
    }
    *out = t;
    return PORT_OK;
}

int port_thread_join(port_thread *t) {
    const int rc = pthread_join(t->t, NULL);
    free(t);
    return ref_errno(rc);
}

void port_thread_detach(port_thread *t) {
    pthread_detach(t->t);
    int expected = 0;
    if (!atomic_compare_exchange_strong(&t->state, &expected, 2))
        free(t); /* fn has returned already */
}

uint64_t port_thread_id(void) {
    static _Thread_local uint64_t id;
    static atomic_uint_fast64_t   next;
    if (!id)
        id = (uint64_t)atomic_fetch_add(&next, 1) + 1;
    return id;
}

int port_cpu_count(void) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

struct port_mutex {
    pthread_mutex_t m;
};

int port_mutex_create(port_mutex **out) {
    struct port_mutex *m = (struct port_mutex *)malloc(sizeof *m);
    if (!m)
        return PORT_E_NOMEM;
    pthread_mutex_init(&m->m, NULL);
    *out = m;
    return PORT_OK;
}

void port_mutex_destroy(port_mutex *m) {
    if (!m)
        return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void port_mutex_lock(port_mutex *m) {
    pthread_mutex_lock(&m->m);
}

void port_mutex_unlock(port_mutex *m) {
    pthread_mutex_unlock(&m->m);
}

int port_mutex_trylock(port_mutex *m) {
    return pthread_mutex_trylock(&m->m) == 0 ? PORT_OK : PORT_E_BUSY;
}

struct port_cond {
    pthread_cond_t c;
};

int port_cond_create(port_cond **out) {
    struct port_cond *c = (struct port_cond *)malloc(sizeof *c);
    if (!c)
        return PORT_E_NOMEM;
    /* Timed waits on the monotonic clock: a wall-clock jump must not stretch
     * or cut a timeout. */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &attr);
    pthread_condattr_destroy(&attr);
    *out = c;
    return PORT_OK;
}

void port_cond_destroy(port_cond *c) {
    if (!c)
        return;
    pthread_cond_destroy(&c->c);
    free(c);
}

void port_cond_wait(port_cond *c, port_mutex *m) {
    pthread_cond_wait(&c->c, &m->m);
}

int port_cond_wait_ms(port_cond *c, port_mutex *m, uint32_t timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == ETIMEDOUT ? PORT_E_TIMEOUT : PORT_OK;
}

void port_cond_signal(port_cond *c) {
    pthread_cond_signal(&c->c);
}

void port_cond_broadcast(port_cond *c) {
    pthread_cond_broadcast(&c->c);
}

/* ── Time ──────────────────────────────────────────────────────────────── */

int64_t port_time_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int64_t port_time_wall_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int32_t port_time_local_offset(int64_t utc_secs) {
    const time_t t = (time_t)utc_secs;
    struct tm    tm;
    if (!localtime_r(&t, &tm))
        return 0;
    return (int32_t)tm.tm_gmtoff;
}

void port_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

int port_random(void *buf, size_t n) {
    unsigned char *p = (unsigned char *)buf;
    while (n > 0) {
        const ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return ref_errno(errno);
        }
        p += got;
        n -= (size_t)got;
    }
    return PORT_OK;
}

/* ── Environment ───────────────────────────────────────────────────────── */

int port_env_get(const char *name, char *buf, size_t cap) {
    const char *v = getenv(name);
    return v ? ref_copy_out(v, buf, cap) : PORT_E_NOENT;
}

int port_env_set(const char *name, const char *value) {
    const int rc = value ? setenv(name, value, 1) : unsetenv(name);
    return rc == 0 ? PORT_OK : ref_errno(errno);
}

void port_env_list(void (*fn)(void *, const char *), void *user) {
    for (char **e = environ; e && *e; ++e)
        fn(user, *e);
}

/* ── Paths ─────────────────────────────────────────────────────────────── */

/* Like plat's headless backend: everything under $HOME/plat-headless, so
 * every test run (each with its own HOME) has its own folders. */
int port_path_get(int which, char *buf, size_t cap) {
    static const char *const names[] = {
        "home",
        "config",
        "data",
        "cache",
        "state",
        "tmp",
        "desktop",
        "documents",
        "downloads",
        "pictures",
    };
    char        path[4096];
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    switch (which) {
    case PORT_PATH_HOME:
        return ref_copy_out(home, buf, cap);
    case PORT_PATH_EXECUTABLE: {
        const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        if (n <= 0)
            return PORT_E_NOENT;
        path[n] = 0;
        return ref_copy_out(path, buf, cap);
    }
    case PORT_PATH_CWD:
        return getcwd(path, sizeof path) ? ref_copy_out(path, buf, cap) : ref_errno(errno);
    default:
        if (which < 0 || which >= PORT_PATH_EXECUTABLE)
            return PORT_E_INVAL;
        if ((size_t)snprintf(path, sizeof path, "%s/plat-headless", home) >= sizeof path)
            return PORT_E_RANGE;
        mkdir(path, 0755);
        const size_t at = strlen(path);
        snprintf(path + at, sizeof path - at, "/%s", names[which]);
        mkdir(path, 0755);
        return ref_copy_out(path, buf, cap);
    }
}

/* ── Files ─────────────────────────────────────────────────────────────── */

struct port_file {
    int fd;
};

int port_file_open(const char *path, uint32_t flags, uint32_t mode, port_file **out) {
    int of = O_CLOEXEC;
    if ((flags & PORT_O_READ) && (flags & PORT_O_WRITE))
        of |= O_RDWR;
    else if (flags & PORT_O_WRITE)
        of |= O_WRONLY;
    else
        of |= O_RDONLY;
    if (flags & PORT_O_CREATE)
        of |= O_CREAT;
    if (flags & PORT_O_TRUNC)
        of |= O_TRUNC;
    if (flags & PORT_O_EXCL)
        of |= O_EXCL;
    if (flags & PORT_O_APPEND)
        of |= O_APPEND;
    const int fd = open(path, of, (mode_t)(mode ? mode : 0644));
    if (fd < 0)
        return ref_errno(errno);
    struct port_file *f = (struct port_file *)malloc(sizeof *f);
    if (!f) {
        close(fd);
        return PORT_E_NOMEM;
    }
    f->fd = fd;
    *out  = f;
    return PORT_OK;
}

int64_t port_file_read(port_file *f, void *buf, size_t n) {
    for (;;) {
        const ssize_t r = read(f->fd, buf, n);
        if (r >= 0)
            return r;
        if (errno != EINTR)
            return ref_errno(errno);
    }
}

int64_t port_file_pread(port_file *f, void *buf, size_t n, int64_t offset) {
    for (;;) {
        const ssize_t r = pread(f->fd, buf, n, (off_t)offset);
        if (r >= 0)
            return r;
        if (errno != EINTR)
            return ref_errno(errno);
    }
}

int64_t port_file_write(port_file *f, const void *buf, size_t n) {
    for (;;) {
        const ssize_t w = write(f->fd, buf, n);
        if (w >= 0)
            return w;
        if (errno != EINTR)
            return ref_errno(errno);
    }
}

int port_file_sync(port_file *f) {
    return fsync(f->fd) == 0 ? PORT_OK : ref_errno(errno);
}

int64_t port_file_size(port_file *f) {
    struct stat st;
    return fstat(f->fd, &st) == 0 ? (int64_t)st.st_size : ref_errno(errno);
}

int port_file_close(port_file *f) {
    const int rc = close(f->fd) == 0 ? PORT_OK : ref_errno(errno);
    free(f);
    return rc;
}

static uint32_t kindOf(mode_t m) {
    return S_ISREG(m)   ? PORT_KIND_FILE
           : S_ISDIR(m) ? PORT_KIND_DIR
           : S_ISLNK(m) ? PORT_KIND_LINK
                        : PORT_KIND_OTHER;
}

int port_stat(const char *path, int follow_links, port_stat_info *out) {
    struct stat st;
    if (!*path || (follow_links ? stat(path, &st) : lstat(path, &st)) != 0)
        return *path ? ref_errno(errno) : PORT_E_NOENT;
    port_stat_info i;
    memset(&i, 0, sizeof i);
    i.size     = sizeof i;
    i.kind     = kindOf(st.st_mode);
    i.bytes    = S_ISREG(st.st_mode) ? (int64_t)st.st_size : 0;
    i.mtime_us = (int64_t)st.st_mtim.tv_sec * 1000000 + st.st_mtim.tv_nsec / 1000;
    i.btime_us = i.mtime_us;
    i.mode     = (uint32_t)(st.st_mode & 07777);
#if defined(__linux__) && defined(STATX_BTIME)
    /* The birth time, where the file system keeps one: the core orders
     * Claude Code sessions (a fork is the younger file) by it. */
    struct statx sx;
    if (statx(AT_FDCWD, path, follow_links ? 0 : AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) == 0 &&
        (sx.stx_mask & STATX_BTIME))
        i.btime_us = (int64_t)sx.stx_btime.tv_sec * 1000000 + sx.stx_btime.tv_nsec / 1000;
#endif
    memcpy(out, &i, out->size < sizeof i ? out->size : sizeof i);
    return PORT_OK;
}

int port_mkdir(const char *path, uint32_t mode) {
    return mkdir(path, (mode_t)(mode ? mode : 0755)) == 0 ? PORT_OK : ref_errno(errno);
}

int port_remove(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return ref_errno(errno);
    return (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) == 0 ? PORT_OK : ref_errno(errno);
}

int port_rename(const char *from, const char *to) {
    return rename(from, to) == 0 ? PORT_OK : ref_errno(errno);
}

int port_touch(const char *path) {
    return utimensat(AT_FDCWD, path, NULL, 0) == 0 ? PORT_OK : ref_errno(errno);
}

struct port_dir {
    DIR *d;
};

int port_dir_open(const char *path, port_dir **out) {
    DIR *d = opendir(path);
    if (!d)
        return ref_errno(errno);
    struct port_dir *pd = (struct port_dir *)malloc(sizeof *pd);
    if (!pd) {
        closedir(d);
        return PORT_E_NOMEM;
    }
    pd->d = d;
    *out  = pd;
    return PORT_OK;
}

int port_dir_next(port_dir *d, port_dir_entry *out) {
    for (;;) {
        errno                  = 0;
        const struct dirent *e = readdir(d->d);
        if (!e)
            return errno ? ref_errno(errno) : 0;
        const char *n = e->d_name;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))
            continue;
        port_dir_entry r;
        memset(&r, 0, sizeof r);
        r.size   = sizeof r;
        r.name   = n;
        r.hidden = n[0] == '.';
        struct stat st;
        /* Relative to the open directory: no joined path per entry. */
        if (fstatat(dirfd(d->d), n, &st, 0) == 0) {
            r.kind     = kindOf(st.st_mode);
            r.bytes    = S_ISREG(st.st_mode) ? (int64_t)st.st_size : 0;
            r.mtime_us = (int64_t)st.st_mtim.tv_sec * 1000000 + st.st_mtim.tv_nsec / 1000;
        }
        memcpy(out, &r, out->size < sizeof r ? out->size : sizeof r);
        return 1;
    }
}

void port_dir_close(port_dir *d) {
    if (!d)
        return;
    closedir(d->d);
    free(d);
}

/* ── Processes ─────────────────────────────────────────────────────────── */

struct port_proc {
    pid_t pid;
    int   in, out, err; /* our pipe ends, -1 when none */
    int   exited, code;
};

static void closeFd(int *fd) {
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static int nameIs(const char *kv, const char *other) {
    const char  *a = strchr(kv, '='), *b = strchr(other, '=');
    const size_t la = a ? (size_t)(a - kv) : strlen(kv),
                 lb = b ? (size_t)(b - other) : strlen(other);
    return la == lb && memcmp(kv, other, la) == 0;
}

/* Our environment with the overrides applied ("NAME=" unsets; last wins). */
static char **mergedEnv(const char *const *env, size_t envc) {
    size_t n = 0;
    while (environ[n])
        ++n;
    char **out = (char **)calloc(n + envc + 1, sizeof *out);
    if (!out)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        int overridden = 0;
        for (size_t j = 0; j < envc && !overridden; ++j)
            overridden = nameIs(environ[i], env[j]);
        if (!overridden)
            out[k++] = environ[i];
    }
    for (size_t j = 0; j < envc; ++j) {
        const char *eq = strchr(env[j], '=');
        if (!eq || eq == env[j] || !eq[1])
            continue;
        int later = 0;
        for (size_t l = j + 1; l < envc && !later; ++l)
            later = nameIs(env[j], env[l]);
        if (!later)
            out[k++] = (char *)env[j];
    }
    out[k] = NULL;
    return out;
}

int port_proc_spawn(const port_spawn_desc *d, port_proc **out) {
    if (d->flags & PORT_SPAWN_PTY)
        return PORT_E_NOSYS;
    int in[2] = {-1, -1}, o[2] = {-1, -1}, e[2] = {-1, -1};
    int rc = PORT_OK;
    if (((d->flags & PORT_SPAWN_STDIN_PIPE) && pipe2(in, O_CLOEXEC) != 0) ||
        ((d->flags & PORT_SPAWN_STDOUT_PIPE) && pipe2(o, O_CLOEXEC) != 0) ||
        ((d->flags & PORT_SPAWN_STDERR_PIPE) && pipe2(e, O_CLOEXEC) != 0))
        rc = ref_errno(errno);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in[0] >= 0)
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    else
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (o[1] >= 0)
        posix_spawn_file_actions_adddup2(&fa, o[1], 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (e[1] >= 0)
        posix_spawn_file_actions_adddup2(&fa, e[1], 2);
    else if ((d->flags & PORT_SPAWN_STDERR_MERGE) && o[1] >= 0)
        posix_spawn_file_actions_adddup2(&fa, o[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (d->cwd && *d->cwd)
        posix_spawn_file_actions_addchdir_np(&fa, d->cwd);
    /* A clean signal state, and a session of its own so the group can be
     * signalled as a whole. */
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    short sf = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (d->flags & PORT_SPAWN_NEW_GROUP)
        sf |= POSIX_SPAWN_SETSID;
    posix_spawnattr_setflags(&attr, sf);

    char **argv = (char **)calloc(d->argc + 2, sizeof *argv);
    char **envp = d->envc ? mergedEnv(d->env, d->envc) : environ;
    pid_t  pid  = 0;
    if (rc == PORT_OK && (!argv || !envp))
        rc = PORT_E_NOMEM;
    if (rc == PORT_OK) {
        argv[0] = (char *)d->exe;
        for (size_t i = 0; i < d->argc; ++i)
            argv[i + 1] = (char *)d->argv[i];
        const int err = (d->flags & PORT_SPAWN_SEARCH_PATH)
                            ? posix_spawnp(&pid, d->exe, &fa, &attr, argv, envp)
                            : posix_spawn(&pid, d->exe, &fa, &attr, argv, envp);
        rc            = err ? ref_errno(err) : PORT_OK;
    }
    free(argv);
    if (envp != environ)
        free(envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    closeFd(&in[0]);
    closeFd(&o[1]);
    closeFd(&e[1]);
    struct port_proc *p = rc == PORT_OK ? (struct port_proc *)calloc(1, sizeof *p) : NULL;
    if (!p) {
        closeFd(&in[1]);
        closeFd(&o[0]);
        closeFd(&e[0]);
        return rc == PORT_OK ? PORT_E_NOMEM : rc;
    }
    p->pid = pid;
    p->in  = in[1];
    p->out = o[0];
    p->err = e[0];
    *out   = p;
    return PORT_OK;
}

/* PORT_OK when fd is ready for `events` within timeout_ms. */
static int waitFd(int fd, short events, int32_t timeout_ms) {
    struct pollfd pfd = {fd, events, 0};
    for (;;) {
        const int n = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
        if (n > 0)
            return PORT_OK;
        if (n == 0)
            return timeout_ms == 0 ? PORT_E_AGAIN : PORT_E_TIMEOUT;
        if (errno != EINTR)
            return ref_errno(errno);
    }
}

int64_t port_proc_read(port_proc *p, int stream, void *buf, size_t n, int32_t timeout_ms) {
    const int fd = stream == PORT_STDERR ? p->err : p->out;
    if (fd < 0)
        return PORT_E_INVAL;
    const int w = waitFd(fd, POLLIN, timeout_ms);
    if (w != PORT_OK)
        return w == PORT_E_AGAIN ? PORT_E_TIMEOUT : w;
    for (;;) {
        const ssize_t r = read(fd, buf, n);
        if (r >= 0)
            return r;
        if (errno != EINTR)
            return ref_errno(errno);
    }
}

int64_t port_proc_write(port_proc *p, const void *buf, size_t n, int32_t timeout_ms) {
    if (p->in < 0)
        return PORT_E_CLOSED;
    const int w = waitFd(p->in, POLLOUT, timeout_ms);
    if (w != PORT_OK)
        return w == PORT_E_AGAIN ? PORT_E_TIMEOUT : w;
    /* MSG_NOSIGNAL has no pipe equivalent: block SIGPIPE on this thread
     * around the write and swallow one the write raised. */
    sigset_t pipeSet, old;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &old);
    /* Never more than PIPE_BUF at once when the pipe may be nearly full:
     * poll said writable, which guarantees that much without blocking. */
    const size_t chunk = n < 4096 ? n : 4096;
    ssize_t      r;
    do
        r = write(p->in, buf, chunk);
    while (r < 0 && errno == EINTR);
    const int err = r < 0 ? errno : 0;
    if (err == EPIPE) {
        const struct timespec zero = {0, 0};
        while (sigtimedwait(&pipeSet, NULL, &zero) == SIGPIPE) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return r >= 0 ? r : ref_errno(err);
}

int port_proc_close_stdin(port_proc *p) {
    closeFd(&p->in);
    return PORT_OK;
}

static int statusCode(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

int port_proc_wait(port_proc *p, int32_t timeout_ms, int *code) {
    const int64_t until = timeout_ms > 0 ? port_time_monotonic_ns() / 1000000 + timeout_ms : 0;
    for (int nap = 1;; nap = nap < 20 ? nap * 2 : 20) {
        if (!p->exited) {
            int         status = 0;
            const pid_t w      = waitpid(p->pid, &status, timeout_ms < 0 ? 0 : WNOHANG);
            if (w == p->pid) {
                p->exited = 1;
                p->code   = statusCode(status);
            } else if (w < 0 && errno == ECHILD) {
                p->exited = 1; /* reaped elsewhere: the status is gone */
                p->code   = -1;
            } else if (w < 0 && errno == EINTR) {
                continue;
            }
        }
        if (p->exited) {
            if (code)
                *code = p->code;
            return PORT_OK;
        }
        const int64_t left = until ? until - port_time_monotonic_ns() / 1000000 : 0;
        if (timeout_ms == 0 || (timeout_ms > 0 && left <= 0))
            return PORT_E_TIMEOUT;
        port_sleep_ms((uint32_t)(left < nap ? left : nap));
    }
}

int port_proc_kill(port_proc *p, int force) {
    if (p->exited)
        return PORT_OK;
    const int sig = force ? SIGKILL : SIGTERM;
    /* The group it leads, when it leads one; else the child alone. */
    if (getpgid(p->pid) == p->pid)
        kill(-p->pid, sig);
    else
        kill(p->pid, sig);
    return PORT_OK;
}

int64_t port_proc_pid(port_proc *p) {
    return p->pid;
}

int port_proc_resize(port_proc *p, uint16_t cols, uint16_t rows) {
    (void)p, (void)cols, (void)rows;
    return PORT_E_NOSYS;
}

void port_proc_release(port_proc *p) {
    if (!p)
        return;
    closeFd(&p->in);
    closeFd(&p->out);
    closeFd(&p->err);
    free(p);
}

/* ── Secrets: one 0600 file per key under the data folder ──────────────── */

static int secretPath(const char *key, char *path, size_t cap) {
    char      dir[4096];
    const int n = port_path_get(PORT_PATH_DATA, dir, sizeof dir);
    if (n < 0 || (size_t)n >= sizeof dir)
        return PORT_E_RANGE;
    size_t at = (size_t)snprintf(path, cap, "%s/secrets", dir);
    if (at >= cap)
        return PORT_E_RANGE;
    mkdir(path, 0700);
    if (at + 2 >= cap)
        return PORT_E_RANGE;
    path[at++] = '/';
    /* Hex: keys hold '/', and a file name must not. */
    for (const unsigned char *k = (const unsigned char *)key; *k; ++k) {
        if (at + 3 >= cap)
            return PORT_E_RANGE;
        static const char hex[] = "0123456789abcdef";
        path[at++]              = hex[*k >> 4];
        path[at++]              = hex[*k & 15];
    }
    path[at] = 0;
    return PORT_OK;
}

int port_secret_read(const char *key, void *buf, size_t cap, size_t *len) {
    char path[8192];
    int  rc = secretPath(key, path, sizeof path);
    if (rc != PORT_OK)
        return rc;
    port_file *f = NULL;
    if ((rc = port_file_open(path, PORT_O_READ, 0, &f)) != PORT_OK)
        return rc;
    const int64_t size = port_file_size(f);
    *len               = size > 0 ? (size_t)size : 0;
    if (size > 0 && (size_t)size > cap) {
        port_file_close(f);
        return PORT_E_RANGE;
    }
    const int64_t got = size > 0 ? port_file_pread(f, buf, (size_t)size, 0) : 0;
    port_file_close(f);
    return got == size ? PORT_OK : PORT_E_IO;
}

int port_secret_write(const char *key, const void *data, size_t len) {
    char path[8192], tmp[8300];
    int  rc = secretPath(key, path, sizeof path);
    if (rc != PORT_OK)
        return rc;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    port_file *f = NULL;
    if ((rc = port_file_open(tmp, PORT_O_WRITE | PORT_O_CREATE | PORT_O_TRUNC, 0600, &f)) !=
        PORT_OK)
        return rc;
    const int64_t put = port_file_write(f, data, len);
    rc                = put == (int64_t)len ? port_file_sync(f) : PORT_E_IO;
    if (port_file_close(f) != PORT_OK && rc == PORT_OK)
        rc = PORT_E_IO;
    if (rc == PORT_OK)
        rc = port_rename(tmp, path);
    if (rc != PORT_OK)
        unlink(tmp);
    return rc;
}

int port_secret_remove(const char *key) {
    char      path[8192];
    const int rc = secretPath(key, path, sizeof path);
    if (rc != PORT_OK)
        return rc;
    return unlink(path) == 0 || errno == ENOENT ? PORT_OK : ref_errno(errno);
}
