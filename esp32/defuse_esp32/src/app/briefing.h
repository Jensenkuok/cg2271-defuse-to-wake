/*
 * Defuse to Wake - morning briefing + coach message (network task only, lowest priority).
 *
 * One LLM run (setter chain, temperature 0.7), repeated while the device is online until the text
 * stored for the next class day is LLM-written: a failed run stores the template and is retried
 * after APP_BRIEF_RETRY_MS (brief_needs_run in text_logic.h). The Serial command "brief" forces a
 * run. Input: the classes of the next alarm's day (<= 6), the alarm time, a summary of
 * the local morning history and the difficulty explanation. Output: JSON {"briefing","coach"},
 * validated (printable ASCII, briefing <= 120 chars, coach <= 80); a field that is missing or
 * invalid is replaced by a deterministic template built from the same data. The result is stored
 * in flash together with the local day it is for and shown on the OLED's DEFUSED screen.
 *
 * It also keeps a "today" template (today's timetable + history, no LLM) in app_state, which the
 * display uses when the stored text is for another day (brief_pick in text_logic.h).
 *
 * Like the puzzle generation it does at most ONE LLM call (<= 30 s) per pass and never runs during
 * a bomb session or close to the alarm.
 */
#ifndef BRIEFING_H
#define BRIEFING_H

#include <stdbool.h>
#include <stdint.h>

/* After flash_store_init() and app_state_init(): load the stored text into app_state. */
bool briefing_init(void);

/* Serial "brief": generate now (when the Wi-Fi, the clock and the game state allow it). */
void briefing_request(void);

/* One scheduling pass: refreshes the "today" template, starts a run when brief_needs_run says so
 * (this also covers a fresh boot) or on request, and advances a running one by one LLM call.
 * net_ok = Wi-Fi up and clock valid; gen_ok = no bomb session and the alarm is not imminent.
 * unix_now is 0 when the clock is not valid. Returns true when this pass made an LLM call, so the
 * caller does not make a puzzle call in the same pass. */
bool briefing_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok);

#endif /* BRIEFING_H */
