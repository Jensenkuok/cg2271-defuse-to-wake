/* Host-side tests for shared/protocol.{h,c}. Plain asserts, no framework. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

/* Feed a buffer; return the result of the last byte that was not PROTO_NONE
 * (frames are copied to *out). Counts of each outcome go to cnt[]. */
static void feed_all(proto_decoder_t *d, const uint8_t *buf, size_t n,
                     proto_frame_t *frames, size_t *n_frames, int *crc_seen, int *len_seen)
{
    size_t i;
    proto_frame_t f;
    for (i = 0; i < n; i++) {
        switch (proto_decoder_feed(d, buf[i], &f)) {
        case PROTO_FRAME_OK:
            if (frames != NULL) {
                frames[*n_frames] = f;
            }
            (*n_frames)++;
            break;
        case PROTO_ERR_CRC:
            if (crc_seen != NULL) { (*crc_seen)++; }
            break;
        case PROTO_ERR_LEN:
            if (len_seen != NULL) { (*len_seen)++; }
            break;
        default:
            break;
        }
    }
}

static void test_crc_check_value(void)
{
    CHECK(proto_crc8((const uint8_t *)"123456789", 9) == 0xF4);
    CHECK(proto_crc8(NULL, 0) == 0x00);
}

static void test_encode_basic_and_limits(void)
{
    uint8_t out[PROTO_MAX_FRAME];
    uint8_t pl[40];
    size_t n;

    memset(pl, 0x11, sizeof(pl));
    n = proto_encode(0x01, 7, pl, 4, out, sizeof(out));
    CHECK(n == 9);
    CHECK(out[0] == 0xAA && out[1] == 0x01 && out[2] == 7 && out[3] == 4);
    CHECK(out[8] == proto_crc8(&out[1], 7));

    CHECK(proto_encode(0x01, 0, NULL, 0, out, sizeof(out)) == 5);
    CHECK(proto_encode(0x01, 0, pl, 32, out, sizeof(out)) == PROTO_MAX_FRAME);
    CHECK(proto_encode(0x01, 0, pl, 33, out, sizeof(out)) == 0);   /* len > 32 */
    CHECK(proto_encode(0x01, 0, NULL, 3, out, sizeof(out)) == 0);  /* null payload */
    CHECK(proto_encode(0x01, 0, pl, 4, NULL, 16) == 0);            /* null out */
    CHECK(proto_encode(0x01, 0, pl, 4, out, 8) == 0);              /* out too small */
    CHECK(proto_encode(0x01, 0, pl, 4, out, 9) == 9);              /* exact fit */
}

/* Encode a payload, decode it, and return the decoded frame. */
static int roundtrip(uint8_t type, uint8_t seq, const uint8_t *pl, uint8_t len, proto_frame_t *got)
{
    uint8_t wire[PROTO_MAX_FRAME];
    proto_decoder_t d;
    size_t n = proto_encode(type, seq, pl, len, wire, sizeof(wire));
    size_t frames = 0;

    proto_decoder_init(&d);
    if (n == 0) { return 0; }
    feed_all(&d, wire, n, got, &frames, NULL, NULL);
    return frames == 1 && got->type == type && got->seq == seq && got->len == len
           && memcmp(got->payload, pl, len) == 0 && d.frames_ok == 1;
}

