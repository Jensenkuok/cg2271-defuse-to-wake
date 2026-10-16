/*
 * Defuse to Wake - the single input event queue.
 * Every input driver (encoder, buttons, joystick, ultrasonic) posts game_event_t
 * items here; the game task (later) is the only consumer. Posting never blocks:
 * a full queue increments a drop counter instead.
 */
#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "game.h"

/* Create the queue (called once from main.c before the scheduler starts). */
bool input_queue_init(void);

QueueHandle_t input_queue_handle(void);

/* From an ISR: post without blocking; *woken is OR-ed for portYIELD_FROM_ISR. */
void input_post_from_isr(game_event_type_t type, int32_t value, BaseType_t *woken);

/* From a task: post without blocking. */
void input_post(game_event_type_t type, int32_t value);

uint32_t input_queue_drops(void);

#endif /* INPUT_QUEUE_H */
