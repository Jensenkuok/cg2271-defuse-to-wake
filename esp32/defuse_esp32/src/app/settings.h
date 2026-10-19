/*
 * Defuse to Wake - user settings kept in flash (Preferences namespace "defuse"):
 *   nusmods_link  NUSMods share link, at most APP_LINK_MAX_CHARS chars. Until one is saved the
 *                 NUSMODS_SHARE_LINK default from secrets.h is used.
 *   lead_min      wake-up lead before the first lecture, clamped to 15..180 min, default 60.
 * Every change bumps settings_version(); the network task watches it and answers a change
 * with a timetable refresh and an alarm reschedule. The future web page uses the same API.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Copy the current link into out (NUL-terminated, truncated to size - 1). */
void settings_get_link(char *out, size_t size);

/* False if url is empty, longer than APP_LINK_MAX_CHARS, or could not be stored. */
bool settings_set_link(const char *url);

int settings_get_lead_min(void);

/* Clamps to the allowed range; returns the value actually stored (or current on a flash error). */
int settings_set_lead_min(int minutes);

/* +1 per effective change (a write of the value already stored does not count). */
uint32_t settings_version(void);

#endif /* SETTINGS_H */
