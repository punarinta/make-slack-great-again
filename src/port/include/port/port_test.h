/* port_test.h — test hooks (PORT_CAP_TEST_HOOKS): input and desktop events
 * synthesised through the port's own path, and read-backs of what the port
 * shows, so msga's test suites drive a port the way they drive plat's
 * native backends (plat/testing.h mirrors these one to one).
 *
 * Only test builds of a port implement them for real; a release port
 * returns PORT_E_NOSYS from each (or leaves PORT_CAP_TEST_HOOKS clear).
 * Events these produce are delivered at once, from inside the call. */
#ifndef PORT_PORT_TEST_H
#define PORT_PORT_TEST_H

#include "port/port.h"

#ifdef __cplusplus
extern "C" {
#endif

int port_test_key(port_window *w, uint32_t key, int down);
int port_test_pointer_move(port_window *w, double x, double y);
int port_test_button(port_window *w, uint32_t button, int down);
int port_test_scroll(port_window *w, double dx, double dy);
int port_test_phased_scroll(port_window *w, double dx, double dy, uint32_t phase);
/* A whole swipe: BEGIN, one UPDATE with the motion, END. */
int port_test_gesture(port_window *w, uint32_t fingers, double dx, double dy);
/* One presented pixel, physical coordinates. */
int port_test_read_pixel(port_window *w, int32_t x, int32_t y, uint32_t *argb);

int port_test_tray_activate(port_tray *t);
int port_test_tray_menu_select(port_tray *t, uint32_t id);
/* What the tray shows: icon sizes, tooltip, menu labels depth-first with
 * separators as "-", each through fn. */
typedef struct port_test_tray_info {
    uint32_t           size;
    const char        *tooltip;
    int                template_image;
    size_t             icon_count;
    const port_rect   *icon_sizes; /* w, h of each */
    size_t             label_count;
    const char *const *labels;
} port_test_tray_info;
int port_test_tray_probe(
    port_tray *t, void (*fn)(void *user, const port_test_tray_info *p), void *user
);

int port_test_notification_invoke(port_app *a, uint64_t id, const char *action);
typedef struct port_test_notification_info {
    uint32_t           size;
    const char        *title, *body;
    int32_t            image_width, image_height;
    int                silent;
    size_t             action_count;
    const char *const *action_labels;
} port_test_notification_info;
int port_test_notification_probe(
    port_app *a,
    uint64_t  id,
    void (*fn)(void *user, const port_test_notification_info *p),
    void *user
);
uint64_t port_test_max_notification_id(port_app *a);
size_t   port_test_notification_count(port_app *a);
int      port_test_badge(port_app *a);
int      port_test_wants_attention(port_window *w, int *out);

/* The next file dialog answers with these paths (count 0 = cancel). */
int port_test_file_dialog_respond(port_app *a, const char *const *paths, size_t count);
int port_test_file_dialog_available(port_app *a, int on);
/* The last dialog asked for: fn gets it (pointers valid during fn). */
int port_test_last_file_dialog(
    port_app *a, void (*fn)(void *user, const port_file_dialog *d), void *user
);
/* NETWORK_CHANGED (online), SUSPENDING or RESUMED through the port's path. */
int port_test_system_event(port_app *a, uint32_t type, int online);
int port_test_deliver_url(port_app *a, const char *url);
/* The last URL port_open_url was given (length, buf as for paths). */
int port_test_last_opened_url(port_app *a, char *buf, size_t cap);

/* In-memory surfaces only (the reference port): change a window's scale,
 * count its presented frames, read its cursor and the last hit-test answer
 * a press got. */
int      port_test_set_scale(port_window *w, double scale);
int      port_test_frames(port_window *w);
uint32_t port_test_cursor(port_window *w);
uint32_t port_test_last_hit(port_window *w);

#ifdef __cplusplus
}
#endif

#endif
