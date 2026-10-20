/*
 * Defuse to Wake - link task (highest priority). The ONLY owner of the proto_link_t,
 * over Serial1. Every other task sends through the TX queue (link_send); received
 * frames are delivered into the shared app state.
 * Also keeps the frame log of the link (last 20 frames, see frame_log.h) and applies the web page's
 * "corrupt next packet" fault in its write callback (see fault.h).
 * Also polls the BOOT button (GPIO0) every 50 ms as a demo trigger and answers a press itself:
 * a fresh BOMB_CONFIG for the next puzzle pair, then CMD RING_NOW. It runs bomb_config_service()
 * too (ACK bookkeeping, explosion / snooze triggers), because the MCX starts the next bomb 3 s
 * after EXPLODED and only this task is guaranteed never to be stuck in an LLM call or a flash write.
 * It reports each BOMB_CONFIG's outcome (ACKed / dropped after the retries) to app_state.
 */
#ifndef LINK_TASK_H
#define LINK_TASK_H

#include <Arduino.h>
#include <stdbool.h>
#include <stdint.h>

#include "frame_log.h"
#include "../shared/protocol.h"

/* Create the TX queue. Call before any task that may send. */
bool link_task_init(void);

bool link_task_start(void);

/* Any task: queue a message for the MCX (copied, never blocks). False if the queue is full. */
bool link_send(uint8_t type, const uint8_t *payload, uint8_t len);

/* Network task: next RESULT frame received from the MCX, oldest first (never blocks).
 * The link task only queues them; recording them (flash writes) happens in the network task. */
bool link_result_pop(proto_result_msg_t *out);

/* Free slots in the TX queue (a task that needs to queue a pair checks this first). */
uint32_t link_tx_space(void);

TaskHandle_t link_task_handle(void);

/* Web task: copy of the frame log, newest first, at most max entries (see frame_log.h). Takes a
 * critical section for the copy only (a few dozen bytes). Returns the number copied. */
int link_frame_log_copy(FrameLogEntry *out, int max);

/* Web task: same path as the BOOT button. The link task answers it within one pass (5 ms): a
 * BOMB_CONFIG with the next puzzle pair, then CMD RING_NOW (APP_BOOT_RING_DELAY_S). */
void link_request_demo_ring(void);

#endif /* LINK_TASK_H */
