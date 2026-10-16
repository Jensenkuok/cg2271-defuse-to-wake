/*
 * Defuse to Wake - PS2 joystick X/Y on ADC0 (PTE20 = SE0, PTE21 = SE4a).
 * POLLED on purpose: an analog stick has no "event" to interrupt on, so the
 * joystick task samples it at a fixed 50 Hz with vTaskDelayUntil (priority 3, the
 * highest input task, so sampling never jitters). Each conversion is started by
 * software; the ADC0 conversion-complete ISR stores the result and wakes the task
 * with vTaskNotifyGiveFromISR (no busy-waiting on COCO).
 * X crossings become JOY_LEFT / JOY_RIGHT edge events with hysteresis.
 * Rubric: Poll+IRQ (polled sensor), Task design.
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

uint32_t joystick_errors(void);

TaskHandle_t joystick_task_handle(void);

#endif /* JOYSTICK_H */
