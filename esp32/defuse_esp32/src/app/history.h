/*
 * Defuse to Wake - local morning history in flash (source of truth for the coach, the
 * difficulty rule and the web page). Ring of the last HIST_LEN (14) mornings, one record per
 * DEFUSED result; EXPLODED results are accumulated until the morning is defused.
 * history_on_result() belongs to the network task (it writes flash); the read functions are
 * thread-safe and cheap, so the web page task can call them.
 */
#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "history_ring.h"

bool history_init(void);   /* creates the mutex, loads the ring from flash (empty if none) */

/* Feed one RESULT. On DEFUSED the record is stored in flash and copied to *rec_out; returns
 * true then. On EXPLODED it only counts the explosion and returns false. */
bool history_on_result(bool defused, uint16_t duration_s, uint8_t strikes, uint8_t snoozes, uint32_t end_unix,
                       const char *module, HistRecord *rec_out);

int history_count(void);

/* Newest first; returns how many records were copied (<= n). */
int history_recent(HistRecord *out, int n);

/* Consecutive days with a DEFUSED record (see history_streak). now_unix = 0 if the time is unknown. */
int history_current_streak(uint32_t now_unix);

#endif /* HISTORY_H */
