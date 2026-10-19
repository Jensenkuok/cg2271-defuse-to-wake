/*
 * Defuse to Wake - timetable: NUSMods fetch + flash cache of the selected lesson slots.
 *   timetable_load_cache()   boot: restore the last good lesson list from flash (works offline)
 *   timetable_refresh()      network task only: GET every distinct module in the share link
 *                            (https://api.nusmods.com/v2/<AY>/modules/<CODE>.json, <= 10 s each),
 *                            keep semester-1 lessons whose type + class number match a slot of
 *                            the link, and on FULL success replace the in-RAM list and the flash
 *                            cache. Any failure leaves the previous list and cache untouched.
 *   timetable_copy()         thread-safe snapshot of the lesson list for the scheduler / web page.
 * Network use is confined to timetable_refresh(); everything else is flash/RAM only.
 */
#ifndef TIMETABLE_H
#define TIMETABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "schedule.h"

bool timetable_init(void);

/* True if a valid cache was found and loaded. */
bool timetable_load_cache(void);

/* Blocks for up to (#modules x 10 s). Call from the network task with Wi-Fi up and time valid. */
bool timetable_refresh(const char *share_link, uint32_t now_unix);

/* Copies up to max lessons into out; returns the number copied. */
int timetable_copy(Lesson *out, int max);

int timetable_count(void);

/* Unix time of the last successful fetch (0 = never; the list then comes from nothing/old cache). */
uint32_t timetable_fetched_unix(void);

#endif /* TIMETABLE_H */
