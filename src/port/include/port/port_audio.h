/* port_audio.h — playing a local file (inline audio clips), recording the
 * microphone (voice input), and notification sounds. Optional:
 * PORT_CAP2_AUDIO_PLAY / _CAPTURE / _SOUNDS; without them the core shows
 * "Audio playback is unavailable" and hides the microphone.
 *
 * Threading: like port_app.h — calls on the loop thread, callbacks posted to
 * it (never from inside the call that caused them). Destroy players and
 * recorders before their port_app. */
#ifndef PORT_PORT_AUDIO_H
#define PORT_PORT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct port_app port_app;

/* Why playback or capture failed; the same order as plat::audio::Error. */
enum {
    PORT_AUDIO_UNAVAILABLE = 0,
    PORT_AUDIO_UNSUPPORTED,
    PORT_AUDIO_NEEDS_FFMPEG,
    PORT_AUDIO_NO_OUTPUT,
    PORT_AUDIO_OUTPUT_FAILED,
    PORT_AUDIO_PLAYBACK_FAILED,
    PORT_AUDIO_NO_CAPTURE_TOOL,
    PORT_AUDIO_NO_MICROPHONE,
    PORT_AUDIO_MICROPHONE_BUSY,
    PORT_AUDIO_MICROPHONE_DENIED,
    PORT_AUDIO_MICROPHONE_OPEN,
    PORT_AUDIO_START_FAILED,
    PORT_AUDIO_NO_AUDIO,
    PORT_AUDIO_STOPPED,
    PORT_AUDIO_READ_FAILED,
    PORT_AUDIO_CAPTURE_FAILED,
    PORT_AUDIO_OS /* detail is the OS's own complete message */
};

/* ── Playback ──────────────────────────────────────────────────────────── */
typedef struct port_player port_player;
typedef struct port_player_callbacks {
    uint32_t size;
    void    *user;
    void (*on_loaded)(void *user); /* play() may be called; duration known if ever */
    void (*on_ended)(void *user);
    void (*on_failed)(void *user, int code, const char *detail);
} port_player_callbacks;
int     port_player_create(port_app *a, const port_player_callbacks *cb, port_player **out);
void    port_player_destroy(port_player *p);
void    port_player_load(port_player *p, const char *path);
void    port_player_play(port_player *p);
void    port_player_pause(port_player *p);
void    port_player_seek(port_player *p, int64_t ms);
void    port_player_stop(port_player *p);
int64_t port_player_position_ms(port_player *p);
int64_t port_player_duration_ms(port_player *p); /* 0 = unknown */
/* Whether files with this lower-case extension ("mp3", "m4a") play. */
int     port_audio_can_play(const char *ext);

/* ── Capture: 16 kHz mono signed 16-bit little-endian PCM ──────────────── */
typedef struct port_recorder port_recorder;
typedef struct port_recorder_callbacks {
    uint32_t size;
    void    *user;
    void (*on_started)(void *user);
    void (*on_level)(void *user, float peak); /* 0..1, ~20 times a second */
    /* After port_recorder_stop: everything captured (PCM, no header). */
    void (*on_finished)(void *user, const void *pcm, size_t bytes);
    void (*on_failed)(void *user, int code, const char *detail);
} port_recorder_callbacks;
int  port_recorder_create(port_app *a, const port_recorder_callbacks *cb, port_recorder **out);
void port_recorder_destroy(port_recorder *r);
void port_recorder_start(port_recorder *r);
void port_recorder_stop(port_recorder *r);
void port_recorder_cancel(port_recorder *r); /* no callback follows */

/* ── Notification sounds ───────────────────────────────────────────────── */
/* fn(user, name, label) per selectable system sound. BLOCKING (reads the
 * OS's sound settings): the core calls it off the loop thread. */
void port_sound_list(void (*fn)(void *user, const char *name, const char *label), void *user);
/* Fire and forget: the system sound `name`, else the WAV file `fallback`. */
void port_sound_play(const char *name, const char *fallback_path);

#ifdef __cplusplus
}
#endif

#endif
