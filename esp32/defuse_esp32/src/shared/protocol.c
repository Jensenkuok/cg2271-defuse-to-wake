/* Defuse to Wake - inter-board protocol codec. See protocol.h. */
#include "protocol.h"

#include <string.h>

/* ---- little-endian helpers ---- */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- CRC-8 (poly 0x07, init 0, no reflection, no xorout) ---- */
uint8_t proto_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;
    size_t i;
    int bit;

    if (data == NULL) {
        return 0u;
    }
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ PROTO_CRC8_POLY) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len,
                    uint8_t *out, size_t out_size)
{
    size_t total = (size_t)len + PROTO_HEADER_SIZE + 1u;

    if (out == NULL || len > PROTO_MAX_PAYLOAD || (len > 0u && payload == NULL) || out_size < total) {
        return 0u;
    }
    out[0] = PROTO_SOF;
    out[1] = type;
    out[2] = seq;
    out[3] = len;
    if (len > 0u) {
        memcpy(&out[PROTO_HEADER_SIZE], payload, len);
    }
    out[total - 1u] = proto_crc8(&out[1], (size_t)len + 3u);
    return total;
}

/* ---- Streaming decoder ---- */
void proto_decoder_init(proto_decoder_t *d)
{
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof(*d));
    d->state = PROTO_ST_WAIT_SOF;
}

/* Advance the state machine by one byte; no resync logic here. */
static proto_result_t decoder_step(proto_decoder_t *d, uint8_t byte, proto_frame_t *out)
{
    if (d->state == PROTO_ST_WAIT_SOF) {
        if (byte == PROTO_SOF) {
            d->buf[0] = byte;
            d->count = 1u;
            d->state = PROTO_ST_TYPE;
        }
        return PROTO_NONE;
    }

    d->buf[d->count++] = byte;

    switch (d->state) {
    case PROTO_ST_TYPE:
        d->state = PROTO_ST_SEQ;
        return PROTO_NONE;
    case PROTO_ST_SEQ:
        d->state = PROTO_ST_LEN;
        return PROTO_NONE;
    case PROTO_ST_LEN:
        if (byte > PROTO_MAX_PAYLOAD) {
            d->len_errors++;
            return PROTO_ERR_LEN;
        }
        d->state = (byte == 0u) ? PROTO_ST_CRC : PROTO_ST_PAYLOAD;
        return PROTO_NONE;
    case PROTO_ST_PAYLOAD:
        if (d->count == (size_t)PROTO_HEADER_SIZE + d->buf[3]) {
            d->state = PROTO_ST_CRC;
        }
        return PROTO_NONE;
    case PROTO_ST_CRC:
        if (proto_crc8(&d->buf[1], d->count - 2u) != byte) {
            d->crc_errors++;
            return PROTO_ERR_CRC;
        }
        out->type = d->buf[1];
        out->seq = d->buf[2];
        out->len = d->buf[3];
        memcpy(out->payload, &d->buf[PROTO_HEADER_SIZE], d->buf[3]);
        d->frames_ok++;
        d->state = PROTO_ST_WAIT_SOF;
        d->count = 0u;
        return PROTO_FRAME_OK;
    default:
        d->state = PROTO_ST_WAIT_SOF;
        d->count = 0u;
        return PROTO_NONE;
    }
}

proto_result_t proto_decoder_feed(proto_decoder_t *d, uint8_t byte, proto_frame_t *out)
{
    proto_result_t res;
    proto_result_t first_error;
    uint8_t replay[PROTO_MAX_FRAME];
    size_t n;
    size_t start;
    size_t i;

    if (d == NULL || out == NULL) {
        return PROTO_NONE;
    }
    res = decoder_step(d, byte, out);
    if (res != PROTO_ERR_CRC && res != PROTO_ERR_LEN) {
        return res;
    }

    /* Error: drop the failed SOF and replay the rest from the next 0xAA.
     * Each pass strictly shortens the buffered data, so this terminates. */
    first_error = res;
    for (;;) {
        n = d->count;
        memcpy(replay, d->buf, n);
        d->state = PROTO_ST_WAIT_SOF;
        d->count = 0u;

        for (start = 1u; start < n && replay[start] != PROTO_SOF; start++) {
        }
        if (start >= n) {
            return first_error;
        }
        res = PROTO_NONE;
        for (i = start; i < n; i++) {
            res = decoder_step(d, replay[i], out);
            if (res == PROTO_FRAME_OK) {
                return PROTO_FRAME_OK;
            }
            if (res == PROTO_ERR_CRC || res == PROTO_ERR_LEN) {
                break; /* nested error: restart the scan on the new buffer */
            }
        }
        if (res != PROTO_ERR_CRC && res != PROTO_ERR_LEN) {
            return first_error;
        }
    }
}

/* ---- Payload pack / unpack ---- */
#define CHECK_PACK(size) \
    do { if (s == NULL || out == NULL || out_size < (size)) { return 0u; } } while (0)
#define CHECK_UNPACK(size) \
    do { if (s == NULL || p == NULL || len != (size)) { return false; } } while (0)

size_t proto_pack_heartbeat(const proto_heartbeat_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_HEARTBEAT);
    put_u32(out, s->uptime_s);
    return PROTO_SIZE_HEARTBEAT;
}

bool proto_unpack_heartbeat(const uint8_t *p, size_t len, proto_heartbeat_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_HEARTBEAT);
    s->uptime_s = get_u32(p);
    return true;
}

size_t proto_pack_ack(const proto_ack_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_ACK);
    out[0] = s->acked_seq;
    return PROTO_SIZE_ACK;
}

