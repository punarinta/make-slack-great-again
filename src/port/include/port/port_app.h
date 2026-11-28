/* port_app.h — the event loop, surfaces (windows), input, clipboard, drag
 * and drop, tray, notifications and the desktop services the UI uses.
 *
 * The loop is the port's: it waits on whatever the OS waits on (fds,
 * handles, ports) and calls the core back. The core never asks for a file
 * descriptor. It gets:
 *   - events, through port_app_callbacks.on_event;
 *   - wake-ups: port_app_wake (any thread) makes the loop call on_wake on
 *     its thread soon (several wakes may merge into one call) — the core
 *     runs its cross-thread queue there;
 *   - timers: port_timer_add, on the loop thread.
 *
 * Threading: everything here but port_app_wake is called on the thread that
 * called port_app_create, and every callback runs on it, from inside
 * port_app_run / port_app_pump. Callbacks never run re-entrantly from inside
 * the port call that caused them, with one exception the core relies on:
 * the test hooks (port_test.h) deliver the events they synthesise at once,
 * from inside the hook call, as an OS would deliver real input.
 *
 * Coordinates are logical pixels (double where input is sub-pixel); a
 * window's scale converts to the physical pixels of its canvas. */
#ifndef PORT_PORT_APP_H
#define PORT_PORT_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct port_app    port_app;
typedef struct port_window port_window;
typedef struct port_tray   port_tray;

typedef struct port_rect {
    int32_t x, y, w, h;
} port_rect;

/* A CPU image (tray icon, notification picture, drag image): premultiplied
 * ARGB32 in native endianness (0xAARRGGBB as a uint32_t), stride = width.
 * scale: physical pixels per logical pixel it was drawn for. */
typedef struct port_image {
    uint32_t        size;
    int32_t         width, height;
    double          scale;
    const uint32_t *pixels;
} port_image;

/* A MIME-typed blob (clipboard, drag payload, drop contents). Standard types:
 * "text/plain;charset=utf-8", "text/html", "text/uri-list", "image/png". */
typedef struct port_data_item {
    const char *mime;
    const char *data;
    size_t      len;
} port_data_item;

/* ── Keys ──────────────────────────────────────────────────────────────────
 * The logical key, resolved through the active layout (Ctrl+Z is "the key
 * that types z"; non-Latin layouts resolve letters through the first Latin
 * one). Same order as plat::Key, which the core checks at compile time. */
