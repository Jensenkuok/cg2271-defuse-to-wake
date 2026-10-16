/*
 * Defuse to Wake - reliable transport (ACK / retransmit / heartbeat) on top of
 * the frame codec in protocol.h. Canonical copy lives in shared/; it is
 * compiled unchanged as C99 on the MCXC444 and the ESP32.
 *
 * THREADING: a link is NOT internally thread-safe, by design. Exactly one RTOS
 * task owns a link and the caller serialises every call to proto_link_send,
 * proto_link_rx_byte and proto_link_poll (and reads of the stats). A UART RX
 * ISR must only push received bytes into a queue; the owning task pops them
 * and calls proto_link_rx_byte. Callbacks run inside those calls on the
 * owning task; they may call proto_link_send but not rx_byte or poll.
 *
 * Platform-agnostic: time is a uint32_t now_ms argument (all comparisons are
 * wraparound-safe), output bytes go through write_fn (one call per whole
 * frame), delivered frames and events go through callbacks. No dynamic
 * allocation, no globals, no OS calls.
 *
 * Reliable types (need ACK): TIME_SYNC, SET_ALARM, BOMB_CONFIG, TAMPER_EVENT,
 * CMD, RESULT. Unreliable: HEARTBEAT, GAME_STATE, TELEMETRY, ACK, NACK.
 * Stop-and-wait: one outstanding reliable frame, plus a FIFO of
 * PROTO_LINK_TX_DEPTH waiting ones. See docs/protocol.md ("Transport").
 */
#ifndef PROTO_LINK_H
#define PROTO_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_LINK_DEFAULT_TIMEOUT_MS   100u  /* retransmit timeout */
#define PROTO_LINK_DEFAULT_MAX_RETRIES  3u    /* retransmits before giving up */
#define PROTO_LINK_HEARTBEAT_MS         1000u
#define PROTO_LINK_DOWN_MS              3000u /* silence before the link is down */
#define PROTO_LINK_TX_DEPTH             4u    /* FIFO behind the outstanding frame */
#define PROTO_LINK_NACK_NO_SEQ          0xFFu /* NACK acked_seq for CRC/length errors */

typedef enum {
    PROTO_EVT_LINK_UP = 1,
    PROTO_EVT_LINK_DOWN,
    PROTO_EVT_SEND_FAILED  /* type and seq identify the dropped frame */
} proto_event_t;

typedef void (*proto_link_write_fn)(const uint8_t *buf, size_t len, void *ctx);
typedef void (*proto_link_deliver_fn)(const proto_frame_t *f, void *ctx);
/* type/seq are 0 for LINK_UP and LINK_DOWN. */
typedef void (*proto_link_event_fn)(proto_event_t evt, uint8_t type, uint8_t seq, void *ctx);

typedef struct {
    proto_link_write_fn write_fn;
    proto_link_deliver_fn deliver_fn;
    proto_link_event_fn event_fn;
    void *ctx;
    uint16_t timeout_ms;  /* 0 = PROTO_LINK_DEFAULT_TIMEOUT_MS */
    uint8_t max_retries;  /* 0 = PROTO_LINK_DEFAULT_MAX_RETRIES */
} proto_link_cfg_t;

typedef struct {
    uint32_t tx_frames;     /* new frames written (retransmits are separate) */
    uint32_t rx_frames;     /* valid frames received, duplicates included */
    uint32_t retransmits;
    uint32_t send_failures;
    uint32_t crc_errors;
    uint32_t len_errors;
    uint32_t dup_drops;     /* duplicate reliable frames not delivered */
    uint32_t nacks_sent;
} proto_link_stats_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} proto_link_slot_t;

/* Fields are private; only stats is meant to be read by the owner. */
typedef struct {
    proto_link_cfg_t cfg;
    proto_decoder_t decoder;
    uint8_t tx_seq;

    proto_link_slot_t fifo[PROTO_LINK_TX_DEPTH];
    uint8_t fifo_head;
    uint8_t fifo_count;

    bool out_active;
    uint8_t out_type;
    uint8_t out_seq;
    uint8_t out_retries;
    uint8_t out_frame[PROTO_MAX_FRAME];
    size_t out_frame_len;
    uint32_t out_last_tx_ms;

    bool have_rx_seq;
    uint8_t last_rx_seq;

    bool up;
    uint32_t last_rx_ms;
    uint32_t last_hb_ms;

    proto_link_stats_t stats;
} proto_link_t;

/* cfg may be NULL (all defaults). The link starts down. */
void proto_link_init(proto_link_t *l, const proto_link_cfg_t *cfg, uint32_t now_ms);

/*
 * Queue or send a message. Unreliable types are written immediately. Reliable
 * types are sent at once if nothing is outstanding, else queued (FIFO of 4,
 * i.e. up to 5 reliable frames in flight + waiting). Returns false on invalid
 * arguments, an unknown type, len > 32, or a full FIFO.
 */
bool proto_link_send(proto_link_t *l, uint8_t type, const uint8_t *payload, uint8_t len,
                     uint32_t now_ms);

/* Feed one received byte (called by the owning task, never from an ISR). */
void proto_link_rx_byte(proto_link_t *l, uint8_t byte, uint32_t now_ms);

/* Drive timers: retransmit timeout, 1 s heartbeat, link-down detection. */
void proto_link_poll(proto_link_t *l, uint32_t now_ms);

bool proto_link_is_up(const proto_link_t *l);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_LINK_H */