static void test_message_roundtrips(void)
{
    uint8_t pl[PROTO_MAX_PAYLOAD];
    proto_frame_t f;

    {
        proto_heartbeat_t s = { 0x01020304u }, r;
        CHECK(proto_pack_heartbeat(&s, pl, sizeof(pl)) == PROTO_SIZE_HEARTBEAT);
        CHECK(pl[0] == 0x04 && pl[3] == 0x01); /* little-endian */
        CHECK(roundtrip(PROTO_MSG_HEARTBEAT, 1, pl, PROTO_SIZE_HEARTBEAT, &f));
        CHECK(proto_unpack_heartbeat(f.payload, f.len, &r) && r.uptime_s == s.uptime_s);
        CHECK(!proto_unpack_heartbeat(f.payload, f.len - 1, &r));
        CHECK(!proto_unpack_heartbeat(f.payload, f.len + 1, &r));
    }
    {
        proto_ack_t s = { 200 }, r;
        CHECK(proto_pack_ack(&s, pl, sizeof(pl)) == PROTO_SIZE_ACK);
        CHECK(roundtrip(PROTO_MSG_ACK, 2, pl, PROTO_SIZE_ACK, &f));
        CHECK(proto_unpack_ack(f.payload, f.len, &r) && r.acked_seq == 200);
        CHECK(!proto_unpack_ack(f.payload, 0, &r));
    }
    {
        proto_nack_t s = { 9, PROTO_NACK_UNKNOWN_TYPE }, r;
        CHECK(proto_pack_nack(&s, pl, sizeof(pl)) == PROTO_SIZE_NACK);
        CHECK(roundtrip(PROTO_MSG_NACK, 3, pl, PROTO_SIZE_NACK, &f));
        CHECK(proto_unpack_nack(f.payload, f.len, &r) && r.acked_seq == 9 && r.reason == 3);
        CHECK(!proto_unpack_nack(f.payload, 1, &r));
    }
    {
        proto_time_sync_t s = { 1790000000u }, r;
        CHECK(proto_pack_time_sync(&s, pl, sizeof(pl)) == PROTO_SIZE_TIME_SYNC);
        CHECK(roundtrip(PROTO_MSG_TIME_SYNC, 4, pl, PROTO_SIZE_TIME_SYNC, &f));
        CHECK(proto_unpack_time_sync(f.payload, f.len, &r) && r.unix_time == 1790000000u);
        CHECK(!proto_unpack_time_sync(f.payload, 3, &r));
    }
    {
        proto_set_alarm_t s = { 0xFFFFFFFFu }, r;
        CHECK(proto_pack_set_alarm(&s, pl, sizeof(pl)) == PROTO_SIZE_SET_ALARM);
        CHECK(roundtrip(PROTO_MSG_SET_ALARM, 5, pl, PROTO_SIZE_SET_ALARM, &f));
        CHECK(proto_unpack_set_alarm(f.payload, f.len, &r) && r.alarm_unix == 0xFFFFFFFFu);
        CHECK(!proto_unpack_set_alarm(f.payload, 5, &r));
    }
    {
        proto_cmd_t s = { PROTO_CMD_SNOOZE, 0xBEEF }, r;
        CHECK(proto_pack_cmd(&s, pl, sizeof(pl)) == PROTO_SIZE_CMD);
        CHECK(pl[1] == 0xEF && pl[2] == 0xBE);
        CHECK(roundtrip(PROTO_MSG_CMD, 6, pl, PROTO_SIZE_CMD, &f));
        CHECK(proto_unpack_cmd(f.payload, f.len, &r) && r.cmd == 2 && r.arg == 0xBEEF);
        CHECK(!proto_unpack_cmd(f.payload, 2, &r));
    }
    {
        proto_bomb_config_t s, r;
        memset(&s, 0, sizeof(s));
        memset(&r, 0, sizeof(r)); /* zero padding so memcmp is meaningful */
        s.module_count = 3;
        s.module_order[0] = PROTO_MODULE_WIRES;
        s.module_order[1] = PROTO_MODULE_DIAL;
        s.module_order[2] = PROTO_MODULE_TRIPWIRE;
        s.time_limit_s = 300;
        s.dial_code[0] = 4; s.dial_code[1] = 2; s.dial_code[2] = 0; s.dial_code[3] = 9;
        s.wire_count = 5; s.correct_wire = 3;
        s.trip_min_cm = 10; s.trip_max_cm = 25; s.max_strikes = 3;
        CHECK(proto_pack_bomb_config(&s, pl, sizeof(pl)) == PROTO_SIZE_BOMB_CONFIG);
        CHECK(pl[5] == 0x2C && pl[6] == 0x01); /* 300 = 0x012C */
        CHECK(roundtrip(PROTO_MSG_BOMB_CONFIG, 7, pl, PROTO_SIZE_BOMB_CONFIG, &f));
        CHECK(proto_unpack_bomb_config(f.payload, f.len, &r));
        CHECK(memcmp(&s, &r, sizeof(s)) == 0);
        CHECK(!proto_unpack_bomb_config(f.payload, 15, &r));
    }
    {
        proto_tamper_event_t s = { 1500 }, r;
        CHECK(proto_pack_tamper_event(&s, pl, sizeof(pl)) == PROTO_SIZE_TAMPER_EVENT);
        CHECK(roundtrip(PROTO_MSG_TAMPER_EVENT, 8, pl, PROTO_SIZE_TAMPER_EVENT, &f));
        CHECK(proto_unpack_tamper_event(f.payload, f.len, &r) && r.magnitude_mg == 1500);
        CHECK(!proto_unpack_tamper_event(f.payload, 3, &r));
    }
    {
        proto_game_state_t s, r;
        memset(&s, 0, sizeof(s));
        memset(&r, 0, sizeof(r));
        s.state = PROTO_STATE_BOMB_ACTIVE; s.module_idx = 1; s.module_type = PROTO_MODULE_DIAL;
        s.time_left_s = 123; s.strikes = 2; s.progress_pct = 66; s.module_value = -5;
        CHECK(proto_pack_game_state(&s, pl, sizeof(pl)) == PROTO_SIZE_GAME_STATE);
        CHECK(pl[7] == 0xFB && pl[8] == 0xFF); /* -5 as LE int16 */
        CHECK(roundtrip(PROTO_MSG_GAME_STATE, 9, pl, PROTO_SIZE_GAME_STATE, &f));
        CHECK(proto_unpack_game_state(f.payload, f.len, &r));
        CHECK(memcmp(&s, &r, sizeof(s)) == 0 && r.module_value == -5);
        CHECK(!proto_unpack_game_state(f.payload, 8, &r));
    }
    {
        proto_result_msg_t s, r;
        memset(&s, 0, sizeof(s));
        memset(&r, 0, sizeof(r));
        s.outcome = PROTO_OUTCOME_EXPLODED; s.duration_s = 65000; s.strikes = 3; s.snoozes = 2;
        CHECK(proto_pack_result(&s, pl, sizeof(pl)) == PROTO_SIZE_RESULT);
        CHECK(roundtrip(PROTO_MSG_RESULT, 10, pl, PROTO_SIZE_RESULT, &f));
        CHECK(proto_unpack_result(f.payload, f.len, &r));
        CHECK(memcmp(&s, &r, sizeof(s)) == 0);
        CHECK(!proto_unpack_result(f.payload, 4, &r));
    }
    {
        proto_telemetry_t s, r;
        memset(&s, 0, sizeof(s));
        memset(&r, 0, sizeof(r));
        s.joy_x = 2048; s.joy_y = 4095; s.distance_cm = 77; s.encoder_count = -32768;
        CHECK(proto_pack_telemetry(&s, pl, sizeof(pl)) == PROTO_SIZE_TELEMETRY);
        CHECK(roundtrip(PROTO_MSG_TELEMETRY, 11, pl, PROTO_SIZE_TELEMETRY, &f));
        CHECK(proto_unpack_telemetry(f.payload, f.len, &r));
        CHECK(memcmp(&s, &r, sizeof(s)) == 0 && r.encoder_count == -32768);
        CHECK(!proto_unpack_telemetry(f.payload, 9, &r));
    }
}