enum {
    PORT_KEY_UNKNOWN = 0,
    PORT_KEY_A,
    PORT_KEY_B,
    PORT_KEY_C,
    PORT_KEY_D,
    PORT_KEY_E,
    PORT_KEY_F,
    PORT_KEY_G,
    PORT_KEY_H,
    PORT_KEY_I,
    PORT_KEY_J,
    PORT_KEY_K,
    PORT_KEY_L,
    PORT_KEY_M,
    PORT_KEY_N,
    PORT_KEY_O,
    PORT_KEY_P,
    PORT_KEY_Q,
    PORT_KEY_R,
    PORT_KEY_S,
    PORT_KEY_T,
    PORT_KEY_U,
    PORT_KEY_V,
    PORT_KEY_W,
    PORT_KEY_X,
    PORT_KEY_Y,
    PORT_KEY_Z,
    PORT_KEY_NUM0,
    PORT_KEY_NUM1,
    PORT_KEY_NUM2,
    PORT_KEY_NUM3,
    PORT_KEY_NUM4,
    PORT_KEY_NUM5,
    PORT_KEY_NUM6,
    PORT_KEY_NUM7,
    PORT_KEY_NUM8,
    PORT_KEY_NUM9,
    PORT_KEY_F1,
    PORT_KEY_F2,
    PORT_KEY_F3,
    PORT_KEY_F4,
    PORT_KEY_F5,
    PORT_KEY_F6,
    PORT_KEY_F7,
    PORT_KEY_F8,
    PORT_KEY_F9,
    PORT_KEY_F10,
    PORT_KEY_F11,
    PORT_KEY_F12,
    PORT_KEY_F13,
    PORT_KEY_F14,
    PORT_KEY_F15,
    PORT_KEY_F16,
    PORT_KEY_F17,
    PORT_KEY_F18,
    PORT_KEY_F19,
    PORT_KEY_F20,
    PORT_KEY_F21,
    PORT_KEY_F22,
    PORT_KEY_F23,
    PORT_KEY_F24,
    PORT_KEY_ESCAPE,
    PORT_KEY_ENTER,
    PORT_KEY_TAB,
    PORT_KEY_BACKSPACE,
    PORT_KEY_DELETE,
    PORT_KEY_INSERT,
    PORT_KEY_HOME,
    PORT_KEY_END,
    PORT_KEY_PAGE_UP,
    PORT_KEY_PAGE_DOWN,
    PORT_KEY_LEFT,
    PORT_KEY_RIGHT,
    PORT_KEY_UP,
    PORT_KEY_DOWN,
    PORT_KEY_SPACE,
    PORT_KEY_MINUS,
    PORT_KEY_EQUAL,
    PORT_KEY_BRACKET_LEFT,
    PORT_KEY_BRACKET_RIGHT,
    PORT_KEY_BACKSLASH,
    PORT_KEY_SEMICOLON,
    PORT_KEY_APOSTROPHE,
    PORT_KEY_GRAVE,
    PORT_KEY_COMMA,
    PORT_KEY_PERIOD,
    PORT_KEY_SLASH,
    PORT_KEY_CAPS_LOCK,
    PORT_KEY_SHIFT_LEFT,
    PORT_KEY_SHIFT_RIGHT,
    PORT_KEY_CONTROL_LEFT,
    PORT_KEY_CONTROL_RIGHT,
    PORT_KEY_ALT_LEFT,
    PORT_KEY_ALT_RIGHT,
    PORT_KEY_SUPER_LEFT,
    PORT_KEY_SUPER_RIGHT,
    PORT_KEY_MENU,
    PORT_KEY_PRINT_SCREEN,
    PORT_KEY_SCROLL_LOCK,
    PORT_KEY_PAUSE,
    PORT_KEY_NUM_LOCK,
    PORT_KEY_KP0,
    PORT_KEY_KP1,
    PORT_KEY_KP2,
    PORT_KEY_KP3,
    PORT_KEY_KP4,
    PORT_KEY_KP5,
    PORT_KEY_KP6,
    PORT_KEY_KP7,
    PORT_KEY_KP8,
    PORT_KEY_KP9,
    PORT_KEY_KP_DECIMAL,
    PORT_KEY_KP_DIVIDE,
    PORT_KEY_KP_MULTIPLY,
    PORT_KEY_KP_SUBTRACT,
    PORT_KEY_KP_ADD,
    PORT_KEY_KP_ENTER,
    PORT_KEY_KP_EQUAL,
    PORT_KEY_BACK,
    PORT_KEY_FORWARD,
    PORT_KEY_COUNT,
};
enum {
    PORT_MOD_SHIFT = 1,
    PORT_MOD_CTRL  = 2,
    PORT_MOD_ALT   = 4, /* Option on macOS */
    PORT_MOD_SUPER = 8, /* Command on macOS, the Windows key elsewhere */
    PORT_MOD_CAPS  = 16,
    PORT_MOD_NUM   = 32
};
enum {
    PORT_BUTTON_LEFT = 0,
    PORT_BUTTON_RIGHT,
    PORT_BUTTON_MIDDLE,
    PORT_BUTTON_BACK,
    PORT_BUTTON_FORWARD
};
/* Pointer shapes; a port without some maps them to the nearest it has. */
enum {
    PORT_CURSOR_ARROW = 0,
    PORT_CURSOR_IBEAM,
    PORT_CURSOR_HAND,
    PORT_CURSOR_WAIT,
    PORT_CURSOR_PROGRESS,
    PORT_CURSOR_CROSSHAIR,
    PORT_CURSOR_NOT_ALLOWED,
    PORT_CURSOR_MOVE,
    PORT_CURSOR_GRAB,
    PORT_CURSOR_GRABBING,
    PORT_CURSOR_RESIZE_H,
    PORT_CURSOR_RESIZE_V,
    PORT_CURSOR_RESIZE_NWSE,
    PORT_CURSOR_RESIZE_NESW,
    PORT_CURSOR_HIDDEN
};
/* What sits under a point of a window drawn with custom decorations (the
 * core answers hit_test; a port turns CAPTION/RESIZE_* into the OS's own
 * move/resize and never delivers those presses). */
