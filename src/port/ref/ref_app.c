/* port_app.h + port_test.h, headless: windows are memory, the loop is a
 * mutex and a condition variable, and input arrives only through the test
 * hooks. It behaves like plat's headless backend, so msga's suites pass on
 * it unchanged; a real port replaces all of this with its OS's surface and
 * event queue. */
#define _POSIX_C_SOURCE 200809L
#include "port/port_test.h"
#include "ref_internal.h"

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

static int64_t nowMs(void) {
    return port_time_monotonic_ns() / 1000000;
}

static char *dupStr(const char *s) {
    return ref_strndup(s ? s : "", s ? strlen(s) : 0);
}

/* A growable array of pointers. */
typedef struct ptr_list {
    void **items;
    size_t count, cap;
} ptr_list;

static int listPush(ptr_list *l, void *p) {
    if (l->count == l->cap) {
        const size_t cap   = l->cap ? l->cap * 2 : 8;
        void       **items = (void **)realloc(l->items, cap * sizeof *items);
        if (!items)
            return PORT_E_NOMEM;
        l->items = items;
        l->cap   = cap;
    }
    l->items[l->count++] = p;
    return PORT_OK;
}

static void listRemove(ptr_list *l, void *p) {
    for (size_t i = 0; i < l->count; ++i)
        if (l->items[i] == p) {
            memmove(l->items + i, l->items + i + 1, (l->count - i - 1) * sizeof *l->items);
            --l->count;
            return;
        }
}

static int listHas(const ptr_list *l, const void *p) {
    for (size_t i = 0; i < l->count; ++i)
        if (l->items[i] == p)
            return 1;
    return 0;
}

/* Owned copies of data items. */
typedef struct item_set {
    port_data_item *items;
    size_t          count;
} item_set;

static void itemsFree(item_set *s) {
    for (size_t i = 0; i < s->count; ++i) {
        free((char *)s->items[i].mime);
        free((char *)s->items[i].data);
    }
    free(s->items);
    s->items = NULL;
    s->count = 0;
}

static int itemsCopy(item_set *s, const port_data_item *items, size_t count) {
    itemsFree(s);
    if (!count)
        return PORT_OK;
    s->items = (port_data_item *)calloc(count, sizeof *s->items);
    if (!s->items)
        return PORT_E_NOMEM;
    for (size_t i = 0; i < count; ++i) {
        s->items[i].mime = dupStr(items[i].mime);
        s->items[i].data = ref_strndup(items[i].data ? items[i].data : "", items[i].len);
        s->items[i].len  = items[i].len;
    }
    s->count = count;
    return PORT_OK;
}

static int isTextMime(const char *m) {
    return !strncmp(m, "text/plain", 10);
}

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef struct timer {
    uint64_t id;
    int64_t  due;
    uint32_t interval;
    int      repeat;
    void (*fn)(void *);
    void *user;
} timer;

typedef struct notification {
    uint64_t id;
    char    *title, *body;
    int32_t  image_w, image_h;
    int      silent;
    size_t   action_count;
    char   **keys, **labels;
} notification;

struct port_window {
    port_app *app;
    void     *user;
    char     *title;
    int       visible, custom_decor;
    uint32_t  cursor;
    int       text_input;
    port_rect caret;
    uint32_t *pixels, *back; /* presented / being painted */
    int32_t   pw, ph;
    int       frames, frame_pending;
    uint32_t  last_hit, drop_reply;
    int       attention;
    int32_t   w, h, min_w, min_h;
    double    x, y, scale;
    int       maximized, fullscreen, active, above, minimized;
};

/* A watched directory: the watcher thread marks it, the loop calls it. */
typedef struct dir_watch {
    uint64_t   id;
    int        wd;
    atomic_int pending;
    void (*fn)(void *);
    void *user;
} dir_watch;

struct port_tray {
    port_app       *app;
    port_rect      *icon_sizes;
    size_t          icon_count;
    int             templ, icon_templ;
    char           *tooltip;
    port_menu_item *menu;
    size_t          menu_count;
    int             attention;
};

struct port_app {
    port_app_callbacks cb;
    pthread_mutex_t    mutex; /* guards woken/quit across threads */
    pthread_cond_t     cond;
    int                woken, quit;

    timer   *timers;
    size_t   timer_count, timer_cap;
    uint64_t next_timer;

    ptr_list     windows, trays;
    port_window *focus, *hover;
    item_set     clipboard[2];

    notification *notes;
    size_t        note_count;
    uint64_t      max_note_id;
    int           badge;
    int           online;
    char         *last_url;
    ptr_list      claimed, schemes;

    /* The fake file dialog. */
    int    dialog_available, has_answer, has_last;
    char **answer;
    size_t answer_count;
    void (*pending_cb)(void *, int, const char *const *, size_t);
    void            *pending_user;
    port_file_dialog last_dialog; /* strings owned */

    /* Synthesised input. */
    uint32_t      mods;
    unsigned char down[PORT_KEY_COUNT];
    double        px, py;
    int           button_held;
    int           click_button, clicks;
    int64_t       click_time;
    /* An in-process drag. */
    int           dragging;
    item_set      drag_items;
    uint32_t      drag_actions;
    port_window  *drag_source, *drop_target;

    /* Directory watches: an inotify fd read by a thread of its own, which
     * marks the watches and wakes the loop. */
    pthread_mutex_t watch_mutex;
    ptr_list        watches; /* dir_watch * */
    uint64_t        next_watch;
    int             inotify_fd, watch_pipe[2];
    pthread_t       watch_thread;
    int             watch_thread_on;
};

static port_app *g_app; /* one per process */

static void emit(port_app *a, port_event *e) {
    e->size = sizeof *e;
    if (a->cb.on_event)
        a->cb.on_event(a->cb.user, e);
}

static void emitType(port_app *a, port_window *w, uint32_t type) {
    port_event e;
    memset(&e, 0, sizeof e);
    e.type   = type;
    e.window = w;
    emit(a, &e);
}

/* ── The loop ──────────────────────────────────────────────────────────── */

int port_app_create(const port_app_desc *d, port_app **out, char *err, size_t cap) {
    if (g_app) {
        ref_detail(err, cap, "one app per process");
        return PORT_E_BUSY;
    }
    port_app *a = (port_app *)calloc(1, sizeof *a);
    if (!a)
        return PORT_E_NOMEM;
    memcpy(
        &a->cb, &d->callbacks, d->callbacks.size < sizeof a->cb ? d->callbacks.size : sizeof a->cb
    );
    pthread_mutex_init(&a->mutex, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&a->cond, &attr);
    pthread_condattr_destroy(&attr);
    a->next_timer       = 1;
    a->online           = 1;
    a->dialog_available = 1;
    a->next_watch       = 1;
    a->inotify_fd       = -1;
    a->watch_pipe[0] = a->watch_pipe[1] = -1;
    pthread_mutex_init(&a->watch_mutex, NULL);
    g_app = a;
    *out  = a;
    return PORT_OK;
}

