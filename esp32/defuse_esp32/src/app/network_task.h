/*
 * Defuse to Wake - network task (lowest priority). ALL network and flash work lives here;
 * no other task ever blocks on either.
 *   - WiFiMulti with the two hotspots from secrets.h, reconnect handled by re-running it.
 *   - SNTP (configTime, UTC+8, two servers) -> app_state.time_valid.
 *   - Timetable: loads the flash cache at boot (works offline); with Wi-Fi + time it fetches
 *     NUSMods at boot, nightly at 22:00 and after a settings change (timetable.h).
 *   - Alarm: next_alarm() over the cached lessons (schedule.h) after the cache loads/refreshes,
 *     after every TIME_SYNC, after a settings change and after a DEFUSED result; the result is
 *     shown on the OLED and sent as SET_ALARM right after a TIME_SYNC (CLAUDE.md ESP32 rule).
 *   - RESULT frames from the link task -> local history ring (history.h) + Firebase queue
 *     (mirror.h, write-only).
 *   - Serial test commands: lead <min> | link <url> | refresh | resched | hist.
 *   - Drives the 10 s status print (status.cpp) since this task has the slack for it.
 * Nothing here is on the alarm path: the MCX rings with or without Wi-Fi.
 */
#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H

#include <Arduino.h>
#include <stdbool.h>

/* Initialises flash storage, timetable cache, history and mirror queue, then creates the task. */
bool network_task_start(void);

TaskHandle_t network_task_handle(void);

/* Web task (any task): ask the network task for a puzzle run now, like the serial command "puzzles".
 * Never blocks; the run itself starts when Wi-Fi, the clock and the game state allow it. */
void network_request_puzzles(void);

#endif /* NETWORK_TASK_H */