enum {
    PORT_HIT_CLIENT = 0,
    PORT_HIT_CAPTION,
    PORT_HIT_RESIZE_TOP,
    PORT_HIT_RESIZE_BOTTOM,
    PORT_HIT_RESIZE_LEFT,
    PORT_HIT_RESIZE_RIGHT,
    PORT_HIT_RESIZE_TOP_LEFT,
    PORT_HIT_RESIZE_TOP_RIGHT,
    PORT_HIT_RESIZE_BOTTOM_LEFT,
    PORT_HIT_RESIZE_BOTTOM_RIGHT,
    PORT_HIT_MINIMIZE_BUTTON,
    PORT_HIT_MAXIMIZE_BUTTON,
    PORT_HIT_CLOSE_BUTTON
};
enum {
    PORT_SCROLL_NONE = 0, /* plain wheels: no phases */
    PORT_SCROLL_BEGIN,
    PORT_SCROLL_UPDATE,
    PORT_SCROLL_END,
    PORT_SCROLL_MOMENTUM
};
enum { PORT_DROP_NONE = 0, PORT_DROP_COPY, PORT_DROP_MOVE, PORT_DROP_LINK };
enum { PORT_DROP_ALLOW_COPY = 1, PORT_DROP_ALLOW_MOVE = 2, PORT_DROP_ALLOW_LINK = 4 };
enum { PORT_SEL_CLIPBOARD = 0, PORT_SEL_PRIMARY = 1 };

/* ── Events ────────────────────────────────────────────────────────────────
 * The same set and meaning as plat::EventType (plat/plat.h documents each);
 * a port sends the ones it can. A single-surface port typically sends
 * RESIZED (the screen's app area changed), FRAME, FOCUS_*, POINTER_*,
 * SCROLL, KEY_*, TEXT_INPUT, CLOSE_REQUESTED, TRAY_*, NOTIFICATION_*. */
enum {
    PORT_EV_NONE = 0,
    PORT_EV_CLOSE_REQUESTED,
    PORT_EV_RESIZED, /* size and/or scale changed; a FRAME follows */
    PORT_EV_FRAME,   /* paint now (after request_frame, exposure, resize) */
    PORT_EV_FOCUS_IN,
    PORT_EV_FOCUS_OUT,
    PORT_EV_STATE_CHANGED, /* port_window_state() bits changed */
    PORT_EV_POINTER_ENTER,
    PORT_EV_POINTER_LEAVE,
    PORT_EV_POINTER_MOVE,
    PORT_EV_POINTER_DOWN,
    PORT_EV_POINTER_UP,
    PORT_EV_SCROLL,
    PORT_EV_KEY_DOWN, /* also auto-repeat (PORT_EVF_REPEAT) */
    PORT_EV_KEY_UP,
    PORT_EV_TEXT_INPUT,   /* committed text, while text input is enabled */
    PORT_EV_TEXT_PREEDIT, /* composition in progress; empty = ended */
    PORT_EV_DROP_ENTER,   /* items: the offered types, no data yet */
    PORT_EV_DROP_MOVE,
    PORT_EV_DROP_LEAVE,
    PORT_EV_DROP,          /* items with data; strings = the URIs of text/uri-list */
    PORT_EV_THEME_CHANGED, /* dark mode, accent or system settings changed */
    PORT_EV_GESTURE_BEGIN, /* multi-finger swipes */
    PORT_EV_GESTURE_UPDATE,
    PORT_EV_GESTURE_END,
    PORT_EV_SWIPE_GESTURE, /* a one-shot navigation swipe: dx = +1 back, -1 forward */
    PORT_EV_DRAG_FINISHED, /* a port_drag_start ended; drop_action = what the target did */
    /* App-level: window == NULL. */
    PORT_EV_TRAY_ACTIVATED,
    PORT_EV_TRAY_MENU_ITEM,         /* id = the item's id */
    PORT_EV_NOTIFICATION_ACTIVATED, /* id, action ("" = the body) */
    PORT_EV_NOTIFICATION_CLOSED,
    PORT_EV_NOTIFICATION_FAILED, /* text = why */
    PORT_EV_MONITORS_CHANGED,
    PORT_EV_MOVED,
    PORT_EV_INSTANCE_ACTIVATED, /* strings = the other launch's args, text = its cwd */
    PORT_EV_OPEN_URLS,          /* strings = the URLs */
    PORT_EV_QUIT_REQUESTED,
    PORT_EV_NETWORK_CHANGED, /* PORT_EVF_ONLINE */
    PORT_EV_SUSPENDING,
    PORT_EV_RESUMED
};
enum {
    PORT_EVF_REPEAT    = 1, /* KEY_DOWN auto-repeat */
    PORT_EVF_PRECISE   = 2, /* SCROLL in logical pixels (touchpad), else in wheel notches */
    PORT_EVF_CANCELLED = 4, /* GESTURE_END taken over by the system */
    PORT_EVF_ONLINE    = 8  /* NETWORK_CHANGED */
};
/* Everything an event carries; which fields mean something depends on the
 * type, as in plat::Event. Pointers are valid during the callback only. */
