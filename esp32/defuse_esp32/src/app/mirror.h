/*
 * Defuse to Wake - Firebase Realtime Database WRITE-ONLY mirror of the morning records.
 * Records wait in a flash-backed queue (MIRROR_MAX = 14, the oldest is dropped with a log line
 * when full). mirror_service() runs in the network task: when Wi-Fi and the clock are good it
 * POSTs the OLDEST pending record to FIREBASE_DB_URL + "mornings.json?auth=" + FIREBASE_AUTH
 * every 60 s and right after a new record; HTTP 200 removes it, anything else keeps it and
 * backs off 60 s -> 120 -> ... -> 10 min. The database is never read, and the auth value is
 * never printed. Not thread-safe: network task only.
 */
#ifndef MIRROR_H
#define MIRROR_H

#include <stdbool.h>
#include <stdint.h>

#include "history_ring.h"

bool mirror_init(void);                          /* load the pending queue from flash */
void mirror_push(const HistRecord &rec);         /* queue + persist, upload on the next service() */
void mirror_wifi_up(void);                       /* Wi-Fi came back: forget the back-off, try now */
void mirror_service(uint32_t now_ms, bool net_ok);   /* at most one POST per call */
int mirror_pending(void);

#endif /* MIRROR_H */
