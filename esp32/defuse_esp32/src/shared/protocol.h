/*
 * Defuse to Wake - inter-board UART protocol (MCXC444 <-> ESP32).
 *
 * Canonical copy lives in shared/. It is compiled unchanged as C99 on the
 * MCXC444 and included from C++ on the ESP32 (tools/sync_shared.ps1 copies it
 * into the sketch folder). No dynamic allocation, no globals.
 *
 * Link: UART 115200 baud, 8 data bits, no parity, 1 stop bit (8N1).
 * This module only builds/parses frames; it contains no UART code.
 *
 * Frame: [SOF 0xAA][TYPE][SEQ][LEN 0..32][PAYLOAD x LEN][CRC8]
 * CRC-8: poly 0x07, init 0x00, no reflection, no final XOR, over
 * TYPE, SEQ, LEN and PAYLOAD. proto_crc8("123456789") == 0xF4.
 * All multi-byte payload fields are little-endian.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SOF           0xAAu
#define PROTO_MAX_PAYLOAD   32u
#define PROTO_HEADER_SIZE   4u  /* SOF, TYPE, SEQ, LEN */
#define PROTO_MAX_FRAME     (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + 1u) /* + CRC = 37 */
#define PROTO_CRC8_POLY     0x07u

/* ---- Message types ---- */
typedef enum {
    PROTO_MSG_HEARTBEAT    = 0x01,
    PROTO_MSG_ACK          = 0x02,
    PROTO_MSG_NACK         = 0x03,
    /* ESP32 -> MCX */
    PROTO_MSG_TIME_SYNC    = 0x10,
    PROTO_MSG_SET_ALARM    = 0x11,
    PROTO_MSG_BOMB_CONFIG  = 0x12,
    PROTO_MSG_TAMPER_EVENT = 0x13,
    PROTO_MSG_CMD          = 0x14,
    /* MCX -> ESP32 */
    PROTO_MSG_GAME_STATE   = 0x20,
    PROTO_MSG_RESULT       = 0x21,
    PROTO_MSG_TELEMETRY    = 0x22
} proto_msg_type_t;

/* ---- Field value enums ---- */
enum { PROTO_NACK_CRC = 1, PROTO_NACK_LEN = 2, PROTO_NACK_UNKNOWN_TYPE = 3 };
enum { PROTO_CMD_RING_NOW = 1, PROTO_CMD_SNOOZE = 2, PROTO_CMD_CANCEL = 3 };
enum { PROTO_MODULE_DIAL = 1, PROTO_MODULE_WIRES = 2, PROTO_MODULE_TRIPWIRE = 3 };
enum {
    PROTO_STATE_IDLE = 0, PROTO_STATE_ARMED = 1, PROTO_STATE_BOMB_ACTIVE = 2,
    PROTO_STATE_DEFUSED = 3, PROTO_STATE_EXPLODED = 4
};
enum { PROTO_OUTCOME_DEFUSED = 1, PROTO_OUTCOME_EXPLODED = 2 };

#define PROTO_MAX_MODULES 4u
#define PROTO_DIAL_DIGITS 4u

/* ---- Packed payload sizes (bytes) ---- */
#define PROTO_SIZE_HEARTBEAT    4u
#define PROTO_SIZE_ACK          1u
#define PROTO_SIZE_NACK         2u
#define PROTO_SIZE_TIME_SYNC    4u
#define PROTO_SIZE_SET_ALARM    4u
#define PROTO_SIZE_CMD          3u
#define PROTO_SIZE_BOMB_CONFIG  16u
#define PROTO_SIZE_TAMPER_EVENT 2u
#define PROTO_SIZE_GAME_STATE   9u
#define PROTO_SIZE_RESULT       5u
#define PROTO_SIZE_TELEMETRY    8u

/* ---- Frame codec ---- */
uint8_t proto_crc8(const uint8_t *data, size_t len);

/* Returns bytes written (len + 5), or 0 on bad args, len > 32, or out too small. */
size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len,
                    uint8_t *out, size_t out_size);

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} proto_frame_t;

typedef enum {
    PROTO_NONE = 0,     /* byte consumed, no frame yet */
    PROTO_FRAME_OK,     /* *out holds a complete, CRC-valid frame */
    PROTO_ERR_CRC,
    PROTO_ERR_LEN
} proto_result_t;

