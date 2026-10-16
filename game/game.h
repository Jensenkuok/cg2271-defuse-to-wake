/*
 * Defuse to Wake - hardware-independent bomb game engine (pure C99, host
 * tested). The MCX tasks feed it events and time, and act on its outputs.
 *
 * THREADING: a game_t is NOT thread-safe, by design. Exactly one MCX task (the
 * game task) owns it and calls every function below, including the getters.
 * Other tasks talk to that task through queues; ISRs never touch the game.
 *
 * TIME: all time arrives as a uint32_t now_ms argument; comparisons are
 * wraparound-safe. game_tick is called by the game task every 50 ms. Note that
 * game_tick_interval_ms is the BUZZER tick period, not the call period.
 * game_set_alarm has no now_ms, so it uses the now_ms of the most recent call
 * that carried one (at most one tick old).
 *
 * No dynamic allocation, no globals, no OS calls, no randomness. See
 * docs/game.md for the state diagram and the full rules.
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "game_config.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GAME_EVT_ENC_STEP = 0,     /* value +1 / -1 */
    GAME_EVT_ENC_PRESS,
    GAME_EVT_ENC_LONG_PRESS,   /* snooze */
    GAME_EVT_JOY_LEFT,
    GAME_EVT_JOY_RIGHT,
    GAME_EVT_JOY_PRESS,
    GAME_EVT_DISTANCE,         /* value = cm */
    GAME_EVT_TAMPER,           /* value = mg */
    GAME_EVT_CMD_RING_NOW,     /* value = delay in seconds, clamped 0..3600 */
    GAME_EVT_CMD_SNOOZE,
    GAME_EVT_CMD_CANCEL
} game_event_type_t;

typedef struct {
    game_event_type_t type;
    int32_t value;
} game_event_t;

typedef enum {
    GAME_LED_OFF = 0,
    GAME_LED_ARMED_DIM_BLUE,
    GAME_LED_ACTIVE_RED_BLINK,
    GAME_LED_DEFUSED_GREEN,
    GAME_LED_EXPLODED_RED_WHITE
} game_led_t;

typedef enum {
    GAME_SOUND_CLICK = 0,
    GAME_SOUND_DIGIT_OK,
    GAME_SOUND_STRIKE,
    GAME_SOUND_MODULE_CLEARED,
    GAME_SOUND_DEFUSED,
    GAME_SOUND_EXPLODED,
    GAME_SOUND_DENIED
} game_sound_t;

/* Fields are private; use the functions below. */
typedef struct {
    uint8_t state;                      /* PROTO_STATE_* */
    uint32_t last_now_ms;               /* most recent now_ms seen */

    proto_bomb_config_t cfg;            /* validated config for the next bomb */
    proto_bomb_config_t bomb_cfg;       /* snapshot used by the running bomb */

    bool have_time;
    uint32_t sync_unix;
    uint32_t sync_ms;

    uint32_t alarm_deadline_ms;         /* ARMED */
    uint32_t state_end_ms;              /* EXPLODED / DEFUSED hold */

    bool session_active;
    uint32_t session_start_ms;
    uint32_t session_strikes;
    uint8_t snoozes;
    uint8_t extras;                     /* snoozes + explosions this session */

    uint8_t modules[PROTO_MAX_MODULES];
    uint8_t module_total;
    uint8_t cleared;
    uint32_t bomb_end_ms;
    uint8_t bomb_strikes;

    uint8_t dial_value;
    uint8_t dial_digit;
    uint8_t wire_cursor;
    uint8_t wire_cut_mask;
    uint8_t trip_distance_cm;
    bool trip_holding;
    uint32_t trip_hold_start_ms;

    bool tamper_cooling;
    uint32_t tamper_cooldown_end_ms;

    game_sound_t sounds[GAME_SOUND_FIFO_DEPTH];
    uint8_t sound_head;
    uint8_t sound_count;
    proto_result_msg_t results[GAME_RESULT_FIFO_DEPTH];
    uint8_t result_head;
    uint8_t result_count;

    proto_game_state_t view_cache;      /* last view, for version tracking */
    uint32_t view_version;
} game_t;

/* IDLE, default bomb config loaded, no time sync, empty FIFOs. */
void game_init(game_t *g, uint32_t now_ms);

/*
 * Validate and store a bomb config for the NEXT bomb (a running bomb keeps its
 * snapshot). Rejected (false, previous config kept) if module_count is not 1..4,
 * a used module type is not 1..3, a dial digit > 9, trip_min >= trip_max, or a
 * trip bound is outside 5..100. Otherwise clamps time_limit_s 30..600,
 * wire_count 3..6, max_strikes 1..5; correct_wire must be < clamped wire_count.
 */
bool game_set_config(game_t *g, const proto_bomb_config_t *cfg);

/* TIME_SYNC. Returns false for unix_s == 0. */
bool game_set_time(game_t *g, uint32_t unix_s, uint32_t now_ms);

/*
 * SET_ALARM. IDLE (or ARMED, which re-schedules) -> ARMED. False if there is no
 * time sync, the alarm is in the past, more than 7 days ahead, or the game is
 * in BOMB_ACTIVE / DEFUSED / EXPLODED.
 */
bool game_set_alarm(game_t *g, uint32_t alarm_unix);

void game_handle_event(game_t *g, const game_event_t *ev, uint32_t now_ms);

/* Call every 50 ms: alarm deadline, tripwire hold, bomb timer, state timeouts. */
void game_tick(game_t *g, uint32_t now_ms);

void game_get_view(const game_t *g, proto_game_state_t *out, uint32_t now_ms);

/* Increments whenever the view changed as of the last tick/event/mutator. */
uint32_t game_view_version(const game_t *g);

/* Buzzer tick period in ms in BOMB_ACTIVE; 0 (no ticking) in every other state. */
uint16_t game_tick_interval_ms(const game_t *g, uint32_t now_ms);

game_led_t game_led_mode(const game_t *g);

/* One-shot sound FIFO (depth 8, newest dropped when full). False when empty. */
bool game_pop_sound(game_t *g, game_sound_t *s);

/* RESULT FIFO (depth 4, newest dropped when full). False when empty. */
bool game_pop_result(game_t *g, proto_result_msg_t *r);

#ifdef __cplusplus
}
#endif

#endif /* GAME_H */
