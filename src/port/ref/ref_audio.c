/* port_audio.h for a machine without sound: nothing plays, nothing records
 * (PORT_CAP2_AUDIO_* clear). The players and recorders still exist and
 * fail asynchronously with PORT_AUDIO_UNAVAILABLE, as the contract asks, so
 * the core's error paths are exercised. */
#define _POSIX_C_SOURCE 200809L
#include "ref_internal.h"

#include <stdlib.h>

struct port_player {
    port_app             *app;
    port_player_callbacks cb;
    uint64_t              pending; /* the timer of a queued failure */
};

struct port_recorder {
    port_app               *app;
    port_recorder_callbacks cb;
    uint64_t                pending;
};

static void playerFail(void *p) {
    port_player *pl = (port_player *)p;
    pl->pending     = 0;
    if (pl->cb.on_failed)
        pl->cb.on_failed(pl->cb.user, PORT_AUDIO_UNAVAILABLE, "");
}

static void recorderFail(void *p) {
    port_recorder *r = (port_recorder *)p;
    r->pending       = 0;
    if (r->cb.on_failed)
        r->cb.on_failed(r->cb.user, PORT_AUDIO_NO_MICROPHONE, "");
}

int port_player_create(port_app *a, const port_player_callbacks *cb, port_player **out) {
    port_player *p = (port_player *)calloc(1, sizeof *p);
    if (!p)
        return PORT_E_NOMEM;
    p->app = a;
    p->cb  = *cb;
    *out   = p;
    return PORT_OK;
}

void port_player_destroy(port_player *p) {
    if (!p)
        return;
    if (p->pending)
        port_timer_cancel(p->app, p->pending);
    free(p);
}

void port_player_load(port_player *p, const char *path) {
    (void)path;
    if (!p->pending)
        p->pending = port_timer_add(p->app, 0, 0, playerFail, p);
}

void port_player_play(port_player *p) {
    (void)p;
}

void port_player_pause(port_player *p) {
    (void)p;
}

void port_player_seek(port_player *p, int64_t ms) {
    (void)p, (void)ms;
}

void port_player_stop(port_player *p) {
    (void)p;
}

int64_t port_player_position_ms(port_player *p) {
    (void)p;
    return 0;
}

int64_t port_player_duration_ms(port_player *p) {
    (void)p;
    return 0;
}

int port_audio_can_play(const char *ext) {
    (void)ext;
    return 0;
}

int port_recorder_create(port_app *a, const port_recorder_callbacks *cb, port_recorder **out) {
    port_recorder *r = (port_recorder *)calloc(1, sizeof *r);
    if (!r)
        return PORT_E_NOMEM;
    r->app = a;
    r->cb  = *cb;
    *out   = r;
    return PORT_OK;
}

void port_recorder_destroy(port_recorder *r) {
    if (!r)
        return;
    port_recorder_cancel(r);
    free(r);
}

void port_recorder_start(port_recorder *r) {
    if (!r->pending)
        r->pending = port_timer_add(r->app, 0, 0, recorderFail, r);
}

void port_recorder_stop(port_recorder *r) {
    (void)r;
}

void port_recorder_cancel(port_recorder *r) {
    if (r->pending)
        port_timer_cancel(r->app, r->pending);
    r->pending = 0;
}

void port_sound_list(void (*fn)(void *, const char *, const char *), void *user) {
    (void)fn, (void)user;
}

void port_sound_play(const char *name, const char *fallback_path) {
    (void)name, (void)fallback_path;
}