typedef struct port_event {
    uint32_t     size;
    uint32_t     type;  /* PORT_EV_* */
    uint32_t     flags; /* PORT_EVF_* */
    port_window *window;
    double       x, y;   /* pointer / drop position, logical */
    double       dx, dy; /* SCROLL (+y = content moves up), GESTURE_UPDATE motion */
    uint32_t     key;    /* PORT_KEY_* */
    uint32_t     scancode;
    uint32_t     mods; /* PORT_MOD_* */
    uint32_t     button;
    int32_t      clicks; /* POINTER_DOWN: 1, 2 (double), ... by the OS's double-click time */
    uint32_t     phase;  /* SCROLL: PORT_SCROLL_* */
    uint32_t     fingers;
    const char  *text; /* TEXT_INPUT, TEXT_PREEDIT, DROP (text), NOTIFICATION_FAILED, ... */
    size_t       text_len;
    int32_t      preedit_begin, preedit_end; /* byte offsets of the preedit cursor; -1 hidden */
    uint32_t     drop_action;                /* PORT_DROP_* */
    uint32_t     allowed_actions;            /* PORT_DROP_ALLOW_* */
    const port_data_item *items;
    size_t                item_count;
    const char *const    *strings;
    size_t                string_count;
    port_tray            *tray;
    uint64_t              id;
    const char           *action; /* NOTIFICATION_ACTIVATED */
    /* TRAY_*, NOTIFICATION_ACTIVATED, INSTANCE_ACTIVATED: an activation token
     * for port_window_activate (Wayland's xdg-activation); "" or NULL. */
    const char           *token;
} port_event;

/* ── The app and its loop ─────────────────────────────────────────────── */
typedef struct port_app_callbacks {
    uint32_t size;
    void    *user;
    void (*on_event)(void *user, const port_event *e);
    void (*on_wake)(void *user);
    /* Custom-decoration windows: what is under (x, y); PORT_HIT_*. */
    uint32_t (*hit_test)(void *user, port_window *w, double x, double y);
} port_app_callbacks;
typedef struct port_app_desc {
    uint32_t           size;
    const char        *name; /* human-readable ("msga") */
    const char        *id;   /* reverse-DNS app id: notification sender, launcher id */
    /* A stable name for this instance's notifications (see plat::AppInfo). */
    const char        *notification_scope;
    port_app_callbacks callbacks;
} port_app_desc;

/* One app per process. err (may be NULL) gets why it failed. */
int  port_app_create(const port_app_desc *d, port_app **out, char *err, size_t err_cap);
void port_app_destroy(port_app *a);
/* Runs until port_app_quit. */
void port_app_run(port_app *a);
void port_app_quit(port_app *a);
/* One turn: wait up to timeout_ms (-1 forever, 0 not at all), dispatch what
 * is ready (events, timers, wakes, due frames), return. */
