/*
 * Defuse to Wake - state shared between tasks:
 *   1. the game-state SNAPSHOT (writer: game task; readers: comm task, debug task),
 *      guarded by a FreeRTOS mutex;
 *   2. the CONTROL queue (writer: comm task; reader: game task) carrying the
 *      rarely-sent ESP32 messages TIME_SYNC, SET_ALARM and BOMB_CONFIG.
 * Rubric: Sync (mutex + queue).
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "game.h"
#include "protocol.h"

typedef struct {
    proto_game_state_t view;   /* what GAME_STATE carries */
    uint32_t version;          /* game_view_version() when this was written */
    uint16_t tick_ms;          /* buzzer tick period (0 = off) */
    game_led_t led;
    uint8_t module_total;      /* modules in the running bomb (debug display only) */
} app_state_t;

typedef enum {
    APP_CTRL_TIME_SYNC = 0,
    APP_CTRL_SET_ALARM,
    APP_CTRL_BOMB_CONFIG,
    APP_CTRL_KIND_COUNT
} app_ctrl_kind_t;

typedef struct {
    app_ctrl_kind_t kind;
    union {
        uint32_t unix_s;                 /* APP_CTRL_TIME_SYNC */
        uint32_t alarm_unix;             /* APP_CTRL_SET_ALARM */
        proto_bomb_config_t cfg;         /* APP_CTRL_BOMB_CONFIG */
    } u;
} app_ctrl_t;

/* Creates the mutex and the control queue; call once before the scheduler starts. */
bool app_state_init(void);

/* Game task only. Holds the mutex just for the struct copy. */
void app_state_write(const app_state_t *s);

/* Any task. False (and *out untouched) if the mutex could not be taken in time. */
bool app_state_read(app_state_t *out);

/* Comm task: queue a control message without blocking. False if the queue is full. */
bool app_ctrl_post(const app_ctrl_t *c);

/* Game task: non-blocking receive. */
bool app_ctrl_receive(app_ctrl_t *c);

uint32_t app_state_lock_timeouts(void);

#endif /* APP_STATE_H */