static void test_pack_rejects_small_buffer(void)
{
    uint8_t small[2];
    proto_game_state_t g;
    proto_heartbeat_t h = { 1 };

    memset(&g, 0, sizeof(g));
    CHECK(proto_pack_heartbeat(&h, small, sizeof(small)) == 0);
    CHECK(proto_pack_game_state(&g, small, sizeof(small)) == 0);
    CHECK(proto_pack_heartbeat(NULL, small, sizeof(small)) == 0);
}

static void test_corrupted_byte_then_recovery(void)
{
    uint8_t a[PROTO_MAX_FRAME], b[PROTO_MAX_FRAME], pl_a[3] = { 1, 2, 3 }, pl_b[2] = { 9, 8 };
    size_t na = proto_encode(0x10, 1, pl_a, 3, a, sizeof(a));
    size_t nb = proto_encode(0x11, 2, pl_b, 2, b, sizeof(b));
    proto_decoder_t d;
    proto_frame_t frames[4];
    size_t nf = 0;
    int crc = 0, len = 0;

    proto_decoder_init(&d);
    a[5] ^= 0xFF; /* corrupt a payload byte */
    feed_all(&d, a, na, frames, &nf, &crc, &len);
    CHECK(nf == 0 && crc == 1 && len == 0 && d.crc_errors == 1);
    feed_all(&d, b, nb, frames, &nf, &crc, &len);
    CHECK(nf == 1 && frames[0].type == 0x11 && frames[0].seq == 2 && frames[0].len == 2);
    CHECK(d.frames_ok == 1);
}

static void test_crc_error_is_reported_on_last_byte(void)
{
    uint8_t a[PROTO_MAX_FRAME], pl[1] = { 5 };
    size_t na = proto_encode(0x02, 1, pl, 1, a, sizeof(a)), i;
    proto_decoder_t d;
    proto_frame_t f;
    proto_result_t r = PROTO_NONE;

    proto_decoder_init(&d);
    a[na - 1] ^= 0x01; /* bad CRC byte */
    for (i = 0; i < na; i++) {
        r = proto_decoder_feed(&d, a[i], &f);
        if (i + 1 < na) { CHECK(r == PROTO_NONE); }
    }
    CHECK(r == PROTO_ERR_CRC);
}