void port_app_pump(port_app *a, int32_t timeout_ms);
/* Any thread: on_wake runs soon on the loop thread. */
void port_app_wake(port_app *a);

/* fn(user) after interval_ms on the loop thread, once or repeatedly. Never
 * 0. Cancelling an unknown or finished timer does nothing; a timer may
 * cancel itself or others from inside fn. */
uint64_t
port_timer_add(port_app *a, uint32_t interval_ms, int repeat, void (*fn)(void *user), void *user);
void port_timer_cancel(port_app *a, uint64_t id);

/* Directory watching (PORT_CAP2_FILE_WATCH): fn(user) on the loop thread
 * soon after an entry of the directory `path` is added, removed, renamed or
 * written to (not its subdirectories); several changes may merge into one
 * call. The core follows Claude Code's transcripts this way; without it the
 * core looks on a timer. 0 = not watched. */
uint64_t port_watch_dir(port_app *a, const char *path, void (*fn)(void *user), void *user);
void     port_unwatch_dir(port_app *a, uint64_t id);

/* ── Windows / the surface ─────────────────────────────────────────────────
 * With PORT_CAP_WINDOWS: top-level windows the user moves and resizes.
 * Without it: the one surface. port_window_create then returns a window for
 * it every time (the last one shown is the one attached and getting input);
 * set_size / set_position / minimize / maximize do nothing, and the size is
 * whatever the OS gives (RESIZED tells of changes). */
enum {
    PORT_WINDOW_RESIZABLE    = 1,
    PORT_WINDOW_VISIBLE      = 2,
    PORT_WINDOW_CUSTOM_DECOR = 4, /* the core draws its title bar; hit_test applies */
    PORT_WINDOW_HAS_POSITION = 8  /* x, y are meaningful */
};
typedef struct port_window_desc {
    uint32_t          size;
    const char       *title;
    const char       *app_id;   /* Wayland app_id / WM_CLASS / AUMID */
    const char       *wm_class; /* X11 res_class; NULL = app_id */
    const port_image *icons;
    size_t            icon_count;
    int32_t           width, height;         /* logical */
    int32_t           min_width, min_height; /* logical */
    double            x, y;                  /* PORT_WINDOW_HAS_POSITION */
    uint32_t          flags;                 /* PORT_WINDOW_* */
    void             *user;                  /* returned by port_window_user */
} port_window_desc;
int   port_window_create(port_app *a, const port_window_desc *d, port_window **out);
void  port_window_destroy(port_window *w);
void *port_window_user(port_window *w);

void   port_window_set_title(port_window *w, const char *title);
void   port_window_size(port_window *w, int32_t *width, int32_t *height); /* logical */
double port_window_scale(port_window *w); /* physical / logical; may be fractional */
void   port_window_set_size(port_window *w, int32_t width, int32_t height);
void   port_window_set_min_size(port_window *w, int32_t width, int32_t height);
void   port_window_show(port_window *w);
void   port_window_hide(port_window *w);
void   port_window_minimize(port_window *w);
void   port_window_set_maximized(port_window *w, int on);
void   port_window_set_fullscreen(port_window *w, int on);
void   port_window_set_always_on_top(port_window *w, int on);
/* Raise and focus where allowed; token from an event (may be NULL). */
void   port_window_activate(port_window *w, const char *token);
enum {
    PORT_WSTATE_VISIBLE      = 1,
    PORT_WSTATE_ACTIVE       = 2,
    PORT_WSTATE_MAXIMIZED    = 4,
    PORT_WSTATE_MINIMIZED    = 8,
    PORT_WSTATE_FULLSCREEN   = 16,
    PORT_WSTATE_ABOVE        = 32,
    PORT_WSTATE_SYSTEM_DECOR = 64 /* the OS draws the decorations right now */
};
uint32_t port_window_state(port_window *w);
/* PORT_CAP_WINDOW_POSITION: logical virtual-desktop coordinates. */
int      port_window_position(port_window *w, double *x, double *y);
int      port_window_set_position(port_window *w, double x, double y);
uint64_t port_window_monitor(port_window *w); /* 0 = unknown */
/* Custom decorations: OS-drawn parts in the app's dark or light look, and
 * the height of a native title band the content extends under. */