static void freeNote(notification *n) {
    free(n->title);
    free(n->body);
    for (size_t i = 0; i < n->action_count; ++i) {
        free(n->keys[i]);
        free(n->labels[i]);
    }
    free(n->keys);
    free(n->labels);
}

static void freeStrings(char **s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        free(s[i]);
    free(s);
}

static void freeLastDialog(port_app *a) {
    port_file_dialog *d = &a->last_dialog;
    free((char *)d->title);
    free((char *)d->initial_dir);
    free((char *)d->suggested_name);
    for (size_t i = 0; i < d->filter_count; ++i) {
        free((char *)d->filters[i].name);
        freeStrings((char **)d->filters[i].patterns, d->filters[i].pattern_count);
    }
    free((port_file_filter *)d->filters);
    memset(d, 0, sizeof *d);
}

static void stopWatching(port_app *a);
static void runWatches(port_app *a);

void port_app_destroy(port_app *a) {
    if (!a)
        return;
    stopWatching(a);
    free(a->timers);
    free(a->windows.items);
    free(a->trays.items);
    itemsFree(&a->clipboard[0]);
    itemsFree(&a->clipboard[1]);
    itemsFree(&a->drag_items);
    for (size_t i = 0; i < a->note_count; ++i)
        freeNote(&a->notes[i]);
    free(a->notes);
    free(a->last_url);
    freeStrings((char **)a->claimed.items, a->claimed.count);
    freeStrings((char **)a->schemes.items, a->schemes.count);
    freeStrings(a->answer, a->answer_count);
    freeLastDialog(a);
    pthread_cond_destroy(&a->cond);
    pthread_mutex_destroy(&a->mutex);
    if (g_app == a)
        g_app = NULL;
    free(a);
}

void port_app_wake(port_app *a) {
    pthread_mutex_lock(&a->mutex);
    a->woken = 1;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mutex);
}

void port_app_quit(port_app *a) {
    pthread_mutex_lock(&a->mutex);
    a->quit = 1;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mutex);
}

uint64_t
port_timer_add(port_app *a, uint32_t interval_ms, int repeat, void (*fn)(void *), void *user) {
    if (a->timer_count == a->timer_cap) {
        const size_t cap = a->timer_cap ? a->timer_cap * 2 : 16;
        timer       *t   = (timer *)realloc(a->timers, cap * sizeof *t);
        if (!t)
            return 0;
        a->timers    = t;
        a->timer_cap = cap;
    }
    timer *t    = &a->timers[a->timer_count++];
    t->id       = a->next_timer++;
    t->due      = nowMs() + interval_ms;
    t->interval = interval_ms;
    t->repeat   = repeat;
    t->fn       = fn;
    t->user     = user;
    return t->id;
}

void port_timer_cancel(port_app *a, uint64_t id) {
    for (size_t i = 0; i < a->timer_count; ++i)
        if (a->timers[i].id == id) {
            memmove(a->timers + i, a->timers + i + 1, (a->timer_count - i - 1) * sizeof *a->timers);
            --a->timer_count;
            return;
        }
}

static void runDueTimers(port_app *a) {
    /* Only timers created before this turn; each at most once per turn. A
     * callback may add or cancel timers, so look each id up again. */
    const uint64_t limit = a->next_timer;
    const int64_t  now   = nowMs();
    for (uint64_t id = 0;;) {
        size_t best = (size_t)-1;
        for (size_t i = 0; i < a->timer_count; ++i)
            if (a->timers[i].id > id && a->timers[i].id < limit && a->timers[i].due <= now &&
                (best == (size_t)-1 || a->timers[i].id < a->timers[best].id))
                best = i;
        if (best == (size_t)-1)
            return;
        timer t = a->timers[best];
        id      = t.id;
        if (t.repeat)
            a->timers[best].due = now + (t.interval ? t.interval : 1);
        else
            port_timer_cancel(a, t.id);
        t.fn(t.user);
    }
}

static int64_t msUntilTimer(port_app *a) {
    int64_t       best = -1;
    const int64_t now  = nowMs();
    for (size_t i = 0; i < a->timer_count; ++i) {
        const int64_t left = a->timers[i].due > now ? a->timers[i].due - now : 0;
        if (best < 0 || left < best)
            best = left;
    }
    return best;
}

