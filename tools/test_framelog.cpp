/* Host-side tests for esp32/defuse_esp32/src/app/frame_log.*: ring behaviour, names, filter, corrupt. */
#include <stdio.h>
#include <string.h>

#include "frame_log.h"
#include "../shared/protocol.h"

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

static FrameLogEntry entry(uint32_t ms, uint8_t type, uint8_t outcome)
{
    return frame_log_entry(ms, FLOG_TX, type, (uint8_t)ms, 4u, outcome);
}

static void test_ring_keeps_last_20_newest_first(void)
{
    FrameLogRing ring;
    uint32_t i;

    ring.clear();
    CHECK(FRAME_LOG_LEN == 20);
    for (i = 1; i <= 25; i++) {
        CHECK(frame_log_record(&ring, false, entry(i, PROTO_MSG_BOMB_CONFIG, FLOG_OK)));
    }
    CHECK(ring.size() == 20);
    CHECK(ring.newest(0).ms == 25u);       /* newest first, as the web page lists them */
    CHECK(ring.newest(19).ms == 6u);       /* 1..5 were pushed out */
    CHECK(ring.oldest(0).ms == 6u);
}

static void test_ring_is_empty_at_start(void)
{
    FrameLogRing ring;

    ring.clear();
    CHECK(ring.size() == 0 && ring.valid());
}

static void test_names(void)
{
    CHECK(strcmp(frame_type_name(PROTO_MSG_HEARTBEAT), "HEARTBEAT") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_ACK), "ACK") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_NACK), "NACK") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_TIME_SYNC), "TIME_SYNC") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_SET_ALARM), "SET_ALARM") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_BOMB_CONFIG), "BOMB_CONFIG") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_TAMPER_EVENT), "TAMPER_EVENT") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_CMD), "CMD") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_GAME_STATE), "GAME_STATE") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_RESULT), "RESULT") == 0);
    CHECK(strcmp(frame_type_name(PROTO_MSG_TELEMETRY), "TELEMETRY") == 0);
    CHECK(strcmp(frame_type_name(0), "?") == 0);
    CHECK(strcmp(frame_type_name(0x7F), "?") == 0);
    CHECK(strcmp(frame_dir_name(FLOG_TX), "TX") == 0);
    CHECK(strcmp(frame_dir_name(FLOG_RX), "RX") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_OK), "OK") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_ACK), "ACK") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_NACK), "NACK") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_RETX), "RETX") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_CRC_ERR), "CRC_ERR") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_SEND_FAIL), "SEND_FAIL") == 0);
    CHECK(strcmp(frame_outcome_name(FLOG_FAULT_CRC), "CRC CORRUPTED (fault)") == 0);
    CHECK(strcmp(frame_outcome_name(200), "?") == 0);
}

static void test_reliable_and_periodic_types(void)
{
    /* the same set as proto_link.c: these six need an ACK */
    CHECK(frame_type_is_reliable(PROTO_MSG_TIME_SYNC));
    CHECK(frame_type_is_reliable(PROTO_MSG_SET_ALARM));
    CHECK(frame_type_is_reliable(PROTO_MSG_BOMB_CONFIG));
    CHECK(frame_type_is_reliable(PROTO_MSG_TAMPER_EVENT));
    CHECK(frame_type_is_reliable(PROTO_MSG_CMD));
    CHECK(frame_type_is_reliable(PROTO_MSG_RESULT));
    CHECK(!frame_type_is_reliable(PROTO_MSG_HEARTBEAT));
    CHECK(!frame_type_is_reliable(PROTO_MSG_ACK));
    CHECK(!frame_type_is_reliable(PROTO_MSG_NACK));
    CHECK(!frame_type_is_reliable(PROTO_MSG_GAME_STATE));
    CHECK(!frame_type_is_reliable(PROTO_MSG_TELEMETRY));
    CHECK(!frame_type_is_reliable(0));

    CHECK(frame_type_is_periodic(PROTO_MSG_HEARTBEAT));
    CHECK(frame_type_is_periodic(PROTO_MSG_GAME_STATE));
    CHECK(frame_type_is_periodic(PROTO_MSG_TELEMETRY));
    CHECK(!frame_type_is_periodic(PROTO_MSG_ACK));
    CHECK(!frame_type_is_periodic(PROTO_MSG_BOMB_CONFIG));
}

static void test_filter_drops_periodic_ok_but_never_errors(void)
{
    FrameLogRing ring;

    ring.clear();
    CHECK(!frame_log_record(&ring, false, entry(1, PROTO_MSG_HEARTBEAT, FLOG_OK)));
    CHECK(!frame_log_record(&ring, false, entry(2, PROTO_MSG_GAME_STATE, FLOG_OK)));
    CHECK(!frame_log_record(&ring, false, entry(3, PROTO_MSG_TELEMETRY, FLOG_OK)));
    CHECK(ring.size() == 0);
    CHECK(frame_log_record(&ring, false, entry(4, PROTO_MSG_ACK, FLOG_ACK)));
    CHECK(frame_log_record(&ring, false, entry(5, PROTO_MSG_NACK, FLOG_NACK)));
    CHECK(frame_log_record(&ring, false, entry(6, 0, FLOG_CRC_ERR)));
    CHECK(frame_log_record(&ring, false, entry(7, PROTO_MSG_BOMB_CONFIG, FLOG_RETX)));
    CHECK(frame_log_record(&ring, false, entry(8, PROTO_MSG_BOMB_CONFIG, FLOG_SEND_FAIL)));
    CHECK(frame_log_record(&ring, false, entry(9, PROTO_MSG_BOMB_CONFIG, FLOG_FAULT_CRC)));
    CHECK(ring.size() == 6);
    /* an error on a periodic type is still kept */
    CHECK(frame_log_record(&ring, false, entry(10, PROTO_MSG_HEARTBEAT, FLOG_RETX)));
    /* with periodic logging on, everything is stored */
    CHECK(frame_log_record(&ring, true, entry(11, PROTO_MSG_HEARTBEAT, FLOG_OK)));
    CHECK(ring.size() == 8);
}