void     port_window_set_dark_chrome(port_window *w, int dark);
double   port_window_title_bar_height(port_window *w);
void     port_window_set_cursor(port_window *w, uint32_t cursor);
/* Text input on/off, and the caret (logical, window-relative) where an IME
 * puts its candidate box. */
void     port_window_set_text_input(port_window *w, int enabled, port_rect caret);
void     port_window_request_attention(port_window *w);
/* The answer to the current DROP_ENTER / DROP_MOVE (PORT_DROP_*). */
void     port_window_set_drop_action(port_window *w, uint32_t action);
/* HWND / NSWindow * / a stage surface id..., for diagnostics. */
void    *port_window_native(port_window *w);

/* Painting. request_frame asks for a FRAME event (paced to the display);
 * inside it the core calls begin_paint, draws into the canvas (physical
 * pixels, premultiplied ARGB32 0xAARRGGBB, which holds the last presented
 * frame, so redrawing only the damage is valid) and end_paint with the
 * damaged rectangles (physical; none = everything). A port whose screen has
 * no alpha ignores it; the core keeps its pixels opaque. */
typedef struct port_canvas {
    uint32_t  size;
    uint32_t *pixels;
    int32_t   width, height; /* physical */
    int32_t   stride;        /* in pixels */
    double    scale;
} port_canvas;
void port_window_request_frame(port_window *w);
int  port_window_begin_paint(port_window *w, port_canvas *out);
void port_window_end_paint(port_window *w, const port_rect *damage, size_t count);

/* ── Clipboard (PORT_CAP_CLIPBOARD, PORT_CAP_PRIMARY_SELECTION) ─────────────
 * Setting is synchronous and replaces every type offered before; reading is
 * asynchronous (the data may live in another process): the callback always
 * runs exactly once, later, on the loop thread — data NULL when the type is
 * not there or the OS refused. */
int  port_clipboard_set(port_app *a, int sel, const port_data_item *items, size_t count);
void port_clipboard_get(
    port_app   *a,
    int         sel,
    const char *mime,
    void (*cb)(void *user, const char *data, size_t len),
    void *user
);
void port_clipboard_types(
    port_app *a, int sel, void (*cb)(void *user, const char *const *mimes, size_t count), void *user
);

/* ── Drag source (PORT_CAP_DRAG_SOURCE) ────────────────────────────────── */
typedef struct port_drag_desc {
    uint32_t              size;
    const port_data_item *items;
    size_t                item_count;
    const port_image     *image; /* NULL = the OS default */
    double                hot_x, hot_y;
    uint32_t              actions; /* PORT_DROP_ALLOW_* */
} port_drag_desc;
/* From a POINTER_DOWN/MOVE handler while the button is held. PORT_OK means a
 * DRAG_FINISHED follows. */
int port_drag_start(port_app *a, port_window *source, const port_drag_desc *d);

/* ── Tray (PORT_CAP_TRAY) ──────────────────────────────────────────────── */
enum { PORT_MENU_ACTION = 0, PORT_MENU_CHECKBOX, PORT_MENU_SEPARATOR, PORT_MENU_SUBMENU };
/* A menu as a flat array in pre-order: each item names its parent's index
 * (-1 at the top), so submenus need no pointers. */
typedef struct port_menu_item {
    uint32_t    size;
    uint32_t    kind; /* PORT_MENU_* */
    uint32_t    id;   /* echoed in TRAY_MENU_ITEM */
    int32_t     parent;
    const char *label;
    int         enabled, checked;
} port_menu_item;
int  port_tray_create(port_app *a, port_tray **out);
void port_tray_destroy(port_tray *t);
/* Several sizes of one icon; ignored without PORT_CAP_TRAY_ICON (the port
 * shows the app's own). template: only alpha counts (macOS menu bar). */
void port_tray_set_icon(port_tray *t, const port_image *sizes, size_t count, int template_image);
void port_tray_set_tooltip(port_tray *t, const char *text);
void port_tray_set_menu(port_tray *t, const port_menu_item *items, size_t count);
/* An attention mark where there is no badge count (PEOS's red dot). */
void port_tray_set_attention(port_tray *t, int on);
int  port_tray_visible(port_tray *t); /* a host shows it right now */

