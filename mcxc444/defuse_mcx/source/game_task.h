/*
 * Defuse to Wake - game task (priority 4, the highest).
 * Owns the game_t. Blocks on the input queue until the next 50 ms boundary, so it
 * reacts to an input at once yet still calls game_tick every 50 ms. After each
 * iteration it publishes a snapshot, updates feedback (LED / buzzer tick / sounds)
 * and forwards RESULTs to the ESP32. Not created in APP_DRIVER_TEST builds.
 * Rubric: Task design (priority), Sync (queues + mutex), RTOS config.
 */
#ifndef GAME_TASK_H
#define GAME_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_state.h"

typedef struct {
    uint32_t accepted[APP_CTRL_KIND_COUNT];  /* control messages the engine accepted */
    uint32_t rejected[APP_CTRL_KIND_COUNT];  /* ... and rejected (validation failed) */
    uint32_t result_send_drops;              /* RESULT could not be queued for the link */
    uint32_t tick_overruns;                  /* iterations that fell behind and resynced */
} game_task_stats_t;

bool game_task_start(void);

TaskHandle_t game_task_handle(void);

void game_task_get_stats(game_task_stats_t *out);

#endif /* GAME_TASK_H */
