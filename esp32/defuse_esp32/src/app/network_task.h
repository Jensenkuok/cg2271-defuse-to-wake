/*
 * Defuse to Wake - network task (lowest priority).
 *   - WiFiMulti with the two hotspots from secrets.h, reconnect handled by re-running it.
 *   - SNTP (configTime, UTC+8, two servers) -> app_state.time_valid.
 *   - Once time is valid AND the link is up: queue TIME_SYNC to the MCX, then (CLAUDE.md
 *     ESP32 rule) SET_ALARM right after it if an alarm is stored. Repeated every 10 min
 *     and after every link-up transition.
 *   - Drives the 10 s status print (status.cpp) since this task has the slack for it.
 * Nothing here is on the alarm path: the MCX rings with or without Wi-Fi.
 */
#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H

#include <Arduino.h>
#include <stdbool.h>

bool network_task_start(void);

TaskHandle_t network_task_handle(void);

#endif /* NETWORK_TASK_H */