static void test_garbage_before_sof(void)
{
    uint8_t buf[64], pl[2] = { 0x33, 0x44 };
    uint8_t junk[6] = { 0x00, 0x55, 0xFF, 0x12, 0x80, 0x7F };
    size_t n = 0, nf = 0;
    proto_decoder_t d;
    proto_frame_t frames[2];
    int crc = 0, len = 0;

    memcpy(buf, junk, sizeof(junk));
    n = sizeof(junk);
    n += proto_encode(0x13, 4, pl, 2, &buf[n], sizeof(buf) - n);
    proto_decoder_init(&d);
    feed_all(&d, buf, n, frames, &nf, &crc, &len);
    CHECK(nf == 1 && crc == 0 && len == 0);
    CHECK(frames[0].type == 0x13 && frames[0].payload[0] == 0x33 && frames[0].payload[1] == 0x44);
}

static void test_sof_inside_payload(void)
{
    uint8_t pl[4] = { 0xAA, 0x01, 0xAA, 0xAA };
    proto_frame_t f;

    CHECK(roundtrip(0x12, 0xAA, pl, 4, &f));
    CHECK(f.payload[0] == 0xAA && f.payload[3] == 0xAA);
}

static void test_len_over_32_rejected(void)
{
    /* LEN = 33 followed by a good frame. */
    uint8_t buf[64] = { 0xAA, 0x10, 0x01, 33 };
    uint8_t pl[1] = { 7 };
    size_t n = 4, nf = 0;
    proto_decoder_t d;
    proto_frame_t frames[2];
    int crc = 0, len = 0;

    n += proto_encode(0x14, 2, pl, 1, &buf[n], sizeof(buf) - n);
    proto_decoder_init(&d);
    feed_all(&d, buf, n, frames, &nf, &crc, &len);
    CHECK(len == 1 && d.len_errors == 1);
    CHECK(nf == 1 && frames[0].type == 0x14 && frames[0].seq == 2);
}

static void test_truncated_frame_then_good_frame(void)
{
    uint8_t a[PROTO_MAX_FRAME], b[PROTO_MAX_FRAME], buf[2 * PROTO_MAX_FRAME];
    uint8_t pl_a[6] = { 1, 2, 3, 4, 5, 6 }, pl_b[3] = { 0xA, 0xB, 0xC };
    size_t na = proto_encode(0x10, 1, pl_a, 6, a, sizeof(a));
    size_t nb = proto_encode(0x20, 2, pl_b, 3, b, sizeof(b));
    size_t n = 0, nf = 0;
    proto_decoder_t d;
    proto_frame_t frames[4];
    int crc = 0, len = 0;

    /* Frame A cut off after 5 bytes (SOF, TYPE, SEQ, LEN, 1 payload byte). */
    memcpy(buf, a, 5);
    n = 5;
    memcpy(&buf[n], b, nb);
    n += nb;
    (void)na;
    proto_decoder_init(&d);
    feed_all(&d, buf, n, frames, &nf, &crc, &len);
    CHECK(nf == 1);
    CHECK(frames[0].type == 0x20 && frames[0].seq == 2 && frames[0].len == 3);
    CHECK(memcmp(frames[0].payload, pl_b, 3) == 0);
}

static void test_back_to_back_frames(void)
{
    uint8_t buf[2 * PROTO_MAX_FRAME], pl1[2] = { 1, 2 }, pl2[1] = { 0xEE };
    size_t n = 0, nf = 0;
    proto_decoder_t d;
    proto_frame_t frames[4];

    n += proto_encode(0x01, 10, pl1, 2, &buf[n], sizeof(buf) - n);
    n += proto_encode(0x02, 11, pl2, 1, &buf[n], sizeof(buf) - n);
    proto_decoder_init(&d);
    feed_all(&d, buf, n, frames, &nf, NULL, NULL);
    CHECK(nf == 2 && d.frames_ok == 2);
    CHECK(frames[0].seq == 10 && frames[0].len == 2);
    CHECK(frames[1].seq == 11 && frames[1].payload[0] == 0xEE);
}

static void test_zero_length_and_max_length_frames(void)
{
    uint8_t pl[PROTO_MAX_PAYLOAD];
    size_t i;
    proto_frame_t f;

    for (i = 0; i < sizeof(pl); i++) { pl[i] = (uint8_t)(i * 7u); }
    CHECK(roundtrip(0x01, 0, pl, 0, &f));
    CHECK(roundtrip(0x01, 255, pl, PROTO_MAX_PAYLOAD, &f));
}

int main(void)
{
    test_crc_check_value();
    test_encode_basic_and_limits();
    test_message_roundtrips();
    test_pack_rejects_small_buffer();
    test_corrupted_byte_then_recovery();
    test_crc_error_is_reported_on_last_byte();
    test_garbage_before_sof();
    test_sof_inside_payload();
    test_len_over_32_rejected();
    test_truncated_frame_then_good_frame();
    test_back_to_back_frames();
    test_zero_length_and_max_length_frames();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
