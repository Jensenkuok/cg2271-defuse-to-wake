/*
 * Defuse to Wake - user settings kept in flash (Preferences namespace "defuse"):
 *   nusmods_link  NUSMods share link, at most APP_LINK_MAX_CHARS chars. Until one is saved the
 *                 NUSMODS_SHARE_LINK default from secrets.h is used.
 *   lead_min      wake-up lead before the first lecture, clamped to 15..180 min, default 60.
 *   diff_mode     difficulty mode (DiffMode): auto / easy / normal / hard, default auto. Kept in a RAM
 *                 copy too, so the link task can read it without touching flash.
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

/* Network task, once after flash_store_init(): load the difficulty mode into its RAM copy. Until then
 * settings_get_difficulty_mode() answers DIFF_MODE_DEFAULT. */
void settings_load_difficulty(void);

/* The RAM copy (one byte: any task may read it, no flash access). */
int settings_get_difficulty_mode(void);

/* Stores a DiffMode; an invalid number is refused. Returns the mode now in force. */
int settings_set_difficulty_mode(int mode);

/* +1 per effective change of the difficulty mode. Separate from settings_version() on purpose: that one
 * makes the network task refresh the NUSMods timetable, which a difficulty change must not do. */
uint32_t settings_difficulty_version(void);

/* +1 per effective change (a write of the value already stored does not count). */
uint32_t settings_version(void);

#endif /* SETTINGS_H */
