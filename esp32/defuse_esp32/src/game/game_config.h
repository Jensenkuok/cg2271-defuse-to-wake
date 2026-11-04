/*
 * Defuse to Wake - game engine tunables. Every number used by game.c lives here.
 * Rules are the team-approved ones in CLAUDE.md ("Game rules").
 */
#ifndef GAME_CONFIG_H
#define GAME_CONFIG_H

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Unit conversions ---- */
#define GAME_MS_PER_S               1000u
#define GAME_PERCENT                100u
#define GAME_VALUE_HI_SHIFT         8u     /* module_value = low byte | (high << 8) */

/* ---- Default (offline) bomb ---- */
#define GAME_DEFAULT_MODULE_COUNT   4u     /* DIAL, WIRES, TRIPWIRE, LIGHTS */
#define GAME_DEFAULT_TIME_LIMIT_S   180u
#define GAME_DEFAULT_DIAL_CODE_0    4u
#define GAME_DEFAULT_DIAL_CODE_1    2u
#define GAME_DEFAULT_DIAL_CODE_2    7u
#define GAME_DEFAULT_DIAL_CODE_3    1u
#define GAME_DEFAULT_WIRE_COUNT     4u
#define GAME_DEFAULT_CORRECT_WIRE   2u     /* zero-based */
#define GAME_DEFAULT_TRIP_MIN_CM    15u
#define GAME_DEFAULT_TRIP_MAX_CM    25u
#define GAME_DEFAULT_MAX_STRIKES    3u

/* ---- Config validation limits (game_set_config) ---- */
#define GAME_MIN_MODULES            1u     /* upper bound is PROTO_MAX_MODULES */
#define GAME_MODULE_TYPE_MIN        PROTO_MODULE_DIAL
#define GAME_MODULE_TYPE_MAX        PROTO_MODULE_LIGHTS
#define GAME_MODULE_TYPE_COUNT      4u
#define GAME_DIAL_BASE              10u    /* digits 0..9 */
/* Repeated dial at module index k uses digit (base + STEP*k) % 10. Steps 3, 6 and 9 are
 * non-zero mod 10, so the codes at indices 0..3 all differ from each other. */
#define GAME_DIAL_REPEAT_STEP       3u
#define GAME_MIN_TIME_LIMIT_S       30u
#define GAME_MAX_TIME_LIMIT_S       600u
#define GAME_MIN_WIRES              3u
#define GAME_MAX_WIRES              6u
#define GAME_MIN_STRIKES            1u
#define GAME_MAX_STRIKES_LIMIT      5u
#define GAME_TRIP_BOUND_MIN_CM      5u
#define GAME_TRIP_BOUND_MAX_CM      100u

/* ---- Tripwire ---- */
#define GAME_TRIP_HOLD_MS           3000u
#define GAME_TRIP_GRACE_MS          300u   /* out-of-band time forgiven before the hold resets */
#define GAME_TRIP_MAX_VALID_CM      400    /* readings above this are noise */
#define GAME_VIEW_MAX_DISTANCE_CM   255    /* distance field of module_value */

/* ---- Lights on ---- */
/* The target is relative to the room: baseline (first reading after the module starts) plus
 * DELTA, never below MIN_LEVEL (a dark room still needs real light) and never above TARGET_CAP
 * (a bright room must stay reachable: a phone torch reads near 255). Levels are 0..255. */
#define GAME_LIGHT_DELTA            40u
#define GAME_LIGHT_MIN_LEVEL        120u
#define GAME_LIGHT_TARGET_CAP       240u
#define GAME_LIGHT_HOLD_MS          2000u
#define GAME_LIGHT_GRACE_MS         300u   /* below-target time forgiven before the hold resets */
#define GAME_LIGHT_LEVEL_MAX        255u

/* ---- Tamper (inert: the gyro is disabled, the engine path stays for tests) ---- */
#define GAME_TAMPER_THRESHOLD_MG    1500
#define GAME_TAMPER_COOLDOWN_MS     2000u

/* ---- State timing ---- */
#define GAME_EXPLODED_HOLD_MS       3000u  /* EXPLODED -> new bomb */
#define GAME_DEFUSED_HOLD_MS        30000u /* DEFUSED -> IDLE */
#define GAME_SNOOZE_MS              300000u
#define GAME_MAX_SNOOZES            2u
#define GAME_RING_NOW_MAX_S         3600
#define GAME_MAX_ALARM_AHEAD_S      604800u /* 7 days: keeps ms deadlines < 2^31 ahead */

/* ---- Buzzer tick interval (game_tick_interval_ms) ---- */
#define GAME_TICK_SLOW_AFTER_MS     10000u /* more time left than this: slow tick */
#define GAME_TICK_SLOW_MS           1000u
#define GAME_TICK_FAST_BASE_MS      150u   /* interval when time is almost out */
#define GAME_TICK_FAST_SPAN_MS      850u   /* added linearly up to the 10 s mark */

/* ---- Output FIFOs ---- */
#define GAME_SOUND_FIFO_DEPTH       8u
#define GAME_RESULT_FIFO_DEPTH      4u

#ifdef __cplusplus
}
#endif

#endif /* GAME_CONFIG_H */