void port_app_pump(port_app *a, int32_t timeout_ms) {
    /* Frames are "vsynced" at the end of every turn: each window that asked
     * gets exactly one FRAME, like a compositor's frame callback. */
    int frameDue = 0;
    for (size_t i = 0; i < a->windows.count && !frameDue; ++i) {
        const port_window *w = (const port_window *)a->windows.items[i];
        frameDue             = w->frame_pending && w->visible;
    }
    int64_t       timeout = frameDue ? 0 : timeout_ms;
    const int64_t t       = msUntilTimer(a);
    if (t >= 0 && (timeout < 0 || t < timeout))
        timeout = t;

    pthread_mutex_lock(&a->mutex);
    if (timeout != 0 && !a->woken && !a->quit) {
        if (timeout < 0) {
            while (!a->woken && !a->quit)
                pthread_cond_wait(&a->cond, &a->mutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += timeout / 1000;
            ts.tv_nsec += (long)(timeout % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L)
                ts.tv_sec += 1, ts.tv_nsec -= 1000000000L;
            while (!a->woken && !a->quit && pthread_cond_timedwait(&a->cond, &a->mutex, &ts) == 0) {
            }
        }
    }
    const int woken = a->woken;
    a->woken        = 0;
    pthread_mutex_unlock(&a->mutex);

    if (woken && a->cb.on_wake)
        a->cb.on_wake(a->cb.user);
    runDueTimers(a);
    runWatches(a);
    /* A snapshot: a FRAME handler may create or destroy windows. */
    const size_t  n    = a->windows.count;
    port_window **snap = n ? (port_window **)malloc(n * sizeof *snap) : NULL;
    if (snap)
        memcpy(snap, a->windows.items, n * sizeof *snap);
    for (size_t i = 0; snap && i < n; ++i) {
        port_window *w = snap[i];
        if (listHas(&a->windows, w) && w->frame_pending && w->visible) {
            w->frame_pending = 0;
            emitType(a, w, PORT_EV_FRAME);
        }
    }
    free(snap);
}

/* ── Directory watches ───────────────────────────────────────────────── */

#ifdef __linux__
static void *watchThread(void *p) {
    port_app                           *a = (port_app *)p;
    _Alignas(struct inotify_event) char buf[4096];
    for (;;) {
        struct pollfd fds[2] = {{a->inotify_fd, POLLIN, 0}, {a->watch_pipe[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0)
            continue;
        if (fds[1].revents)
            return NULL; /* stopWatching */
        const ssize_t n   = read(a->inotify_fd, buf, sizeof buf);
        int           any = 0;
        pthread_mutex_lock(&a->watch_mutex);
        for (ssize_t at = 0; n > 0 && at < n;) {
            const struct inotify_event *e = (const struct inotify_event *)(buf + at);
            for (size_t i = 0; i < a->watches.count; ++i) {
                dir_watch *w = (dir_watch *)a->watches.items[i];
                if (w->wd == e->wd) {
                    atomic_store(&w->pending, 1);
                    any = 1;
                }
            }
            at += (ssize_t)sizeof *e + e->len;
        }
        pthread_mutex_unlock(&a->watch_mutex);
        if (any)
            port_app_wake(a);
    }
}
#endif

uint64_t port_watch_dir(port_app *a, const char *path, void (*fn)(void *), void *user) {
#ifdef __linux__
    if (a->inotify_fd < 0) {
        a->inotify_fd = inotify_init1(IN_CLOEXEC);
        if (a->inotify_fd < 0)
            return 0;
        if (pipe(a->watch_pipe) != 0 ||
            pthread_create(&a->watch_thread, NULL, watchThread, a) != 0) {
            close(a->inotify_fd);
            a->inotify_fd = -1;
            return 0;
        }
        a->watch_thread_on = 1;
    }
    const int wd = inotify_add_watch(
        a->inotify_fd,
        path,
        IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB
    );
    dir_watch *w = wd >= 0 ? (dir_watch *)calloc(1, sizeof *w) : NULL;
    if (!w)
        return 0;
    w->id   = a->next_watch++;
    w->wd   = wd;
    w->fn   = fn;
    w->user = user;
    atomic_init(&w->pending, 0);
    pthread_mutex_lock(&a->watch_mutex);
    const int rc = listPush(&a->watches, w);
    pthread_mutex_unlock(&a->watch_mutex);
    if (rc != PORT_OK) {
        free(w);
        return 0;
    }
    return w->id;
#else
    (void)a, (void)path, (void)fn, (void)user;
    return 0;
#endif
}

void port_unwatch_dir(port_app *a, uint64_t id) {
    dir_watch *gone = NULL;
    pthread_mutex_lock(&a->watch_mutex);
    for (size_t i = 0; i < a->watches.count && !gone; ++i)
        if (((dir_watch *)a->watches.items[i])->id == id)
            gone = (dir_watch *)a->watches.items[i];
    if (gone)
        listRemove(&a->watches, gone);
    /* Another watch may share the descriptor (the same directory twice). */
    int shared = 0;
    for (size_t i = 0; gone && i < a->watches.count; ++i)
        shared = shared || ((dir_watch *)a->watches.items[i])->wd == gone->wd;
    pthread_mutex_unlock(&a->watch_mutex);
#ifdef __linux__
    if (gone && !shared)
        inotify_rm_watch(a->inotify_fd, gone->wd);
#endif
    free(gone);
}

static void runWatches(port_app *a) {
    /* By id, looked up again each time: a callback may unwatch others. */
    for (uint64_t id = 0;;) {
        void (*fn)(void *) = NULL;
        void *user         = NULL;
        pthread_mutex_lock(&a->watch_mutex);
        for (size_t i = 0; i < a->watches.count && !fn; ++i) {
            dir_watch *w = (dir_watch *)a->watches.items[i];
            if (w->id > id && atomic_exchange(&w->pending, 0)) {
                id   = w->id;
                fn   = w->fn;
                user = w->user;
            }
        }
        pthread_mutex_unlock(&a->watch_mutex);
        if (!fn)
            return;
        fn(user);
    }
}

static void stopWatching(port_app *a) {
    if (a->watch_thread_on) {
        const char b = 1;
        if (write(a->watch_pipe[1], &b, 1) == 1)
            pthread_join(a->watch_thread, NULL);
        close(a->watch_pipe[0]);
        close(a->watch_pipe[1]);
        close(a->inotify_fd);
        a->watch_thread_on = 0;
    }
    for (size_t i = 0; i < a->watches.count; ++i)
        free(a->watches.items[i]);
    free(a->watches.items);
    a->watches.items = NULL;
    a->watches.count = 0;
    pthread_mutex_destroy(&a->watch_mutex);
}

void port_app_run(port_app *a) {
    pthread_mutex_lock(&a->mutex);
    a->quit = 0;
    pthread_mutex_unlock(&a->mutex);
    for (;;) {
        pthread_mutex_lock(&a->mutex);
        const int quit = a->quit;
        pthread_mutex_unlock(&a->mutex);
        if (quit)
            return;
        port_app_pump(a, -1);
    }
}

/* Runs fn(arg) on a later turn: callbacks never run inside their call. */
static void later(port_app *a, void (*fn)(void *), void *arg) {
    port_timer_add(a, 0, 0, fn, arg);
}

/* ── Windows ───────────────────────────────────────────────────────────── */

/* Single-surface mode (ref_single_surface): every window is the surface's
 * size at the origin, draws its own decorations, and ignores size, position
 * and stacking requests; the last one shown is attached (visible), the one
 * before it is detached (hidden), as a port without PORT_CAP_WINDOWS does. */

int port_window_create(port_app *a, const port_window_desc *d, port_window **out) {
    port_window *w = (port_window *)calloc(1, sizeof *w);
    if (!w || listPush(&a->windows, w) != PORT_OK) {
        free(w);
        return PORT_E_NOMEM;
    }
    w->app          = a;
    w->user         = d->user;
    w->title        = dupStr(d->title);
    w->w            = d->width;
    w->h            = d->height;
    w->min_w        = d->min_width;
    w->min_h        = d->min_height;
    w->scale        = 1.0;
    w->custom_decor = (d->flags & PORT_WINDOW_CUSTOM_DECOR) != 0;
    w->drop_reply   = PORT_DROP_COPY;
    if (d->flags & PORT_WINDOW_HAS_POSITION)
        w->x = d->x, w->y = d->y;
    int32_t sw = 0, sh = 0;
    if (ref_single_surface(&sw, &sh)) {
        if (sw > 0) /* else "auto": the surface is what the window asked for */
            w->w = sw, w->h = sh;
        w->x = w->y     = 0;
        w->custom_decor = 1;
    }
    *out = w;
    if (d->flags & PORT_WINDOW_VISIBLE)
        port_window_show(w);
    return PORT_OK;
}

void port_window_destroy(port_window *w) {
    if (!w)
        return;
    port_app *a = w->app;
    listRemove(&a->windows, w);
    if (a->focus == w)
        a->focus = NULL;
    if (a->hover == w)
        a->hover = NULL;
    if (a->drop_target == w)
        a->drop_target = NULL;
    if (a->drag_source == w) {
        a->drag_source = NULL;
        a->dragging    = 0;
    }
    free(w->title);
    free(w->pixels);
    free(w->back);
    free(w);
}

void *port_window_user(port_window *w) {
    return w->user;
}

void port_window_set_title(port_window *w, const char *title) {
    free(w->title);
    w->title = dupStr(title);
}

void port_window_size(port_window *w, int32_t *width, int32_t *height) {
    *width  = w->w;
    *height = w->h;
}

double port_window_scale(port_window *w) {
    return w->scale;
}

void port_window_set_size(port_window *w, int32_t width, int32_t height) {
    if (ref_single_surface(NULL, NULL))
        return; /* the surface's size is the OS's */
    width  = width < w->min_w ? w->min_w : width;
    height = height < w->min_h ? w->min_h : height;
    if (width == w->w && height == w->h)
        return;
    w->w = width;
    w->h = height;
    emitType(w->app, w, PORT_EV_RESIZED);
    port_window_request_frame(w);
}

void port_window_set_min_size(port_window *w, int32_t width, int32_t height) {
    w->min_w = width;
    w->min_h = height;
}

void port_window_show(port_window *w) {
    port_app *a = w->app;
    if (ref_single_surface(NULL, NULL))
        for (size_t i = 0; i < a->windows.count; ++i) {
            port_window *o = (port_window *)a->windows.items[i];
            if (o != w && o->visible) { /* detached */
                o->visible = 0;
                emitType(a, o, PORT_EV_STATE_CHANGED);
            }
        }
    w->visible = 1;
    port_window_request_frame(w);
}

void port_window_hide(port_window *w) {
    w->visible = 0;
}

void port_window_minimize(port_window *w) {
    if (ref_single_surface(NULL, NULL))
        return;
    w->minimized = 1;
    emitType(w->app, w, PORT_EV_STATE_CHANGED);
}

void port_window_set_maximized(port_window *w, int on) {
    if (ref_single_surface(NULL, NULL))
        return;
    w->maximized = on;
    emitType(w->app, w, PORT_EV_STATE_CHANGED);
}

void port_window_set_fullscreen(port_window *w, int on) {
    if (ref_single_surface(NULL, NULL))
        return;
    w->fullscreen = on;
    emitType(w->app, w, PORT_EV_STATE_CHANGED);
}

void port_window_set_always_on_top(port_window *w, int on) {
    if (ref_single_surface(NULL, NULL))
        return;
    w->above = on;
    emitType(w->app, w, PORT_EV_STATE_CHANGED);
}

static void focus(port_app *a, port_window *w) {
    if (a->focus == w)
        return;
    if (a->focus)
        emitType(a, a->focus, PORT_EV_FOCUS_OUT);
    a->focus = w;
    emitType(a, w, PORT_EV_FOCUS_IN);
}

void port_window_activate(port_window *w, const char *token) {
    (void)token;
    w->active    = 1;
    w->attention = 0;
    focus(w->app, w);
    if (w->minimized) { /* activating restores, as window managers do */
        w->minimized = 0;
        emitType(w->app, w, PORT_EV_STATE_CHANGED);
    }
}

uint32_t port_window_state(port_window *w) {
    return (w->visible ? PORT_WSTATE_VISIBLE : 0) | (w->active ? PORT_WSTATE_ACTIVE : 0) |
           (w->maximized ? PORT_WSTATE_MAXIMIZED : 0) | (w->minimized ? PORT_WSTATE_MINIMIZED : 0) |
           (w->fullscreen ? PORT_WSTATE_FULLSCREEN : 0) | (w->above ? PORT_WSTATE_ABOVE : 0) |
           (w->custom_decor ? 0 : PORT_WSTATE_SYSTEM_DECOR);
}

int port_window_position(port_window *w, double *x, double *y) {
    if (ref_single_surface(NULL, NULL))
        return PORT_E_NOSYS;
    *x = w->x;
    *y = w->y;
    return PORT_OK;
}

int port_window_set_position(port_window *w, double x, double y) {
    if (ref_single_surface(NULL, NULL))
        return PORT_E_NOSYS;
    w->x = x;
    w->y = y;
    emitType(w->app, w, PORT_EV_MOVED);
    return PORT_OK;
}

/* Two fake monitors side by side, the second HiDPI. */
static const port_monitor kMonitors[2] = {
    {sizeof(port_monitor), 1, "HEADLESS-1", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}, 1.0, 60000, 1},
    {sizeof(port_monitor),
     2,
     "HEADLESS-2",
     {1920, 0, 1280, 720},
     {1920, 0, 1280, 720},
     1.5,
     60000,
     0},
};

uint64_t port_window_monitor(port_window *w) {
    const double cx = w->x + w->w / 2.0, cy = w->y + w->h / 2.0;
    for (int i = 0; i < 2; ++i) {
        const port_rect *b = &kMonitors[i].bounds;
        if (cx >= b->x && cx < b->x + b->w && cy >= b->y && cy < b->y + b->h)
            return kMonitors[i].id;
    }
    return 1;
}

void port_window_set_dark_chrome(port_window *w, int dark) {
    (void)w, (void)dark;
}

double port_window_title_bar_height(port_window *w) {
    (void)w;
    return 0;
}

void port_window_set_cursor(port_window *w, uint32_t cursor) {
    w->cursor = cursor;
}

void port_window_set_text_input(port_window *w, int enabled, port_rect caret) {
    w->text_input = enabled;
    w->caret      = caret;
}

void port_window_request_attention(port_window *w) {
    if (!w->active)
        w->attention = 1;
}

void port_window_set_drop_action(port_window *w, uint32_t action) {
    w->drop_reply = action;
}

void *port_window_native(port_window *w) {
    return w;
}

void port_window_request_frame(port_window *w) {
    w->frame_pending = 1;
    port_app_wake(w->app);
}

int port_window_begin_paint(port_window *w, port_canvas *out) {
    const int32_t pw = (int32_t)(w->w * w->scale + 0.5), ph = (int32_t)(w->h * w->scale + 0.5);
    if (pw != w->pw || ph != w->ph || !w->back) {
        free(w->back);
        w->back = (uint32_t *)calloc((size_t)(pw > 0 ? pw : 1) * (size_t)(ph > 0 ? ph : 1), 4);
        if (!w->back)
            return PORT_E_NOMEM;
        w->pw = pw;
        w->ph = ph;
    }
    port_canvas c = {sizeof c, w->back, pw, ph, pw, w->scale};
    memcpy(out, &c, out->size < sizeof c ? out->size : sizeof c);
    return PORT_OK;
}

void port_window_end_paint(port_window *w, const port_rect *damage, size_t count) {
    (void)damage, (void)count;
    const size_t n = (size_t)w->pw * (size_t)w->ph;
    if (!w->pixels || n == 0) {
        free(w->pixels);
        w->pixels = (uint32_t *)malloc((n ? n : 1) * 4);
    }
    /* The presented frame is the whole back buffer (sizes may have changed
     * since the last present: reallocate to match). */
    uint32_t *p = (uint32_t *)realloc(w->pixels, (n ? n : 1) * 4);
    if (p) {
        w->pixels = p;
        memcpy(w->pixels, w->back, n * 4);
    }
    ++w->frames;
}

/* ── Clipboard ─────────────────────────────────────────────────────────── */

int port_clipboard_set(port_app *a, int sel, const port_data_item *items, size_t count) {
    return itemsCopy(&a->clipboard[sel ? 1 : 0], items, count);
}

typedef struct clip_req {
    void (*cb)(void *, const char *, size_t);
    void (*types_cb)(void *, const char *const *, size_t);
    void    *user;
    char    *data; /* NULL = not there */
    size_t   len;
    item_set types;
} clip_req;

static void clipDeliver(void *p) {
    clip_req *r = (clip_req *)p;
    if (r->cb) {
        r->cb(r->user, r->data, r->len);
    } else {
        const char **m = (const char **)calloc(r->types.count + 1, sizeof *m);
        for (size_t i = 0; m && i < r->types.count; ++i)
            m[i] = r->types.items[i].mime;
        r->types_cb(r->user, m, m ? r->types.count : 0);
        free(m);
    }
    free(r->data);
    itemsFree(&r->types);
    free(r);
}

void port_clipboard_get(
    port_app *a, int sel, const char *mime, void (*cb)(void *, const char *, size_t), void *user
) {
    clip_req *r = (clip_req *)calloc(1, sizeof *r);
    if (!r)
        return;
    r->cb               = cb;
    r->user             = user;
    const item_set *set = &a->clipboard[sel ? 1 : 0];
    for (size_t i = 0; i < set->count; ++i) {
        const port_data_item *it = &set->items[i];
        if (!strcmp(it->mime, mime) || (isTextMime(it->mime) && isTextMime(mime))) {
            free(r->data);
            r->data = ref_strndup(it->data, it->len);
            r->len  = it->len;
        }
    }
    later(a, clipDeliver, r);
}

void port_clipboard_types(
    port_app *a, int sel, void (*cb)(void *, const char *const *, size_t), void *user
) {
    clip_req *r = (clip_req *)calloc(1, sizeof *r);
    if (!r)
        return;
    r->types_cb = cb;
    r->user     = user;
    itemsCopy(&r->types, a->clipboard[sel ? 1 : 0].items, a->clipboard[sel ? 1 : 0].count);
    later(a, clipDeliver, r);
}

/* ── Drag and drop, in process ─────────────────────────────────────────── */

int port_drag_start(port_app *a, port_window *source, const port_drag_desc *d) {
    if (!a->button_held || a->dragging)
        return PORT_E_BUSY;
    if (itemsCopy(&a->drag_items, d->items, d->item_count) != PORT_OK)
        return PORT_E_NOMEM;
    a->dragging     = 1;
    a->drag_actions = d->actions;
    a->drag_source  = source;
    a->drop_target  = NULL;
    return PORT_OK;
}

static uint32_t preferredAction(uint32_t allowed) {
    return (allowed & PORT_DROP_ALLOW_COPY)   ? PORT_DROP_COPY
           : (allowed & PORT_DROP_ALLOW_MOVE) ? PORT_DROP_MOVE
           : (allowed & PORT_DROP_ALLOW_LINK) ? PORT_DROP_LINK
                                              : PORT_DROP_NONE;
}

static void dropEvent(port_app *a, port_window *w, uint32_t type, int withData) {
    port_event e;
    memset(&e, 0, sizeof e);
    e.type                   = type;
    e.window                 = w;
    e.x                      = a->px;
    e.y                      = a->py;
    e.drop_action            = preferredAction(a->drag_actions);
    e.allowed_actions        = a->drag_actions;
    const size_t    n        = a->drag_items.count;
    port_data_item *items    = (port_data_item *)calloc(n ? n : 1, sizeof *items);
    const char    **uris     = NULL;
    size_t          uriCount = 0;
    char           *uriText  = NULL;
    for (size_t i = 0; items && i < n; ++i) {
        const port_data_item *it = &a->drag_items.items[i];
        items[i].mime            = it->mime;
        items[i].data            = withData ? it->data : "";
        items[i].len             = withData ? it->len : 0;
        if (!withData)
            continue;
        if (!strcmp(it->mime, "text/uri-list") && !uriText) {
            /* One URI per line; '#' lines are comments. */
            uriText = ref_strndup(it->data, it->len);
            uris    = (const char **)calloc(it->len + 1, sizeof *uris);
            for (char *line = uriText; uris && line && *line;) {
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                const size_t ll = strlen(line);
                if (ll && line[ll - 1] == '\r')
                    line[ll - 1] = 0;
                if (*line && *line != '#')
                    uris[uriCount++] = line;
                line = nl ? nl + 1 : NULL;
            }
        } else if (isTextMime(it->mime)) {
            e.text     = it->data;
            e.text_len = it->len;
        }
    }
    e.items        = items;
    e.item_count   = items ? n : 0;
    e.strings      = uris;
    e.string_count = uriCount;
    emit(a, &e);
    free(items);
    free(uris);
    free(uriText);
}

static void dragOver(port_app *a, port_window *w) {
    if (w != a->drop_target) {
        if (a->drop_target)
            emitType(a, a->drop_target, PORT_EV_DROP_LEAVE);
        a->drop_target = w;
        w->drop_reply  = preferredAction(a->drag_actions);
        dropEvent(a, w, PORT_EV_DROP_ENTER, 0);
    } else {
        dropEvent(a, w, PORT_EV_DROP_MOVE, 0);
    }
}

static void finishDrag(port_app *a) {
    uint32_t     result = PORT_DROP_NONE;
    port_window *target = a->drop_target;
    if (target && target->drop_reply != PORT_DROP_NONE) {
        result = target->drop_reply;
        dropEvent(a, target, PORT_EV_DROP, 1);
    } else if (target) {
        emitType(a, target, PORT_EV_DROP_LEAVE);
    }
    port_window *src = a->drag_source;
    a->dragging      = 0;
    a->drag_source = a->drop_target = NULL;
    itemsFree(&a->drag_items);
    if (src) {
        port_event e;
        memset(&e, 0, sizeof e);
        e.type        = PORT_EV_DRAG_FINISHED;
        e.window      = src;
        e.drop_action = result;
        emit(a, &e);
    }
}

/* ── Tray ──────────────────────────────────────────────────────────────── */

static void freeMenu(port_tray *t) {
    for (size_t i = 0; i < t->menu_count; ++i)
        free((char *)t->menu[i].label);
    free(t->menu);
    t->menu       = NULL;
    t->menu_count = 0;
}

int port_tray_create(port_app *a, port_tray **out) {
    port_tray *t = (port_tray *)calloc(1, sizeof *t);
    if (!t || listPush(&a->trays, t) != PORT_OK) {
        free(t);
        return PORT_E_NOMEM;
    }
    t->app     = a;
    t->tooltip = dupStr("");
    *out       = t;
    return PORT_OK;
}

void port_tray_destroy(port_tray *t) {
    if (!t)
        return;
    listRemove(&t->app->trays, t);
    freeMenu(t);
    free(t->icon_sizes);
    free(t->tooltip);
    free(t);
}

void port_tray_set_icon(port_tray *t, const port_image *sizes, size_t count, int template_image) {
    free(t->icon_sizes);
    t->icon_sizes = (port_rect *)calloc(count ? count : 1, sizeof *t->icon_sizes);
    t->icon_count = t->icon_sizes ? count : 0;
    for (size_t i = 0; i < t->icon_count; ++i)
        t->icon_sizes[i] = (port_rect){0, 0, sizes[i].width, sizes[i].height};
    t->icon_templ = template_image;
}

void port_tray_set_tooltip(port_tray *t, const char *text) {
    free(t->tooltip);
    t->tooltip = dupStr(text);
}

void port_tray_set_menu(port_tray *t, const port_menu_item *items, size_t count) {
    freeMenu(t);
    t->menu = (port_menu_item *)calloc(count ? count : 1, sizeof *t->menu);
    if (!t->menu)
        return;
    for (size_t i = 0; i < count; ++i) {
        t->menu[i]       = items[i];
        t->menu[i].label = dupStr(items[i].label);
    }
    t->menu_count = count;
}

void port_tray_set_attention(port_tray *t, int on) {
    t->attention = on;
}

int port_tray_visible(port_tray *t) {
    (void)t;
    return 1;
}

/* ── Notifications, badge, desktop services ────────────────────────────── */

static notification *findNote(port_app *a, uint64_t id) {
    for (size_t i = 0; i < a->note_count; ++i)
        if (a->notes[i].id == id)
            return &a->notes[i];
    return NULL;
}

void port_notify_withdraw(port_app *a, uint64_t id) {
    notification *n = findNote(a, id);
    if (!n)
        return;
    freeNote(n);
    *n = a->notes[--a->note_count];
}

int port_notify(port_app *a, uint64_t id, const port_notification *src) {
    port_notification in;
    memset(&in, 0, sizeof in);
    memcpy(&in, src, src->size < sizeof in ? src->size : sizeof in);
    port_notify_withdraw(a, id); /* the same id replaces */
    notification *grown = (notification *)realloc(a->notes, (a->note_count + 1) * sizeof *grown);
    if (!grown)
        return PORT_E_NOMEM;
    a->notes        = grown;
    notification *n = &a->notes[a->note_count++];
    memset(n, 0, sizeof *n);
    n->id      = id;
    n->title   = dupStr(in.title);
    n->body    = dupStr(in.body);
    n->image_w = in.image ? in.image->width : 0;
    n->image_h = in.image ? in.image->height : 0;
    n->silent  = in.silent;
    n->keys    = (char **)calloc(in.action_count + 1, sizeof *n->keys);
    n->labels  = (char **)calloc(in.action_count + 1, sizeof *n->labels);
    for (size_t i = 0; n->keys && n->labels && i < in.action_count; ++i) {
        n->keys[i]   = dupStr(in.actions[i].key);
        n->labels[i] = dupStr(in.actions[i].label);
        n->action_count++;
    }
    if (id > a->max_note_id)
        a->max_note_id = id;
    return PORT_OK;
}

int port_launched_from_notification(port_app *a) {
    (void)a;
    return 0;
}

void port_set_badge(port_app *a, int count) {
    a->badge = count;
}

int port_monitors(port_app *a, port_monitor *out, size_t cap) {
    (void)a;
    int32_t sw, sh;
    if (ref_single_surface(&sw, &sh)) { /* the surface is the one monitor */
        if (sw <= 0)
            sw = (int32_t)kMonitors[0].bounds.w, sh = (int32_t)kMonitors[0].bounds.h;
        if (cap > 0) {
            port_monitor m = kMonitors[0];
            m.bounds.w = m.work_area.w = sw;
            m.bounds.h = m.work_area.h = sh;
            const uint32_t size        = out[0].size;
            memcpy(&out[0], &m, size < sizeof m ? size : sizeof m);
            out[0].size = size;
        }
        return 1;
    }
    for (size_t i = 0; i < cap && i < 2; ++i) {
        const uint32_t size = out[i].size;
        memcpy(&out[i], &kMonitors[i], size < sizeof kMonitors[i] ? size : sizeof kMonitors[i]);
        out[i].size = size;
    }
    return 2;
}

/* In-process only: a second process can't find a headless app. */
int port_instance_claim(port_app *a, const char *key, const char *const *args, size_t argc) {
    (void)args, (void)argc;
    for (size_t i = 0; i < a->claimed.count; ++i)
        if (!strcmp((const char *)a->claimed.items[i], key))
            return 0;
    listPush(&a->claimed, dupStr(key));
    return 1;
}

int port_url_scheme_register(port_app *a, const char *scheme) {
    return listPush(&a->schemes, dupStr(scheme));
}

int port_network_online(port_app *a) {
    return a->online;
}

int port_open_url(port_app *a, const char *url) {
    free(a->last_url);
    a->last_url = dupStr(url);
    return PORT_OK;
}

typedef struct dialog_answer {
    void (*cb)(void *, int, const char *const *, size_t);
    void  *user;
    int    status;
    char **paths;
    size_t count;
} dialog_answer;

static void dialogDeliver(void *p) {
    dialog_answer *d = (dialog_answer *)p;
    d->cb(d->user, d->status, (const char *const *)d->paths, d->count);
    freeStrings(d->paths, d->count);
    free(d);
}

static void dialogAnswer(
    port_app *a,
    void (*cb)(void *, int, const char *const *, size_t),
    void  *user,
    int    status,
    char **paths,
    size_t count
) {
    dialog_answer *d = (dialog_answer *)calloc(1, sizeof *d);
    if (!d) {
        freeStrings(paths, count);
        return;
    }
    d->cb     = cb;
    d->user   = user;
    d->status = status == PORT_DIALOG_CHOSEN && count == 0 ? PORT_DIALOG_CANCELLED : status;
    d->paths  = paths;
    d->count  = count;
    later(a, dialogDeliver, d);
}

static char **copyStrings(const char *const *s, size_t n) {
    char **out = (char **)calloc(n + 1, sizeof *out);
    for (size_t i = 0; out && i < n; ++i)
        out[i] = dupStr(s[i]);
    return out;
}

/* The fake dialog: answered by port_test_file_dialog_respond (queued or
 * later), or at once with UNAVAILABLE after port_test_file_dialog_available(0). */
void port_file_dialog_show(
    port_app               *a,
    const port_file_dialog *src,
    void (*cb)(void *, int, const char *const *, size_t),
    void *user
) {
    port_file_dialog d;
    memset(&d, 0, sizeof d);
    memcpy(&d, src, src->size < sizeof d ? src->size : sizeof d);
    freeLastDialog(a);
    port_file_dialog *last = &a->last_dialog;
    last->size             = sizeof *last;
    last->mode             = d.mode;
    last->title            = dupStr(d.title);
    last->initial_dir      = dupStr(d.initial_dir);
    last->suggested_name   = dupStr(d.suggested_name);
    last->parent           = d.parent; /* compared by tests, never dereferenced */
    port_file_filter *f =
        (port_file_filter *)calloc(d.filter_count ? d.filter_count : 1, sizeof *f);
    for (size_t i = 0; f && i < d.filter_count; ++i) {
        f[i].name = dupStr(d.filters[i].name);
        f[i].patterns =
            (const char *const *)copyStrings(d.filters[i].patterns, d.filters[i].pattern_count);
        f[i].pattern_count = d.filters[i].pattern_count;
    }
    last->filters      = f;
    last->filter_count = f ? d.filter_count : 0;
    a->has_last        = 1;

    if (!a->dialog_available) {
        dialogAnswer(a, cb, user, PORT_DIALOG_UNAVAILABLE, NULL, 0);
    } else if (a->has_answer) {
        a->has_answer = 0;
        dialogAnswer(a, cb, user, PORT_DIALOG_CHOSEN, a->answer, a->answer_count);
        a->answer       = NULL;
        a->answer_count = 0;
    } else {
        a->pending_cb   = cb;
        a->pending_user = user;
    }
}

void port_system_settings_get(port_app *a, port_system_settings *out) {
    (void)a;
    port_system_settings s;
    memset(&s, 0, sizeof s);
    s.size            = sizeof s;
    s.text_scale      = 1.0;
    s.caret_blink_ms  = 530;
    s.double_click_ms = 400;
    memcpy(out, &s, out->size < sizeof s ? out->size : sizeof s);
}

/* gettext's order, simplified: $LANGUAGE's list, then the locale; a
 * "C"/"POSIX" locale means untranslated and disables $LANGUAGE. */
int port_preferred_languages(char *buf, size_t cap) {
    const char *names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    const char *locale  = NULL;
    for (int i = 0; i < 3 && !locale; ++i) {
        const char *v = getenv(names[i]);
        if (v && *v)
            locale = v;
    }
    if (!locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX"))
        return ref_copy_out("", buf, cap);
    char        out[2048];
    const char *list = getenv("LANGUAGE");
    size_t      at   = 0;
    for (const char *p = list; p && *p;) {
        const char  *e = strchr(p, ':');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && at + n + 1 < sizeof out) {
            memcpy(out + at, p, n);
            at += n;
            out[at++] = '\n';
        }
        p += n + (e ? 1 : 0);
    }
    const size_t n = strlen(locale);
    if (at + n < sizeof out) {
        memcpy(out + at, locale, n);
        at += n;
    }
    out[at] = 0;
    return ref_copy_out(out, buf, cap);
}

/* ── Test hooks ────────────────────────────────────────────────────────── */

static void updateMods(port_app *a, uint32_t k, int down) {
    uint32_t bit = 0;
    switch (k) {
    case PORT_KEY_SHIFT_LEFT:
    case PORT_KEY_SHIFT_RIGHT:
        bit = PORT_MOD_SHIFT;
        break;
    case PORT_KEY_CONTROL_LEFT:
    case PORT_KEY_CONTROL_RIGHT:
        bit = PORT_MOD_CTRL;
        break;
    case PORT_KEY_ALT_LEFT:
    case PORT_KEY_ALT_RIGHT:
        bit = PORT_MOD_ALT;
        break;
    case PORT_KEY_SUPER_LEFT:
    case PORT_KEY_SUPER_RIGHT:
        bit = PORT_MOD_SUPER;
        break;
    default:
        return;
    }
    a->mods = down ? (a->mods | bit) : (a->mods & ~bit);
}

int port_test_key(port_window *w, uint32_t key, int down) {
    port_app *a = w->app;
    if (key >= PORT_KEY_COUNT)
        return PORT_E_INVAL;
    focus(a, w);
    updateMods(a, key, down);
    port_event e;
    memset(&e, 0, sizeof e);
    e.type       = down ? PORT_EV_KEY_DOWN : PORT_EV_KEY_UP;
    e.window     = w;
    e.key        = key;
    e.mods       = a->mods;
    e.flags      = down && a->down[key] ? PORT_EVF_REPEAT : 0;
    a->down[key] = (unsigned char)down;
    emit(a, &e);
    /* US-layout text for the keys tests use; Ctrl/Super chords type nothing. */
    if (down && !(a->mods & (PORT_MOD_CTRL | PORT_MOD_SUPER)) && w->text_input &&
        listHas(&a->windows, w)) {
        char t = 0;
        if (key >= PORT_KEY_A && key <= PORT_KEY_Z)
            t = (char)(((a->mods & PORT_MOD_SHIFT) ? 'A' : 'a') + (int)(key - PORT_KEY_A));
        else if (key >= PORT_KEY_NUM0 && key <= PORT_KEY_NUM9 && !(a->mods & PORT_MOD_SHIFT))
            t = (char)('0' + (int)(key - PORT_KEY_NUM0));
        else if (key == PORT_KEY_SPACE)
            t = ' ';
        if (t) {
            memset(&e, 0, sizeof e);
            e.type     = PORT_EV_TEXT_INPUT;
            e.window   = w;
            e.text     = &t;
            e.text_len = 1;
            emit(a, &e);
        }
    }
    return PORT_OK;
}

int port_test_pointer_move(port_window *w, double x, double y) {
    port_app *a = w->app;
    a->px       = x;
    a->py       = y;
    if (a->dragging) {
        dragOver(a, w);
        return PORT_OK;
    }
    port_event e;
    memset(&e, 0, sizeof e);
    if (a->hover != w) {
        if (a->hover)
            emitType(a, a->hover, PORT_EV_POINTER_LEAVE);
        a->hover = w;
        e.type   = PORT_EV_POINTER_ENTER;
        e.window = w;
        e.x      = x;
        e.y      = y;
        e.mods   = a->mods;
        emit(a, &e);
    }
    memset(&e, 0, sizeof e);
    e.type   = PORT_EV_POINTER_MOVE;
    e.window = w;
    e.x      = x;
    e.y      = y;
    e.mods   = a->mods;
    emit(a, &e);
    return PORT_OK;
}

static int isNonClient(uint32_t hit) {
    return hit != PORT_HIT_CLIENT && hit != PORT_HIT_MINIMIZE_BUTTON &&
           hit != PORT_HIT_MAXIMIZE_BUTTON && hit != PORT_HIT_CLOSE_BUTTON;
}

int port_test_button(port_window *w, uint32_t button, int down) {
    port_app *a = w->app;
    if (down) {
        focus(a, w);
        const int64_t t = nowMs();
        a->clicks = a->clicks > 0 && (int)button == a->click_button && t - a->click_time <= 400
                        ? a->clicks + 1
                        : 1;
        a->click_button = (int)button;
        a->click_time   = t;
        /* Like the OS: a press on a non-client area becomes a move/resize and
         * never reaches the app. */
        if (button == PORT_BUTTON_LEFT && a->cb.hit_test) {
            w->last_hit = a->cb.hit_test(a->cb.user, w, a->px, a->py);
            if (isNonClient(w->last_hit))
                return PORT_OK;
        }
    }
    if (!down && a->dragging) {
        finishDrag(a);
        a->button_held = 0;
        return PORT_OK;
    }
    a->button_held = down;
    port_event e;
    memset(&e, 0, sizeof e);
    e.type   = down ? PORT_EV_POINTER_DOWN : PORT_EV_POINTER_UP;
    e.window = w;
    e.x      = a->px;
    e.y      = a->py;
    e.button = button;
    e.clicks = down ? a->clicks : 0;
    e.mods   = a->mods;
    emit(a, &e);
    return PORT_OK;
}

int port_test_scroll(port_window *w, double dx, double dy) {
    return port_test_phased_scroll(w, dx, dy, (uint32_t)-1);
}

int port_test_phased_scroll(port_window *w, double dx, double dy, uint32_t phase) {
    port_app  *a = w->app;
    port_event e;
    memset(&e, 0, sizeof e);
    e.type   = PORT_EV_SCROLL;
    e.window = w;
    e.x      = a->px;
    e.y      = a->py;
    e.dx     = dx;
    e.dy     = dy;
    e.mods   = a->mods;
    if (phase != (uint32_t)-1) { /* phased: a touchpad, precise */
        e.flags = PORT_EVF_PRECISE;
        e.phase = phase;
    }
    emit(a, &e);
    return PORT_OK;
}

int port_test_gesture(port_window *w, uint32_t fingers, double dx, double dy) {
    port_app      *a = w->app;
    port_event     e;
    const uint32_t types[3] = {PORT_EV_GESTURE_BEGIN, PORT_EV_GESTURE_UPDATE, PORT_EV_GESTURE_END};
    for (int i = 0; i < 3; ++i) {
        memset(&e, 0, sizeof e);
        e.type    = types[i];
        e.window  = w;
        e.x       = a->px;
        e.y       = a->py;
        e.fingers = fingers;
        if (i == 1)
            e.dx = dx, e.dy = dy;
        emit(a, &e);
    }
    return PORT_OK;
}

int port_test_read_pixel(port_window *w, int32_t x, int32_t y, uint32_t *argb) {
    if (x < 0 || y < 0 || x >= w->pw || y >= w->ph || !w->pixels)
        return PORT_E_RANGE;
    *argb = w->pixels[(size_t)y * (size_t)w->pw + (size_t)x];
    return PORT_OK;
}

int port_test_tray_activate(port_tray *t) {
    port_event e;
    memset(&e, 0, sizeof e);
    e.type = PORT_EV_TRAY_ACTIVATED;
    e.tray = t;
    emit(t->app, &e);
    return PORT_OK;
}

int port_test_tray_menu_select(port_tray *t, uint32_t id) {
    for (size_t i = 0; i < t->menu_count; ++i) {
        const port_menu_item *m = &t->menu[i];
        if (m->id != id || m->kind == PORT_MENU_SEPARATOR || m->kind == PORT_MENU_SUBMENU)
            continue;
        if (!m->enabled)
            return PORT_E_INVAL;
        port_event e;
        memset(&e, 0, sizeof e);
        e.type = PORT_EV_TRAY_MENU_ITEM;
        e.tray = t;
        e.id   = id;
        emit(t->app, &e);
        return PORT_OK;
    }
    return PORT_E_NOENT;
}

int port_test_tray_probe(
    port_tray *t, void (*fn)(void *, const port_test_tray_info *), void *user
) {
    const char **labels = (const char **)calloc(t->menu_count + 1, sizeof *labels);
    if (!labels)
        return PORT_E_NOMEM;
    /* The array is already in pre-order, which is depth-first. */
    for (size_t i = 0; i < t->menu_count; ++i)
        labels[i] = t->menu[i].kind == PORT_MENU_SEPARATOR ? "-" : t->menu[i].label;
    port_test_tray_info p = {
        sizeof p, t->tooltip, t->icon_templ, t->icon_count, t->icon_sizes, t->menu_count, labels
    };
    fn(user, &p);
    free(labels);
    return PORT_OK;
}

int port_test_notification_invoke(port_app *a, uint64_t id, const char *action) {
    if (!findNote(a, id))
        return PORT_E_NOENT;
    port_event e;
    memset(&e, 0, sizeof e);
    e.type   = PORT_EV_NOTIFICATION_ACTIVATED;
    e.id     = id;
    e.action = action ? action : "";
    emit(a, &e);
    return PORT_OK;
}

int port_test_notification_probe(
    port_app *a, uint64_t id, void (*fn)(void *, const port_test_notification_info *), void *user
) {
    const notification *n = findNote(a, id);
    if (!n)
        return PORT_E_NOENT;
    port_test_notification_info p = {
        sizeof p,
        n->title,
        n->body,
        n->image_w,
        n->image_h,
        n->silent,
        n->action_count,
        (const char *const *)n->labels
    };
    fn(user, &p);
    return PORT_OK;
}

uint64_t port_test_max_notification_id(port_app *a) {
    return a->max_note_id;
}

size_t port_test_notification_count(port_app *a) {
    return a->note_count;
}

int port_test_badge(port_app *a) {
    return a->badge;
}

int port_test_wants_attention(port_window *w, int *out) {
    *out = w->attention;
    return PORT_OK;
}

int port_test_file_dialog_respond(port_app *a, const char *const *paths, size_t count) {
    if (a->pending_cb) {
        void (*cb)(void *, int, const char *const *, size_t) = a->pending_cb;
        a->pending_cb                                        = NULL;
        dialogAnswer(a, cb, a->pending_user, PORT_DIALOG_CHOSEN, copyStrings(paths, count), count);
    } else {
        freeStrings(a->answer, a->answer_count);
        a->answer       = copyStrings(paths, count);
        a->answer_count = count;
        a->has_answer   = 1;
    }
    return PORT_OK;
}

int port_test_file_dialog_available(port_app *a, int on) {
    a->dialog_available = on;
    return PORT_OK;
}

int port_test_last_file_dialog(
    port_app *a, void (*fn)(void *, const port_file_dialog *), void *user
) {
    if (!a->has_last)
        return PORT_E_NOENT;
    fn(user, &a->last_dialog);
    return PORT_OK;
}

int port_test_system_event(port_app *a, uint32_t type, int online) {
    if (type == PORT_EV_NETWORK_CHANGED)
        a->online = online;
    port_event e;
    memset(&e, 0, sizeof e);
    e.type  = type;
    e.flags = online ? PORT_EVF_ONLINE : 0;
    emit(a, &e);
    return PORT_OK;
}

int port_test_deliver_url(port_app *a, const char *url) {
    port_event e;
    memset(&e, 0, sizeof e);
    e.type         = PORT_EV_OPEN_URLS;
    e.strings      = &url;
    e.string_count = 1;
    emit(a, &e);
    return PORT_OK;
}

int port_test_last_opened_url(port_app *a, char *buf, size_t cap) {
    return a->last_url ? ref_copy_out(a->last_url, buf, cap) : PORT_E_NOENT;
}

int port_test_set_scale(port_window *w, double scale) {
    w->scale = scale;
    emitType(w->app, w, PORT_EV_RESIZED);
    port_window_request_frame(w);
    return PORT_OK;
}

int port_test_frames(port_window *w) {
    return w->frames;
}

uint32_t port_test_cursor(port_window *w) {
    return w->cursor;
}

uint32_t port_test_last_hit(port_window *w) {
    return w->last_hit;
}
