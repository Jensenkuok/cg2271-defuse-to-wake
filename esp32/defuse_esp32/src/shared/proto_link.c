/* Defuse to Wake - reliable transport layer. See proto_link.h. */
#include "proto_link.h"

#include <string.h>

/* Wraparound-safe "a is at or after b". */
static bool time_reached(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

static bool is_reliable_type(uint8_t type)
{
    switch (type) {
    case PROTO_MSG_TIME_SYNC:
    case PROTO_MSG_SET_ALARM:
    case PROTO_MSG_BOMB_CONFIG:
    case PROTO_MSG_TAMPER_EVENT:
    case PROTO_MSG_CMD:
    case PROTO_MSG_RESULT:
        return true;
    default:
        return false;
    }
}

static bool is_unreliable_type(uint8_t type)
{
    switch (type) {
    case PROTO_MSG_HEARTBEAT:
    case PROTO_MSG_ACK:
    case PROTO_MSG_NACK:
    case PROTO_MSG_GAME_STATE:
    case PROTO_MSG_TELEMETRY:
        return true;
    default:
        return false;
    }
}

static void write_raw(proto_link_t *l, const uint8_t *buf, size_t len)
{
    if (l->cfg.write_fn != NULL) {
        l->cfg.write_fn(buf, len, l->cfg.ctx);
    }
}

static void emit_event(proto_link_t *l, proto_event_t evt, uint8_t type, uint8_t seq)
{
    if (l->cfg.event_fn != NULL) {
        l->cfg.event_fn(evt, type, seq, l->cfg.ctx);
    }
}

/* Encode a NEW frame (takes the next SEQ) into buf and write it. Returns the
 * frame length, or 0 if encoding failed (nothing is sent, SEQ unchanged). */
static size_t emit_new(proto_link_t *l, uint8_t type, const uint8_t *payload, uint8_t len,
                       uint8_t *buf, size_t buf_size, uint8_t *seq_used)
{
    size_t n = proto_encode(type, l->tx_seq, payload, len, buf, buf_size);

    if (n == 0u) {
        return 0u;
    }
    *seq_used = l->tx_seq;
    l->tx_seq = (uint8_t)(l->tx_seq + 1u);
    l->stats.tx_frames++;
    write_raw(l, buf, n);
    return n;
}

static void send_unreliable(proto_link_t *l, uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint8_t seq;

    (void)emit_new(l, type, payload, len, buf, sizeof(buf), &seq);
}

static void send_ack(proto_link_t *l, uint8_t acked_seq)
{
    proto_ack_t a;
    uint8_t pl[PROTO_SIZE_ACK];

    a.acked_seq = acked_seq;
    if (proto_pack_ack(&a, pl, sizeof(pl)) != 0u) {
        send_unreliable(l, PROTO_MSG_ACK, pl, PROTO_SIZE_ACK);
    }
}

static void send_nack(proto_link_t *l, uint8_t acked_seq, uint8_t reason)
{
    proto_nack_t n;
    uint8_t pl[PROTO_SIZE_NACK];

    n.acked_seq = acked_seq;
    n.reason = reason;
    if (proto_pack_nack(&n, pl, sizeof(pl)) != 0u) {
        l->stats.nacks_sent++;
        send_unreliable(l, PROTO_MSG_NACK, pl, PROTO_SIZE_NACK);
    }
}

/* Promote the head of the FIFO to the outstanding frame (first transmission). */
static void start_next(proto_link_t *l, uint32_t now_ms)
{
    const proto_link_slot_t *slot;
    size_t n;

    if (l->out_active || l->fifo_count == 0u) {
        return;
    }
    slot = &l->fifo[l->fifo_head];
    n = emit_new(l, slot->type, slot->payload, slot->len,
                 l->out_frame, sizeof(l->out_frame), &l->out_seq);
    l->out_type = slot->type;
    l->fifo_head = (uint8_t)((l->fifo_head + 1u) % PROTO_LINK_TX_DEPTH);
    l->fifo_count--;
    if (n == 0u) {
        /* Cannot happen: send() validated the slot. Skip it rather than stall. */
        start_next(l, now_ms);
        return;
    }
    l->out_frame_len = n;
    l->out_retries = 0u;
    l->out_last_tx_ms = now_ms;
    l->out_active = true;
}

/* Retransmit the outstanding frame, or drop it once retries are exhausted. */
static void retry_or_fail(proto_link_t *l, uint32_t now_ms)
{
    uint8_t type;
    uint8_t seq;

    if (l->out_retries < l->cfg.max_retries) {
        l->out_retries++;
        l->stats.retransmits++;
        l->out_last_tx_ms = now_ms;
        write_raw(l, l->out_frame, l->out_frame_len);
        return;
    }
    type = l->out_type;
    seq = l->out_seq;
    l->out_active = false;
    l->stats.send_failures++;
    emit_event(l, PROTO_EVT_SEND_FAILED, type, seq);
    start_next(l, now_ms);
}

void proto_link_init(proto_link_t *l, const proto_link_cfg_t *cfg, uint32_t now_ms)
{
    if (l == NULL) {
        return;
    }
    memset(l, 0, sizeof(*l));
    if (cfg != NULL) {
        l->cfg = *cfg;
    }
    if (l->cfg.timeout_ms == 0u) {
        l->cfg.timeout_ms = (uint16_t)PROTO_LINK_DEFAULT_TIMEOUT_MS;
    }
    if (l->cfg.max_retries == 0u) {
        l->cfg.max_retries = (uint8_t)PROTO_LINK_DEFAULT_MAX_RETRIES;
    }
    proto_decoder_init(&l->decoder);
    l->last_rx_ms = now_ms;
    l->last_hb_ms = now_ms;
}

bool proto_link_send(proto_link_t *l, uint8_t type, const uint8_t *payload, uint8_t len,
                     uint32_t now_ms)
{
    proto_link_slot_t *slot;
    uint8_t tail;

    if (l == NULL || len > PROTO_MAX_PAYLOAD || (len > 0u && payload == NULL)) {
        return false;
    }
    if (is_unreliable_type(type)) {
        send_unreliable(l, type, payload, len);
        return true;
    }
    if (!is_reliable_type(type) || l->fifo_count >= PROTO_LINK_TX_DEPTH) {
        return false;
    }
    /* Invariant: a non-empty FIFO implies a frame is outstanding, so when idle
     * the FIFO is empty and this frame is promoted (sent) right away. */
    tail = (uint8_t)((l->fifo_head + l->fifo_count) % PROTO_LINK_TX_DEPTH);
    slot = &l->fifo[tail];
    slot->type = type;
    slot->len = len;
    if (len > 0u) {
        memcpy(slot->payload, payload, len);
    }
    l->fifo_count++;
    start_next(l, now_ms);
    return true;
}

static void handle_ack(proto_link_t *l, const proto_frame_t *f, uint32_t now_ms)
{
    proto_ack_t a;

    if (!proto_unpack_ack(f->payload, f->len, &a)) {
        return;
    }
    if (l->out_active && a.acked_seq == l->out_seq) {
        l->out_active = false;
        start_next(l, now_ms);
    }
}

static void handle_frame(proto_link_t *l, const proto_frame_t *f, uint32_t now_ms)
{
    l->stats.rx_frames++;
    l->last_rx_ms = now_ms;
    if (!l->up) {
        l->up = true;
        emit_event(l, PROTO_EVT_LINK_UP, 0u, 0u);
    }

    switch (f->type) {
    case PROTO_MSG_ACK:
        handle_ack(l, f, now_ms);
        break;
    case PROTO_MSG_NACK:
        if (l->out_active) {
            retry_or_fail(l, now_ms);
        }
        break;
    case PROTO_MSG_HEARTBEAT:
    case PROTO_MSG_GAME_STATE:
    case PROTO_MSG_TELEMETRY:
        if (l->cfg.deliver_fn != NULL) {
            l->cfg.deliver_fn(f, l->cfg.ctx);
        }
        break;
    case PROTO_MSG_TIME_SYNC:
    case PROTO_MSG_SET_ALARM:
    case PROTO_MSG_BOMB_CONFIG:
    case PROTO_MSG_TAMPER_EVENT:
    case PROTO_MSG_CMD:
    case PROTO_MSG_RESULT:
        send_ack(l, f->seq);
        if (l->have_rx_seq && l->last_rx_seq == f->seq) {
            l->stats.dup_drops++;
        } else {
            l->have_rx_seq = true;
            l->last_rx_seq = f->seq;
            if (l->cfg.deliver_fn != NULL) {
                l->cfg.deliver_fn(f, l->cfg.ctx);
            }
        }
        break;
    default:
        send_nack(l, f->seq, PROTO_NACK_UNKNOWN_TYPE);
        break;
    }
}

void proto_link_rx_byte(proto_link_t *l, uint8_t byte, uint32_t now_ms)
{
    proto_frame_t f;
    proto_result_t res;
    uint32_t crc_before;
    uint32_t len_before;

    if (l == NULL) {
        return;
    }
    crc_before = l->decoder.crc_errors;
    len_before = l->decoder.len_errors;
    res = proto_decoder_feed(&l->decoder, byte, &f);

    /* The decoder may report FRAME_OK from a resync replay although it also hit
     * an error, so use its counters to see every error, not just the return. */
    if (l->decoder.crc_errors != crc_before) {
        l->stats.crc_errors += l->decoder.crc_errors - crc_before;
        send_nack(l, PROTO_LINK_NACK_NO_SEQ, PROTO_NACK_CRC);
    }
    if (l->decoder.len_errors != len_before) {
        l->stats.len_errors += l->decoder.len_errors - len_before;
        send_nack(l, PROTO_LINK_NACK_NO_SEQ, PROTO_NACK_LEN);
    }
    if (res == PROTO_FRAME_OK) {
        handle_frame(l, &f, now_ms);
    }
}

void proto_link_poll(proto_link_t *l, uint32_t now_ms)
{
    proto_heartbeat_t hb;
    uint8_t pl[PROTO_SIZE_HEARTBEAT];

    if (l == NULL) {
        return;
    }
    if (l->out_active && time_reached(now_ms, l->out_last_tx_ms + l->cfg.timeout_ms)) {
        retry_or_fail(l, now_ms);
    }
    if (l->up && time_reached(now_ms, l->last_rx_ms + PROTO_LINK_DOWN_MS)) {
        l->up = false;
        emit_event(l, PROTO_EVT_LINK_DOWN, 0u, 0u);
    }
    if (time_reached(now_ms, l->last_hb_ms + PROTO_LINK_HEARTBEAT_MS)) {
        l->last_hb_ms += PROTO_LINK_HEARTBEAT_MS;
        if (time_reached(now_ms, l->last_hb_ms + PROTO_LINK_HEARTBEAT_MS)) {
            l->last_hb_ms = now_ms; /* polled very late: do not burst */
        }
        hb.uptime_s = now_ms / 1000u;
        if (proto_pack_heartbeat(&hb, pl, sizeof(pl)) != 0u) {
            send_unreliable(l, PROTO_MSG_HEARTBEAT, pl, PROTO_SIZE_HEARTBEAT);
        }
    }
}

bool proto_link_is_up(const proto_link_t *l)
{
    return l != NULL && l->up;
}
