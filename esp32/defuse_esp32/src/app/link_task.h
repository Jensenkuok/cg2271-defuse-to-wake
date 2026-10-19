/*
 * Defuse to Wake - link task (highest priority). The ONLY owner of the proto_link_t,
 * over Serial1. Every other task sends through the TX queue (link_send); received
 * frames are delivered into the shared app state.
 * Also polls the BOOT button (GPIO0) every 50 ms as a demo trigger: CMD RING_NOW.
 */
#ifndef LINK_TASK_H
#define LINK_TASK_H

#include <Arduino.h>
#include <stdbool.h>
#include <stdint.h>

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

#endif /* LINK_TASK_H */
