/*
 * Soar iOS C bridge — DESIGN DRAFT, NOT WIRED (see ios/README.md).
 *
 * This header sketches the C ABI over the C++ core (`soar::Player`,
 * include/soar/core/player.h) for the future `CxxEngine` / xcframework
 * batch. It is intentionally NOT part of the SoarKit build: nothing
 * implements it yet. The shape follows the Player facade one-to-one;
 * state/event enums mirror backend.h.
 *
 * Ownership: functions returning `soar_player*` hand out an owning handle;
 * `soar_player_destroy` ends it. Strings are UTF-8, NUL-terminated, and
 * borrowed by the callee unless a `_copy` variant says otherwise.
 * The event callback may be invoked on a non-main thread — the Swift side
 * hops to its delivery context (PlayerController.deliver).
 */
#ifndef SOAR_BRIDGE_H
#define SOAR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct soar_player soar_player;

typedef enum {
    SOAR_STATE_STOPPED = 0,
    SOAR_STATE_PAUSED = 1,
    SOAR_STATE_PLAYING = 2,
    SOAR_STATE_ENDED = 3,
    SOAR_STATE_ERROR = 4
} soar_state;

typedef enum {
    SOAR_EV_STATE_CHANGED = 0,
    SOAR_EV_MEDIA_INFO_CHANGED = 1,
    SOAR_EV_POSITION_CHANGED = 2,
    SOAR_EV_ERROR = 3,
    SOAR_EV_BUFFERING_STARTED = 4,
    SOAR_EV_BUFFERING_ENDED = 5,
    SOAR_EV_DOWNLOAD_PROGRESS = 6
} soar_event_type;

typedef struct {
    soar_event_type type;
    long long position_ms;
    long long downloaded;
    long long total;
    const char* message; /* valid for the duration of the callback only */
} soar_event;

typedef void (*soar_event_fn)(const soar_event* event, void* user);

/* Backend selection: 0 = null (state machine, no media), 1 = ffmpeg
 * (requires the FFmpeg xcframeworks from the cross-compile batch). */
soar_player* soar_player_create(int backend_kind);
void soar_player_destroy(soar_player* player);

void soar_player_set_event_callback(soar_player* player,
                                    soar_event_fn callback, void* user);

int soar_player_open(soar_player* player, const char* uri,
                     const char* cache_dir);
void soar_player_close(soar_player* player);
int soar_player_play(soar_player* player);
int soar_player_pause(soar_player* player);
int soar_player_stop(soar_player* player);

int soar_player_seek(soar_player* player, long long position_ms);
int soar_player_set_rate(soar_player* player, double rate);
int soar_player_set_volume(soar_player* player, double volume01);
int soar_player_set_muted(soar_player* player, int muted);

long long soar_player_position(const soar_player* player);
long long soar_player_duration(const soar_player* player);
soar_state soar_player_state(const soar_player* player);
const char* soar_player_last_error(const soar_player* player);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SOAR_BRIDGE_H */