bool proto_unpack_ack(const uint8_t *p, size_t len, proto_ack_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_ACK);
    s->acked_seq = p[0];
    return true;
}

size_t proto_pack_nack(const proto_nack_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_NACK);
    out[0] = s->acked_seq;
    out[1] = s->reason;
    return PROTO_SIZE_NACK;
}

bool proto_unpack_nack(const uint8_t *p, size_t len, proto_nack_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_NACK);
    s->acked_seq = p[0];
    s->reason = p[1];
    return true;
}

size_t proto_pack_time_sync(const proto_time_sync_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_TIME_SYNC);
    put_u32(out, s->unix_time);
    return PROTO_SIZE_TIME_SYNC;
}

bool proto_unpack_time_sync(const uint8_t *p, size_t len, proto_time_sync_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_TIME_SYNC);
    s->unix_time = get_u32(p);
    return true;
}

size_t proto_pack_set_alarm(const proto_set_alarm_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_SET_ALARM);
    put_u32(out, s->alarm_unix);
    return PROTO_SIZE_SET_ALARM;
}

bool proto_unpack_set_alarm(const uint8_t *p, size_t len, proto_set_alarm_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_SET_ALARM);
    s->alarm_unix = get_u32(p);
    return true;
}

size_t proto_pack_cmd(const proto_cmd_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_CMD);
    out[0] = s->cmd;
    put_u16(&out[1], s->arg);
    return PROTO_SIZE_CMD;
}

bool proto_unpack_cmd(const uint8_t *p, size_t len, proto_cmd_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_CMD);
    s->cmd = p[0];
    s->arg = get_u16(&p[1]);
    return true;
}

size_t proto_pack_bomb_config(const proto_bomb_config_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_BOMB_CONFIG);
    out[0] = s->module_count;
    memcpy(&out[1], s->module_order, PROTO_MAX_MODULES);
    put_u16(&out[5], s->time_limit_s);
    memcpy(&out[7], s->dial_code, PROTO_DIAL_DIGITS);
    out[11] = s->wire_count;
    out[12] = s->correct_wire;
    out[13] = s->trip_min_cm;
    out[14] = s->trip_max_cm;
    out[15] = s->max_strikes;
    return PROTO_SIZE_BOMB_CONFIG;
}

bool proto_unpack_bomb_config(const uint8_t *p, size_t len, proto_bomb_config_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_BOMB_CONFIG);
    s->module_count = p[0];
    memcpy(s->module_order, &p[1], PROTO_MAX_MODULES);
    s->time_limit_s = get_u16(&p[5]);
    memcpy(s->dial_code, &p[7], PROTO_DIAL_DIGITS);
    s->wire_count = p[11];
    s->correct_wire = p[12];
    s->trip_min_cm = p[13];
    s->trip_max_cm = p[14];
    s->max_strikes = p[15];
    return true;
}

size_t proto_pack_tamper_event(const proto_tamper_event_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_TAMPER_EVENT);
    put_u16(out, s->magnitude_mg);
    return PROTO_SIZE_TAMPER_EVENT;
}

bool proto_unpack_tamper_event(const uint8_t *p, size_t len, proto_tamper_event_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_TAMPER_EVENT);
    s->magnitude_mg = get_u16(p);
    return true;
}

size_t proto_pack_game_state(const proto_game_state_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_GAME_STATE);
    out[0] = s->state;
    out[1] = s->module_idx;
    out[2] = s->module_type;
    put_u16(&out[3], s->time_left_s);
    out[5] = s->strikes;
    out[6] = s->progress_pct;
    put_u16(&out[7], (uint16_t)s->module_value);
    return PROTO_SIZE_GAME_STATE;
}

bool proto_unpack_game_state(const uint8_t *p, size_t len, proto_game_state_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_GAME_STATE);
    s->state = p[0];
    s->module_idx = p[1];
    s->module_type = p[2];
    s->time_left_s = get_u16(&p[3]);
    s->strikes = p[5];
    s->progress_pct = p[6];
    s->module_value = (int16_t)get_u16(&p[7]);
    return true;
}

size_t proto_pack_result(const proto_result_msg_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_RESULT);
    out[0] = s->outcome;
    put_u16(&out[1], s->duration_s);
    out[3] = s->strikes;
    out[4] = s->snoozes;
    return PROTO_SIZE_RESULT;
}

bool proto_unpack_result(const uint8_t *p, size_t len, proto_result_msg_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_RESULT);
    s->outcome = p[0];
    s->duration_s = get_u16(&p[1]);
    s->strikes = p[3];
    s->snoozes = p[4];
    return true;
}

size_t proto_pack_telemetry(const proto_telemetry_t *s, uint8_t *out, size_t out_size)
{
    CHECK_PACK(PROTO_SIZE_TELEMETRY);
    put_u16(&out[0], s->joy_x);
    put_u16(&out[2], s->joy_y);
    put_u16(&out[4], s->distance_cm);
    put_u16(&out[6], (uint16_t)s->encoder_count);
    return PROTO_SIZE_TELEMETRY;
}

bool proto_unpack_telemetry(const uint8_t *p, size_t len, proto_telemetry_t *s)
{
    CHECK_UNPACK(PROTO_SIZE_TELEMETRY);
    s->joy_x = get_u16(&p[0]);
    s->joy_y = get_u16(&p[2]);
    s->distance_cm = get_u16(&p[4]);
    s->encoder_count = (int16_t)get_u16(&p[6]);
    return true;
}
