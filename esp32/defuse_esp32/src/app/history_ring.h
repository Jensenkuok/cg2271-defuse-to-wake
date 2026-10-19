/*
 * Defuse to Wake - morning history records (pure logic, host-tested).
 * One HistRecord per DEFUSED morning. The same record type is the history ring (last 14
 * mornings, source of truth for the coach/difficulty/web page) and the Firebase mirror queue
 * (records not uploaded yet). Persistence and Wi-Fi live elsewhere (history.cpp, mirror.cpp).
 */
#ifndef HISTORY_RING_H
#define HISTORY_RING_H

#include <stddef.h>
#include <stdint.h>

#include "fixed_ring.h"
#include "schedule.h"   /* SCHED_MODULE_LEN, SCHED_TZ_OFFSET_S, SCHED_SEC_PER_DAY */

#define HIST_LEN            14      /* mornings kept locally */
#define MIRROR_MAX          14      /* records waiting for Firebase */
#define HIST_DEVICE_ID      "defuse-1"

struct HistRecord {
    uint32_t end_unix;              /* when the bomb was defused; 0 if the clock was not valid */
    uint16_t duration_s;            /* length of the final (defused) bomb */
    uint8_t strikes;                /* summed over the exploded bombs and the final one, saturating */
    uint8_t snoozes;
    uint8_t explosions;
    char module[SCHED_MODULE_LEN];  /* lesson that rang the alarm, "" for a manual/demo ring */
};

typedef FixedRing<HistRecord, HIST_LEN> HistoryRing;
typedef FixedRing<HistRecord, MIRROR_MAX> MirrorQueue;

/* Per-session accumulator: EXPLODED results build up until a DEFUSED one closes the morning. */
struct MorningTracker {
    uint8_t explosions;
    uint8_t strikes;
};

/* Feed one RESULT. EXPLODED (defused = false) only accumulates and returns false. DEFUSED fills
 * *rec (module is reduced to [A-Za-z0-9], at most SCHED_MODULE_LEN - 1 chars), resets the
 * tracker and returns true. */
bool morning_on_result(MorningTracker *t, bool defused, uint16_t duration_s, uint8_t strikes, uint8_t snoozes,
                       uint32_t end_unix, const char *module, HistRecord *rec);

/* Consecutive local days (UTC+8) with a record, counted back from the newest record's day.
 * With now_unix != 0 the streak is 0 unless that day is today or yesterday. Records with
 * end_unix == 0 are ignored. */
int history_streak(const HistoryRing &ring, uint32_t now_unix);

/* Firebase body {"ts":..,"duration_s":..,"strikes":..,"snoozes":..,"explosions":..,"module":"..",
 * "device":".."}. Returns the length, or 0 if it does not fit in size bytes. */
size_t history_to_json(const HistRecord &r, char *buf, size_t size);

#endif /* HISTORY_RING_H */
