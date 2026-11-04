/*
 * Defuse to Wake - PS2 joystick X/Y and light sensor (LDR) on ADC0
 * (PTE20 = SE0, PTE21 = SE4a, PTB0 = SE8).
 * POLLED on purpose: an analog stick has no "event" to interrupt on, so the
 * joystick task samples it at a fixed 50 Hz with vTaskDelayUntil (priority 3, the
 * highest input task, so sampling never jitters). The same task also reads the LDR
 * every 5th loop (10 Hz): light changes slowly, so polling is enough and an interrupt
 * would add nothing. ADC0 therefore has exactly ONE owner (this task), so no mutex is
 * needed. Each conversion is started by software; the ADC0 conversion-complete ISR
 * stores the result and wakes the task with vTaskNotifyGiveFromISR (no busy-waiting
 * on COCO).
 * X crossings become JOY_LEFT / JOY_RIGHT edge events with hysteresis. The LDR is
 * averaged over 4 samples, scaled to a 0..255 level and posted as GAME_EVT_LIGHT.
 * Rubric: Poll+IRQ (polled sensors), Task design.
 */
#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

/* ADC0 clock, reference and one-time calibration (call before the scheduler). */
bool joystick_init(void);

bool joystick_start(void);

/* Latest raw 12-bit X/Y (telemetry). */
void joystick_get_raw(uint16_t *x, uint16_t *y);

/* Latest LDR reading: averaged raw 12-bit value and the level 0..255 that was posted. */
void joystick_get_light(uint16_t *raw, uint8_t *level);

/* GAME_EVT_LIGHT samples dropped because the input queue was full. */
uint32_t joystick_light_drops(void);

uint32_t joystick_errors(void);

TaskHandle_t joystick_task_handle(void);

#endif /* JOYSTICK_H */
