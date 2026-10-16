/*
 * Defuse to Wake - buzzer (TPM2_CH0 on PTB2) and RGB LED (TPM0 CH4/CH5/CH2).
 * The feedback task (priority 1, 10 ms vTaskDelayUntil) is the ONLY code that
 * touches TPM0/TPM2. Every public call just posts a small request to the
 * feedback queue (length 8, never blocks), so any task can ask for a sound or
 * an LED mode without sharing registers. Sounds pre-empt the bomb-timer tick.
 * Rubric: Sensors/actuators (PWM buzzer + RGB LED), Sync (queue).
 */
#ifndef FEEDBACK_H
#define FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "game.h"

bool feedback_init(void);

bool feedback_start(void);

/* One-shot sound pattern; replaces a sound that is still playing. */
void feedback_play(game_sound_t s);

/* Bomb-timer tick: a 30 ms 2 kHz click every `ms`; 0 = off. */
void feedback_set_tick_interval(uint16_t ms);

void feedback_set_led(game_led_t mode);

/* Link status, shown only in LED mode OFF (blue blink while down). */
void feedback_set_link(bool up);

uint32_t feedback_drops(void);

TaskHandle_t feedback_task_handle(void);

#endif /* FEEDBACK_H */
