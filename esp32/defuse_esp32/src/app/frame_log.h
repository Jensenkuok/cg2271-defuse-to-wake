/*
 * Defuse to Wake - frame log of the MCX link (pure logic, host-tested: no Arduino headers, no heap).
 * The link task records the frames it writes and the frames it receives into a ring of the last
 * FRAME_LOG_LEN entries; the web page's Engineering tab shows them (rubric: comms protocol). This
 * file only holds the entry type, the names, the filter and the corrupt-frame helper; the lock and
 * the hooks live in link_task.cpp.
 *
 * What is logged: every frame except the periodic ones (HEARTBEAT, GAME_STATE, TELEMETRY: dozens a
 * second would push the interesting rows out of a 20-entry ring). Errors are never filtered:
 * a CRC error, a NACK, a retransmit and a dropped frame are always recorded.
 */
#ifndef FRAME_LOG_H
#define FRAME_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "fixed_ring.h"
#include "../shared/protocol.h"

#define FRAME_LOG_LEN       20

enum FrameDir { FLOG_TX = 0, FLOG_RX = 1 };

/* What happened to the frame. For ACK / NACK frames the entry's seq is the sequence number being
 * acknowledged (payload byte 0), not the ACK frame's own SEQ; a NACK for a CRC/length error has
 * PROTO_LINK_NACK_NO_SEQ (255) there. */
enum FrameOutcome {
    FLOG_OK = 0,        /* written / received normally */
    FLOG_ACK,           /* RX or TX ACK frame */
    FLOG_NACK,          /* RX or TX NACK frame */
    FLOG_RETX,          /* TX: the same frame written again (timeout or NACK) */
    FLOG_CRC_ERR,       /* RX: a frame that failed its CRC (type unknown) */
    FLOG_SEND_FAIL,     /* TX: dropped after the retries */
    FLOG_FAULT_CRC      /* TX: our own fault injection flipped the CRC byte */
};

struct FrameLogEntry {
    uint32_t ms;        /* millis() when it was recorded */
    uint8_t dir;        /* FrameDir */
    uint8_t type;       /* PROTO_MSG_*, 0 when unknown (CRC error) */
    uint8_t seq;        /* see above */
    uint8_t len;        /* payload bytes */
    uint8_t outcome;    /* FrameOutcome */
};

typedef FixedRing<FrameLogEntry, FRAME_LOG_LEN> FrameLogRing;

const char *frame_type_name(uint8_t type);       /* "BOMB_CONFIG", "?" for an unknown type */
const char *frame_dir_name(uint8_t dir);         /* "TX" / "RX" */
const char *frame_outcome_name(uint8_t outcome); /* "OK", "ACK", "NACK", "RETX", "CRC_ERR", "SEND_FAIL", "CRC CORRUPTED (fault)" */

/* Types that need an ACK (proto_link.h "Reliable types"). */
bool frame_type_is_reliable(uint8_t type);
/* HEARTBEAT, GAME_STATE, TELEMETRY. */
bool frame_type_is_periodic(uint8_t type);

FrameLogEntry frame_log_entry(uint32_t ms, uint8_t dir, uint8_t type, uint8_t seq, uint8_t len, uint8_t outcome);

/* The seq to record for a frame: payload[0] for a well-formed ACK / NACK, else the frame's own seq. */
uint8_t frame_log_seq(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len);

/* TX outcome: a corrupted frame is FLOG_FAULT_CRC, a retransmit FLOG_RETX, else by type (see
 * frame_rx_outcome: our own ACK / NACK frames are FLOG_ACK / FLOG_NACK, everything else FLOG_OK). */
uint8_t frame_tx_outcome(uint8_t type, bool retransmit, bool corrupted);
/* RX outcome by type: ACK -> FLOG_ACK, NACK -> FLOG_NACK, anything else FLOG_OK. */
uint8_t frame_rx_outcome(uint8_t type);

/* Push e unless it is a periodic frame with a plain OK outcome (and include_periodic is false).
 * Returns true if it was stored. */
bool frame_log_record(FrameLogRing *ring, bool include_periodic, const FrameLogEntry &e);

/* Fault injection: flip the last byte (the CRC) of a whole frame. No-op for len == 0. */
void frame_corrupt_crc(uint8_t *frame, size_t len);

#endif /* FRAME_LOG_H */
