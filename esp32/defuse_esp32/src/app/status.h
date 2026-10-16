/*
 * Defuse to Wake - periodic Serial status block (every 10 s): link counters, Wi-Fi and
 * time state, per-task stack headroom (BYTES on ESP32) and free heap.
 */
#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>

/* Call often (e.g. every loop of the network task); prints when 10 s have passed. */
void status_tick(uint32_t now_ms);

#endif /* STATUS_H */
