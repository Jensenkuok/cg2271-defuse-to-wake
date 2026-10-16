/*
 * Defuse to Wake - HC-SR04 ultrasonic ranger (TRIG = PTB3, ECHO = PTA13/TPM1_CH1).
 * The ultrasonic task (priority 2, equal to comm on purpose so time slicing is
 * visible) pings every 60 ms with vTaskDelayUntil. TPM1 free-runs at 1 MHz and
 * input-captures ECHO on both edges in an interrupt (exact edge timestamps; a
 * polled pin could never time a 58 us/cm pulse). The ISR computes the pulse width
 * and wakes the task with vTaskNotifyGiveFromISR. Every cycle posts DISTANCE
 * (cm, or 999 on timeout / beyond 400 cm).
 * Rubric: Poll+IRQ, ISR design, Task design.
 */
#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

void ultrasonic_init(void);

bool ultrasonic_start(void);

/* Last posted distance in cm (999 = no echo / out of range). */
int32_t ultrasonic_last_cm(void);

uint32_t ultrasonic_timeouts(void);

/* Cycles discarded by the capture rule (stale echo, wrong edge order). */
uint32_t ultrasonic_stale(void);

TaskHandle_t ultrasonic_task_handle(void);

#endif /* ULTRASONIC_H */