/* ── Notifications (PORT_CAP_NOTIFY, _IMAGES, _ACTIONS, _WITHDRAW) ──────── */
typedef struct port_notify_action {
    const char *key; /* echoed as the event's action */
    const char *label;
} port_notify_action;
typedef struct port_notification {
    uint32_t                  size;
    const char               *title, *body; /* plain text */
    const port_image         *image;        /* NULL = none */
    const port_notify_action *actions;
    size_t                    action_count;
    int32_t                   timeout_ms; /* -1 = the OS default */
    int                       silent;
} port_notification;
/* id: the core's (> 0), echoed in NOTIFICATION_* events. */
int  port_notify(port_app *a, uint64_t id, const port_notification *n);
void port_notify_withdraw(port_app *a, uint64_t id);
/* macOS: started by a click on a notification (the click then arrives as an
 * event). 0 elsewhere. */
int  port_launched_from_notification(port_app *a);
void port_set_badge(port_app *a, int count); /* PORT_CAP_BADGE; 0 clears */

/* ── Monitors (PORT_CAP_MONITORS) ──────────────────────────────────────── */
typedef struct port_monitor {
    uint32_t  size;
    uint64_t  id;
    char      name[64];
    port_rect bounds, work_area; /* logical, virtual-desktop coordinates */
    double    scale;
    int32_t   refresh_mhz; /* 0 = unknown */
    int       primary;
} port_monitor;
/* The number of monitors (fills up to cap of them; each out[i].size set by
 * the caller), or an error. */
int port_monitors(port_app *a, port_monitor *out, size_t cap);

/* ── Desktop services ──────────────────────────────────────────────────── */
/* 1: we are the primary instance; 0: another one has the key and got our
 * args (the caller exits). PORT_CAP_SINGLE_INSTANCE. */
int port_instance_claim(port_app *a, const char *key, const char *const *args, size_t argc);
int port_url_scheme_register(port_app *a, const char *scheme);
/* 1 online, 0 offline, PORT_E_NOSYS unknown. */
int port_network_online(port_app *a);
/* Hands a URL to the OS (the browser for http/https). */
int port_open_url(port_app *a, const char *url);

enum { PORT_DIALOG_OPEN = 0, PORT_DIALOG_OPEN_MULTIPLE, PORT_DIALOG_SAVE, PORT_DIALOG_FOLDER };
enum { PORT_DIALOG_CHOSEN = 0, PORT_DIALOG_CANCELLED, PORT_DIALOG_UNAVAILABLE };
typedef struct port_file_filter {
    const char        *name;     /* "Images" */
    const char *const *patterns; /* "*.png" */
    size_t             pattern_count;
} port_file_filter;
typedef struct port_file_dialog {
    uint32_t                size;
    uint32_t                mode; /* PORT_DIALOG_* */
    const char             *title;
    const char             *initial_dir;
    const char             *suggested_name;
    const port_file_filter *filters;
    size_t                  filter_count;
    port_window            *parent;
} port_file_dialog;
/* Asynchronous; cb runs once on the loop thread with a PORT_DIALOG_* status
 * and the chosen paths. Without PORT_CAP_FILE_DIALOG it answers
 * UNAVAILABLE and the core shows its own chooser. */
void port_file_dialog_show(
    port_app               *a,
    const port_file_dialog *d,
    void (*cb)(void *user, int status, const char *const *paths, size_t count),
    void *user
);

typedef struct port_system_settings {
    uint32_t size;
    int      dark_mode;
    int      reduced_motion;
    int      high_contrast;
    double   text_scale;     /* the user's font size on top of the scale; 1 = none */
    uint32_t accent_color;   /* 0xAARRGGBB; 0 = none */
    int32_t  caret_blink_ms; /* half period; 0 = steady */
    int32_t  double_click_ms;
} port_system_settings;
void port_system_settings_get(port_app *a, port_system_settings *out);
/* The user's languages, most preferred first, '\n'-separated ("sv-SE\nen-US");
 * returns the length (buf as for paths), 0 = unknown. */
int  port_preferred_languages(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