typedef enum {
    PROTO_ST_WAIT_SOF = 0,
    PROTO_ST_TYPE,
    PROTO_ST_SEQ,
    PROTO_ST_LEN,
    PROTO_ST_PAYLOAD,
    PROTO_ST_CRC
} proto_state_t;

typedef struct {
    proto_state_t state;
    uint8_t buf[PROTO_MAX_FRAME]; /* every byte received since the current SOF */
    size_t count;                 /* valid bytes in buf */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t len_errors;
} proto_decoder_t;

void proto_decoder_init(proto_decoder_t *d);

/*
 * Feed one received byte. On an error the decoder resyncs: bytes already
 * buffered after the failed SOF are replayed from the next 0xAA, so a good
 * frame hidden inside a bad one is still found. If the replay completes a
 * frame, PROTO_FRAME_OK is returned (the error is still counted).
 */
proto_result_t proto_decoder_feed(proto_decoder_t *d, uint8_t byte, proto_frame_t *out);

/* ---- Payload structs ---- */
typedef struct { uint32_t uptime_s; } proto_heartbeat_t;
typedef struct { uint8_t acked_seq; } proto_ack_t;
typedef struct { uint8_t acked_seq; uint8_t reason; } proto_nack_t;
typedef struct { uint32_t unix_time; } proto_time_sync_t;
typedef struct { uint32_t alarm_unix; } proto_set_alarm_t;
typedef struct { uint8_t cmd; uint16_t arg; } proto_cmd_t;
typedef struct {
    uint8_t module_count;                    /* 1..4 */
    uint8_t module_order[PROTO_MAX_MODULES]; /* PROTO_MODULE_* */
    uint16_t time_limit_s;
    uint8_t dial_code[PROTO_DIAL_DIGITS];    /* digits 0-9 */
    uint8_t wire_count;
    uint8_t correct_wire;
    uint8_t trip_min_cm;
    uint8_t trip_max_cm;
    uint8_t max_strikes;
} proto_bomb_config_t;
typedef struct { uint16_t magnitude_mg; } proto_tamper_event_t;
typedef struct {
    uint8_t state;        /* PROTO_STATE_* */
    uint8_t module_idx;
    uint8_t module_type;  /* PROTO_MODULE_* */
    uint16_t time_left_s;
    uint8_t strikes;
    uint8_t progress_pct;
    int16_t module_value;
} proto_game_state_t;
typedef struct {
    uint8_t outcome;      /* PROTO_OUTCOME_* */
    uint16_t duration_s;
    uint8_t strikes;
    uint8_t snoozes;
} proto_result_msg_t;
typedef struct {
    uint16_t joy_x;
    uint16_t joy_y;
    uint16_t distance_cm;
    int16_t encoder_count;
} proto_telemetry_t;

/*
 * proto_pack_*: returns bytes written (PROTO_SIZE_*) or 0 if out_size is too small.
 * proto_unpack_*: returns false when len != PROTO_SIZE_* (or a pointer is NULL).
 */
size_t proto_pack_heartbeat(const proto_heartbeat_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_heartbeat(const uint8_t *p, size_t len, proto_heartbeat_t *s);
size_t proto_pack_ack(const proto_ack_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_ack(const uint8_t *p, size_t len, proto_ack_t *s);
size_t proto_pack_nack(const proto_nack_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_nack(const uint8_t *p, size_t len, proto_nack_t *s);
size_t proto_pack_time_sync(const proto_time_sync_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_time_sync(const uint8_t *p, size_t len, proto_time_sync_t *s);
size_t proto_pack_set_alarm(const proto_set_alarm_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_set_alarm(const uint8_t *p, size_t len, proto_set_alarm_t *s);
size_t proto_pack_cmd(const proto_cmd_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_cmd(const uint8_t *p, size_t len, proto_cmd_t *s);
size_t proto_pack_bomb_config(const proto_bomb_config_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_bomb_config(const uint8_t *p, size_t len, proto_bomb_config_t *s);
size_t proto_pack_tamper_event(const proto_tamper_event_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_tamper_event(const uint8_t *p, size_t len, proto_tamper_event_t *s);
size_t proto_pack_game_state(const proto_game_state_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_game_state(const uint8_t *p, size_t len, proto_game_state_t *s);
size_t proto_pack_result(const proto_result_msg_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_result(const uint8_t *p, size_t len, proto_result_msg_t *s);
size_t proto_pack_telemetry(const proto_telemetry_t *s, uint8_t *out, size_t out_size);
bool proto_unpack_telemetry(const uint8_t *p, size_t len, proto_telemetry_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
