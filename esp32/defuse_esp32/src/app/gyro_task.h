/*
 * Defuse to Wake - MPU-6050 tamper detector (priority 3, 50 Hz via vTaskDelayUntil).
 * deviation = | |a| - 1 g | in mg. While the bomb is BOMB_ACTIVE, a deviation >= 600 mg
 * is sent to the MCX as TAMPER_EVENT (500 ms local cooldown). The MCX applies its own
 * 1500 mg threshold and 2 s cooldown; the peak deviation is logged every 5 s so the
 * team can tune both numbers. If the sensor is missing the task logs once and deletes
 * itself, and everything else keeps running.
 */
#ifndef GYRO_TASK_H
#define GYRO_TASK_H

#include <Arduino.h>
#include <stdbool.h>

bool gyro_task_start(void);

TaskHandle_t gyro_task_handle(void);

#endif /* GYRO_TASK_H */
