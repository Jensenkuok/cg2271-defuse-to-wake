/*
 * Defuse to Wake - local web page task (priority APP_PRIO_WEB = the network task's level, the lowest).
 * The built-in WebServer (no async library, no CDN) serves ONE page from flash plus a small JSON
 * API; ESPmDNS announces it as http://defuse.local once Wi-Fi is up. CLAUDE.md "Local web page".
 *
 *   GET  /                  the page (web_page.h), two tabs: My mornings, Engineering
 *   GET  /api/status        everything the page shows, one JSON object (<= APP_WEB_STATUS_MAX bytes)
 *   POST /api/settings      nusmods_link + lead_min (validated, saved through settings.h, which makes
 *                           the network task refresh the timetable and reschedule the alarm)
 *   POST /api/ring          the BOOT-button demo ring (link task)
 *   POST /api/fault/corrupt arm "corrupt the next frame to the MCX" (fault.h)
 *   POST /api/fault/outage  60 s simulated internet outage (fault.h)
 *   POST /api/puzzles       ask the network task for a puzzle run (non-blocking)
 *
 * Rules this task keeps: it only READS shared state through copies taken under the existing locks
 * (app_state_read, history_recent, timetable_copy, puzzle_set_snapshot, link_frame_log_copy), each
 * held for the copy only; every buffer is fixed-size and static; nothing here can block the game,
 * the link or the display (they never wait on this task); and no secret (API keys, Firebase auth,
 * Wi-Fi passwords) is ever put in a response or a log line. The ACTIVE puzzle's answer is reported
 * as "hidden" while the MCX is ARMED or in a bomb session (answer_hidden in web_logic.h).
 */
#ifndef WEB_TASK_H
#define WEB_TASK_H

#include <Arduino.h>
#include <stdbool.h>

/* Registers the routes and creates the task. The server and mDNS start when Wi-Fi connects. */
bool web_task_start(void);

TaskHandle_t web_task_handle(void);

#endif /* WEB_TASK_H */