static void test_log_seq_uses_acked_seq_for_ack_and_nack(void)
{
    const uint8_t ack[1] = { 7 };
    const uint8_t nack[2] = { 255, PROTO_NACK_CRC };

    CHECK(frame_log_seq(PROTO_MSG_ACK, 40, ack, 1) == 7);
    CHECK(frame_log_seq(PROTO_MSG_NACK, 41, nack, 2) == 255);
    CHECK(frame_log_seq(PROTO_MSG_ACK, 40, ack, 0) == 40);          /* malformed: own seq */
    CHECK(frame_log_seq(PROTO_MSG_ACK, 40, NULL, 1) == 40);
    CHECK(frame_log_seq(PROTO_MSG_BOMB_CONFIG, 12, ack, 1) == 12);
}

static void test_outcome_helpers(void)
{
    CHECK(frame_tx_outcome(PROTO_MSG_BOMB_CONFIG, false, false) == FLOG_OK);
    CHECK(frame_tx_outcome(PROTO_MSG_BOMB_CONFIG, true, false) == FLOG_RETX);
    CHECK(frame_tx_outcome(PROTO_MSG_BOMB_CONFIG, false, true) == FLOG_FAULT_CRC);
    CHECK(frame_tx_outcome(PROTO_MSG_BOMB_CONFIG, true, true) == FLOG_FAULT_CRC);   /* the fault is the more important fact */
    CHECK(frame_tx_outcome(PROTO_MSG_ACK, false, false) == FLOG_ACK);               /* our own ACK / NACK frames */
    CHECK(frame_tx_outcome(PROTO_MSG_NACK, false, false) == FLOG_NACK);
    CHECK(frame_tx_outcome(PROTO_MSG_HEARTBEAT, false, false) == FLOG_OK);
    CHECK(frame_rx_outcome(PROTO_MSG_ACK) == FLOG_ACK);
    CHECK(frame_rx_outcome(PROTO_MSG_NACK) == FLOG_NACK);
    CHECK(frame_rx_outcome(PROTO_MSG_RESULT) == FLOG_OK);
    CHECK(frame_rx_outcome(PROTO_MSG_HEARTBEAT) == FLOG_OK);
}

static void test_entry_fields(void)
{
    FrameLogEntry e = frame_log_entry(123456u, FLOG_RX, PROTO_MSG_RESULT, 9, 5, FLOG_OK);

    CHECK(e.ms == 123456u && e.dir == FLOG_RX && e.type == PROTO_MSG_RESULT);
    CHECK(e.seq == 9 && e.len == 5 && e.outcome == FLOG_OK);
}

/* The corrupted frame must fail the receiver's CRC, and the intact one must pass. */
static void test_corrupt_crc_is_detected_by_the_decoder(void)
{
    uint8_t payload[PROTO_SIZE_BOMB_CONFIG];
    uint8_t frame[PROTO_MAX_FRAME];
    uint8_t orig[PROTO_MAX_FRAME];
    proto_decoder_t dec;
    proto_frame_t out;
    proto_result_t res = PROTO_NONE;
    size_t n;
    size_t i;

    memset(payload, 0x5A, sizeof(payload));
    n = proto_encode(PROTO_MSG_BOMB_CONFIG, 5, payload, sizeof(payload), frame, sizeof(frame));
    CHECK(n == PROTO_SIZE_BOMB_CONFIG + 5u);
    memcpy(orig, frame, n);

    proto_decoder_init(&dec);
    for (i = 0; i < n; i++) {
        res = proto_decoder_feed(&dec, orig[i], &out);
    }
    CHECK(res == PROTO_FRAME_OK && out.type == PROTO_MSG_BOMB_CONFIG && out.seq == 5);

    frame_corrupt_crc(frame, n);
    CHECK(frame[n - 1] == (uint8_t)(orig[n - 1] ^ 0xFFu));
    CHECK(memcmp(frame, orig, n - 1u) == 0);                        /* only the CRC byte changed */
    proto_decoder_init(&dec);
    for (i = 0; i < n; i++) {
        res = proto_decoder_feed(&dec, frame[i], &out);
    }
    CHECK(res == PROTO_ERR_CRC);
    CHECK(dec.crc_errors == 1u);

    frame_corrupt_crc(frame, 0);                                    /* must not touch anything */
    CHECK(frame[n - 1] == (uint8_t)(orig[n - 1] ^ 0xFFu));
}

int main(void)
{
    test_ring_is_empty_at_start();
    test_ring_keeps_last_20_newest_first();
    test_names();
    test_reliable_and_periodic_types();
    test_filter_drops_periodic_ok_but_never_errors();
    test_log_seq_uses_acked_seq_for_ack_and_nack();
    test_outcome_helpers();
    test_entry_fields();
    test_corrupt_crc_is_detected_by_the_decoder();
    printf("test_framelog: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
