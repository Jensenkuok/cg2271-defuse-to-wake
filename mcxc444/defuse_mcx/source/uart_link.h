/*
 * Defuse to Wake - inter-board link on UART2 (PTE22/PTE23, 115200 8N1).
 *
 * One comm task owns the proto_link_t (shared/proto_link.h is not thread-safe
 * by design). Other tasks never touch it: they call uart_link_send(), which
 * only posts a request to a queue that the comm task drains.
 *
 * Data path:
 *   RX: UART2 ISR -> RX byte queue -> comm task -> proto_link_rx_byte
 *   TX: comm task (proto_link write callback) -> TX ring -> UART2 ISR (TIE)
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "proto_link.h"

/* Highest message type value + 1, for the per-type receive counters. */
#define UART_LINK_TYPE_SLOTS (PROTO_MSG_TELEMETRY + 1)

typedef struct {
    proto_link_stats_t link;                    /* copied from the proto_link_t */
    uint32_t rx_by_type[UART_LINK_TYPE_SLOTS];  /* delivered frames per type */
    uint32_t rx_other_type;                     /* delivered type >= TYPE_SLOTS */
    uint32_t rx_queue_overflows;                /* ISR could not queue a byte */
    uint32_t uart_errors;                       /* overrun / noise / framing / parity */
    uint32_t tx_ring_drops;                     /* frames dropped: TX ring full */
    uint32_t tx_req_drops;                      /* uart_link_send: request queue full */
    uint32_t rx_bad_payload;                    /* delivered frame whose payload failed to unpack */
    uint32_t rx_ignored;                        /* delivered frame of a type the MCX does not act on */
    uint32_t ctrl_drops;                        /* control queue full: TIME_SYNC/SET_ALARM/CONFIG lost */
    uint32_t game_state_sent;                   /* GAME_STATE frames handed to the link */
    uint32_t telemetry_sent;                    /* TELEMETRY frames handed to the link */
} uart_link_stats_t;

/* Called from the comm task. Keep them short; they must not block. */
typedef struct {
    void (*on_link_change)(bool up);
    void (*on_frame)(const proto_frame_t *f);
} uart_link_hooks_t;

/* UART2 hardware, queues and link state. Call once before the scheduler starts. */
bool uart_link_init(const uart_link_hooks_t *hooks);

/* Create the comm task. */
bool uart_link_start(void);

/* Any task: ask the comm task to send a message (copied). False if the queue is full. */
bool uart_link_send(uint8_t type, const uint8_t *payload, uint8_t len);

/* Any task: consistent snapshot of the counters. */
void uart_link_get_stats(uart_link_stats_t *out);

/* For stack high-water-mark reporting. */
TaskHandle_t uart_link_task_handle(void);

#endif /* UART_LINK_H */
