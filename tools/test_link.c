/*
 * Host-side tests for shared/proto_link.{h,c}. Two links are wired back to back
 * through a simulated channel with a fake clock and fault injection.
 * Faults match "the next N frames of a given type" (type -1 = any type).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto_link.h"

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

#define QUEUE_LEN 64
#define LOG_LEN 128
#define MAX_DELIVERED 16
#define MAX_EVENTS 16
#define ANY_TYPE (-1)

typedef struct {
    uint8_t data[PROTO_MAX_FRAME];
    size_t len;
} wire_frame_t;

typedef struct {
    wire_frame_t queue[QUEUE_LEN];
    size_t head;
    size_t count;
    wire_frame_t log[LOG_LEN]; /* every frame written, before faults */
    size_t n_log;
    int drop_type;
    int drop_left;
    int corrupt_type;
    int corrupt_left;
} channel_t;

typedef struct {
    proto_event_t evt;
    uint8_t type;
    uint8_t seq;
} event_rec_t;

typedef struct {
    proto_link_t link;
    channel_t *tx;
    proto_frame_t delivered[MAX_DELIVERED];
    size_t n_delivered;
    event_rec_t events[MAX_EVENTS];
    size_t n_events;
} node_t;

static node_t g_a, g_b;
static channel_t g_ab, g_ba; /* A->B and B->A */
static uint32_t g_now;

static int fault_hits(int type_filter, int *left, uint8_t frame_type)
{
    if (*left > 0 && (type_filter == ANY_TYPE || type_filter == (int)frame_type)) {
        (*left)--;
        return 1;
    }
    return 0;
}

static void write_cb(const uint8_t *buf, size_t len, void *ctx)
{
    node_t *n = (node_t *)ctx;
    channel_t *ch = n->tx;
    wire_frame_t wf;

    memcpy(wf.data, buf, len);
    wf.len = len;
    if (ch->n_log < LOG_LEN) {
        ch->log[ch->n_log++] = wf;
    }
    if (fault_hits(ch->drop_type, &ch->drop_left, buf[1])) {
        return;
    }
    if (fault_hits(ch->corrupt_type, &ch->corrupt_left, buf[1])) {
        wf.data[4] ^= 0xFF; /* first payload byte, or the CRC of an empty frame */
    }
    if (ch->count < QUEUE_LEN) {
        ch->queue[(ch->head + ch->count) % QUEUE_LEN] = wf;
        ch->count++;
    }
}

static void deliver_cb(const proto_frame_t *f, void *ctx)
{
    node_t *n = (node_t *)ctx;
    if (n->n_delivered < MAX_DELIVERED) {
        n->delivered[n->n_delivered++] = *f;
    }
}

static void event_cb(proto_event_t evt, uint8_t type, uint8_t seq, void *ctx)
{
    node_t *n = (node_t *)ctx;
    if (n->n_events < MAX_EVENTS) {
        n->events[n->n_events].evt = evt;
        n->events[n->n_events].type = type;
        n->events[n->n_events].seq = seq;
        n->n_events++;
    }
}

static void setup(uint32_t start)
{
    proto_link_cfg_t cfg;

    memset(&g_a, 0, sizeof(g_a));
    memset(&g_b, 0, sizeof(g_b));
    memset(&g_ab, 0, sizeof(g_ab));
    memset(&g_ba, 0, sizeof(g_ba));
    g_ab.drop_type = g_ab.corrupt_type = ANY_TYPE;
    g_ba.drop_type = g_ba.corrupt_type = ANY_TYPE;
    g_a.tx = &g_ab;
    g_b.tx = &g_ba;
    g_now = start;

    memset(&cfg, 0, sizeof(cfg)); /* timeout/retries 0 -> defaults (100 ms, 3) */
    cfg.write_fn = write_cb;
    cfg.deliver_fn = deliver_cb;
    cfg.event_fn = event_cb;
    cfg.ctx = &g_a;
    proto_link_init(&g_a.link, &cfg, start);
    cfg.ctx = &g_b;
    proto_link_init(&g_b.link, &cfg, start);
}

static int pop_frame(channel_t *ch, wire_frame_t *out)
{
    if (ch->count == 0) {
        return 0;
    }
    *out = ch->queue[ch->head];
    ch->head = (ch->head + 1) % QUEUE_LEN;
    ch->count--;
    return 1;
}

/* Move frames across the channel until both directions are empty. */
static void pump(void)
{
    wire_frame_t wf;
    size_t i;

    while (g_ab.count > 0 || g_ba.count > 0) {
        while (pop_frame(&g_ab, &wf)) {
            for (i = 0; i < wf.len; i++) {
                proto_link_rx_byte(&g_b.link, wf.data[i], g_now);
            }
        }
        while (pop_frame(&g_ba, &wf)) {
            for (i = 0; i < wf.len; i++) {
                proto_link_rx_byte(&g_a.link, wf.data[i], g_now);
            }
        }
    }
}

static void tick(uint32_t ms)
{
    uint32_t i;
    for (i = 0; i < ms; i++) {
        g_now++;
        proto_link_poll(&g_a.link, g_now);
        proto_link_poll(&g_b.link, g_now);
        pump();
    }
}

static int count_logged(const channel_t *ch, uint8_t type)
{
    size_t i;
    int n = 0;
    for (i = 0; i < ch->n_log; i++) {
        if (ch->log[i].data[1] == type) {
            n++;
        }
    }
    return n;
}

/* Send a CMD from A with a recognisable arg. */
static bool send_cmd(node_t *n, uint16_t arg)
{
    proto_cmd_t c;
    uint8_t pl[PROTO_SIZE_CMD];

    c.cmd = PROTO_CMD_RING_NOW;
    c.arg = arg;
    proto_pack_cmd(&c, pl, sizeof(pl));
    return proto_link_send(&n->link, PROTO_MSG_CMD, pl, PROTO_SIZE_CMD, g_now);
}

static uint16_t delivered_cmd_arg(const node_t *n, size_t idx)
{
    proto_cmd_t c;
    c.arg = 0xFFFF;
    proto_unpack_cmd(n->delivered[idx].payload, n->delivered[idx].len, &c);
    return c.arg;
}

/* Inject raw bytes into a link as if received from the wire. */
static void inject(node_t *n, const uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        proto_link_rx_byte(&n->link, buf[i], g_now);
    }
}

/* ---------------- tests ---------------- */

static void test_reliable_delivery_and_ack(void)
{
    proto_cmd_t c;

    setup(0);
    CHECK(!proto_link_is_up(&g_a.link) && !proto_link_is_up(&g_b.link));
    CHECK(send_cmd(&g_a, 0x1234));
    pump();
    CHECK(g_b.n_delivered == 1);
    CHECK(g_b.delivered[0].type == PROTO_MSG_CMD && g_b.delivered[0].seq == 0);
    CHECK(proto_unpack_cmd(g_b.delivered[0].payload, g_b.delivered[0].len, &c) && c.arg == 0x1234);
    CHECK(count_logged(&g_ba, PROTO_MSG_ACK) == 1);
    CHECK(!g_a.link.out_active);
    CHECK(g_a.n_delivered == 0); /* ACKs are not delivered to the application */
    CHECK(g_a.link.stats.tx_frames == 1 && g_a.link.stats.rx_frames == 1);
    CHECK(g_b.link.stats.tx_frames == 1 && g_b.link.stats.rx_frames == 1);
    CHECK(g_a.link.stats.retransmits == 0);
    CHECK(proto_link_is_up(&g_a.link) && proto_link_is_up(&g_b.link));
    CHECK(g_a.n_events == 1 && g_a.events[0].evt == PROTO_EVT_LINK_UP);
    tick(500);
    CHECK(g_a.link.stats.retransmits == 0); /* nothing left outstanding */
}

static void test_unreliable_types_not_acked(void)
{
    uint8_t pl[PROTO_SIZE_TELEMETRY] = { 0 };

    setup(0);
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_HEARTBEAT, pl, PROTO_SIZE_HEARTBEAT, g_now));
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_GAME_STATE, pl, PROTO_SIZE_GAME_STATE, g_now));
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_TELEMETRY, pl, PROTO_SIZE_TELEMETRY, g_now));
    pump();
    CHECK(g_b.n_delivered == 3);
    CHECK(g_b.link.stats.tx_frames == 0 && g_ba.n_log == 0);
    CHECK(g_a.link.stats.tx_frames == 3 && !g_a.link.out_active);
}

static void test_unreliable_sent_while_reliable_outstanding(void)
{
    uint8_t pl[PROTO_SIZE_TELEMETRY] = { 0 };

    setup(0);
    g_ab.drop_type = PROTO_MSG_CMD;
    g_ab.drop_left = 1;
    CHECK(send_cmd(&g_a, 1));
    CHECK(g_a.link.out_active);
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_TELEMETRY, pl, PROTO_SIZE_TELEMETRY, g_now));
    pump();
    CHECK(g_b.n_delivered == 1 && g_b.delivered[0].type == PROTO_MSG_TELEMETRY);
    CHECK(g_a.link.out_active); /* still waiting for its ACK */
}

static void test_retransmit_after_timeout(void)
{
    setup(0);
    g_ab.drop_type = PROTO_MSG_CMD;
    g_ab.drop_left = 1;
    CHECK(send_cmd(&g_a, 7));
    pump();
    CHECK(g_b.n_delivered == 0);
    tick(99);
    CHECK(g_a.link.stats.retransmits == 0);
    tick(1);
    CHECK(g_a.link.stats.retransmits == 1);
    CHECK(g_b.n_delivered == 1 && g_b.delivered[0].seq == 0); /* same SEQ reused */
    CHECK(!g_a.link.out_active);
    CHECK(g_a.link.stats.tx_frames == 1); /* the retransmit is not a new frame */
}

static void test_send_failed_after_max_retries_then_queue_advances(void)
{
    proto_set_alarm_t sa;
    uint8_t pl[PROTO_SIZE_SET_ALARM];

    setup(0);
    g_ab.drop_type = PROTO_MSG_CMD;
    g_ab.drop_left = 100;
    CHECK(send_cmd(&g_a, 1));
    sa.alarm_unix = 1234567u;
    proto_pack_set_alarm(&sa, pl, sizeof(pl));
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_SET_ALARM, pl, sizeof(pl), g_now));
    pump();
    tick(399);
    CHECK(g_a.link.stats.retransmits == 3 && g_a.link.stats.send_failures == 0);
    tick(1);
    CHECK(g_a.link.stats.send_failures == 1);
    CHECK(g_a.link.stats.retransmits == 3);
    CHECK(count_logged(&g_ab, PROTO_MSG_CMD) == 4); /* original + 3 retransmits */
    CHECK(g_a.n_events == 2); /* LINK_UP happens only after B's reply */
    CHECK(g_a.events[0].evt == PROTO_EVT_SEND_FAILED);
    CHECK(g_a.events[0].type == PROTO_MSG_CMD && g_a.events[0].seq == 0);
    /* The queued SET_ALARM went out next and got through. */
    CHECK(g_b.n_delivered == 1 && g_b.delivered[0].type == PROTO_MSG_SET_ALARM);
    CHECK(g_b.delivered[0].seq == 1);
    CHECK(!g_a.link.out_active);
}

static void test_duplicate_suppressed_when_ack_lost(void)
{
    setup(0);
    g_ba.drop_type = PROTO_MSG_ACK;
    g_ba.drop_left = 1;
    CHECK(send_cmd(&g_a, 42));
    pump();
    CHECK(g_b.n_delivered == 1);
    CHECK(g_a.link.out_active);
    tick(99);
    CHECK(g_b.link.stats.dup_drops == 0);
    tick(1);
    CHECK(g_a.link.stats.retransmits == 1);
    CHECK(g_b.link.stats.dup_drops == 1);
    CHECK(g_b.n_delivered == 1);                    /* not delivered twice */
    CHECK(count_logged(&g_ba, PROTO_MSG_ACK) == 2); /* but re-ACKed */
    CHECK(!g_a.link.out_active);
}

static void test_nack_on_crc_error_triggers_immediate_retransmit(void)
{
    setup(0);
    g_ab.corrupt_type = PROTO_MSG_CMD;
    g_ab.corrupt_left = 1;
    CHECK(send_cmd(&g_a, 99));
    pump(); /* no clock advance: recovery must not wait for the timeout */
    CHECK(g_b.link.stats.crc_errors == 1 && g_b.link.stats.nacks_sent == 1);
    CHECK(g_a.link.stats.retransmits == 1);
    CHECK(g_b.n_delivered == 1 && delivered_cmd_arg(&g_b, 0) == 99);
    CHECK(!g_a.link.out_active);
    /* The NACK carried acked_seq 0xFF and reason 1. */
    {
        proto_nack_t n;
        size_t i;
        int found = 0;
        for (i = 0; i < g_ba.n_log; i++) {
            if (g_ba.log[i].data[1] == PROTO_MSG_NACK) {
                found = proto_unpack_nack(&g_ba.log[i].data[4], g_ba.log[i].data[3], &n)
                        && n.acked_seq == 0xFF && n.reason == PROTO_NACK_CRC;
            }
        }
        CHECK(found);
    }
}

static void test_len_error_sends_nack_reason_2(void)
{
    const uint8_t bad[4] = { 0xAA, PROTO_MSG_CMD, 5, 33 };
    proto_nack_t n;

    setup(0);
    inject(&g_b, bad, sizeof(bad));
    CHECK(g_b.link.stats.len_errors == 1 && g_b.link.stats.nacks_sent == 1);
    CHECK(g_ba.n_log == 1 && g_ba.log[0].data[1] == PROTO_MSG_NACK);
    CHECK(proto_unpack_nack(&g_ba.log[0].data[4], 2, &n) && n.acked_seq == 0xFF && n.reason == 2);
    CHECK(g_b.n_delivered == 0);
}

static void test_unknown_type_gets_reason_3_nack(void)
{
    uint8_t raw[PROTO_MAX_FRAME];
    uint8_t pl[1] = { 1 };
    size_t n = proto_encode(0x7E, 77, pl, 1, raw, sizeof(raw));
    proto_nack_t nk;

    setup(0);
    inject(&g_b, raw, n);
    CHECK(g_b.n_delivered == 0);
    CHECK(g_b.link.stats.nacks_sent == 1 && g_ba.n_log == 1);
    CHECK(g_ba.log[0].data[1] == PROTO_MSG_NACK);
    CHECK(proto_unpack_nack(&g_ba.log[0].data[4], 2, &nk) && nk.acked_seq == 77 && nk.reason == 3);
    CHECK(proto_link_is_up(&g_b.link)); /* a valid frame, even if unknown */
}

static void test_ack_with_wrong_seq_ignored(void)
{
    uint8_t raw[PROTO_MAX_FRAME];
    uint8_t pl[1];
    size_t n;

    setup(0);
    g_ab.drop_type = PROTO_MSG_CMD;
    g_ab.drop_left = 1;
    CHECK(send_cmd(&g_a, 5)); /* seq 0 */
    pl[0] = 9;
    n = proto_encode(PROTO_MSG_ACK, 0, pl, 1, raw, sizeof(raw));
    inject(&g_a, raw, n);
    CHECK(g_a.link.out_active);
    pl[0] = 0;
    n = proto_encode(PROTO_MSG_ACK, 1, pl, 1, raw, sizeof(raw));
    inject(&g_a, raw, n);
    CHECK(!g_a.link.out_active);
}

static void test_fifo_capacity_and_argument_checks(void)
{
    uint8_t pl[PROTO_SIZE_CMD] = { 0 };
    uint8_t big[33] = { 0 };
    int i;

    setup(0);
    g_ab.drop_type = ANY_TYPE;
    g_ab.drop_left = 1000; /* keep the first frame outstanding */
    CHECK(send_cmd(&g_a, 0)); /* becomes the outstanding frame */
    for (i = 1; i <= 4; i++) {
        CHECK(send_cmd(&g_a, (uint16_t)i)); /* FIFO slots 1..4 */
    }
    CHECK(!send_cmd(&g_a, 5)); /* FIFO full */
    CHECK(!proto_link_send(&g_a.link, PROTO_MSG_CMD, pl, PROTO_SIZE_CMD, g_now));

    CHECK(!proto_link_send(&g_a.link, PROTO_MSG_TELEMETRY, big, 33, g_now));
    CHECK(!proto_link_send(&g_a.link, PROTO_MSG_TELEMETRY, NULL, 4, g_now));
    CHECK(!proto_link_send(&g_a.link, 0x7E, pl, 1, g_now));
    CHECK(!proto_link_send(NULL, PROTO_MSG_CMD, pl, 1, g_now));
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_TELEMETRY, NULL, 0, g_now)); /* empty payload ok */
}

static void test_fifo_order_preserved(void)
{
    int i;

    setup(0);
    for (i = 1; i <= 5; i++) {
        CHECK(send_cmd(&g_a, (uint16_t)(i * 10)));
    }
    pump();
    CHECK(g_b.n_delivered == 5);
    for (i = 0; i < 5; i++) {
        CHECK(delivered_cmd_arg(&g_b, (size_t)i) == (uint16_t)((i + 1) * 10));
        CHECK(g_b.delivered[i].seq == (uint8_t)i);
    }
    CHECK(!g_a.link.out_active && g_a.link.fifo_count == 0);
    CHECK(g_a.link.stats.retransmits == 0);
}

static void test_queued_payload_is_copied(void)
{
    uint8_t pl[PROTO_SIZE_CMD] = { PROTO_CMD_RING_NOW, 0x11, 0x00 };

    setup(0);
    g_ab.drop_type = PROTO_MSG_CMD;
    g_ab.drop_left = 1;
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_CMD, pl, sizeof(pl), g_now));
    pl[1] = 0x22;
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_CMD, pl, sizeof(pl), g_now));
    pl[1] = 0x33; /* caller reuses its buffer after send */
    tick(100);
    CHECK(g_b.n_delivered == 2);
    CHECK(delivered_cmd_arg(&g_b, 0) == 0x11 && delivered_cmd_arg(&g_b, 1) == 0x22);
}

static void test_heartbeat_every_second(void)
{
    proto_heartbeat_t hb;
    size_t i;
    int hbs = 0;

    setup(0);
    tick(999);
    CHECK(count_logged(&g_ab, PROTO_MSG_HEARTBEAT) == 0);
    tick(1);
    CHECK(count_logged(&g_ab, PROTO_MSG_HEARTBEAT) == 1);
    tick(2500); /* now = 3500 */
    CHECK(count_logged(&g_ab, PROTO_MSG_HEARTBEAT) == 3);
    for (i = 0; i < g_b.n_delivered; i++) {
        if (g_b.delivered[i].type == PROTO_MSG_HEARTBEAT) {
            hbs++;
            CHECK(proto_unpack_heartbeat(g_b.delivered[i].payload, g_b.delivered[i].len, &hb));
            CHECK(hb.uptime_s == (uint32_t)hbs);
        }
    }
    CHECK(hbs == 3);
}

static void test_link_down_after_silence_and_up_again(void)
{
    setup(0);
    tick(1000); /* first heartbeats cross: both up */
    CHECK(proto_link_is_up(&g_a.link) && proto_link_is_up(&g_b.link));

    g_ab.drop_type = ANY_TYPE;
    g_ab.drop_left = 1000;
    g_ba.drop_type = ANY_TYPE;
    g_ba.drop_left = 1000;
    tick(2999); /* t = 3999, last frame heard at t = 1000 */
    CHECK(proto_link_is_up(&g_b.link));
    tick(1);    /* t = 4000 */
    CHECK(!proto_link_is_up(&g_b.link) && !proto_link_is_up(&g_a.link));

    g_ab.drop_left = 0;
    g_ba.drop_left = 0;
    tick(1000); /* heartbeat at t = 5000 gets through */
    CHECK(proto_link_is_up(&g_b.link) && proto_link_is_up(&g_a.link));
    CHECK(g_b.n_events == 3);
    CHECK(g_b.events[0].evt == PROTO_EVT_LINK_UP);
    CHECK(g_b.events[1].evt == PROTO_EVT_LINK_DOWN);
    CHECK(g_b.events[2].evt == PROTO_EVT_LINK_UP);
}

static void test_timing_across_now_ms_wraparound(void)
{
    setup(0xFFFFFF00u);
    tick(200); /* t = 0xFFFFFFC8 */
    g_ab.drop_type = PROTO_MSG_CMD;
    g_ab.drop_left = 1;
    CHECK(send_cmd(&g_a, 3));
    pump();
    tick(99); /* crosses the 32-bit wrap */
    CHECK(g_now < 0x1000u);
    CHECK(g_a.link.stats.retransmits == 0);
    tick(1);
    CHECK(g_a.link.stats.retransmits == 1);
    CHECK(g_b.n_delivered == 1);
    tick(700); /* 1000 ms after init: heartbeat due */
    CHECK(count_logged(&g_ab, PROTO_MSG_HEARTBEAT) == 1);
    CHECK(proto_link_is_up(&g_b.link));
    tick(3001);
    CHECK(count_logged(&g_ab, PROTO_MSG_HEARTBEAT) == 4);

    /* Link-down timing across the wrap too: silence from now on. */
    g_ab.drop_type = ANY_TYPE;
    g_ab.drop_left = 1000;
    g_ba.drop_type = ANY_TYPE;
    g_ba.drop_left = 1000;
    tick(2900);
    CHECK(proto_link_is_up(&g_b.link));
    tick(1100);
    CHECK(!proto_link_is_up(&g_b.link));
}

static void test_stats_match_injected_faults(void)
{
    proto_set_alarm_t sa;
    uint8_t pl[PROTO_SIZE_SET_ALARM];

    setup(0);
    /* Fault 1: first CMD frame corrupted -> CRC error, NACK, immediate retransmit. */
    g_ab.corrupt_type = PROTO_MSG_CMD;
    g_ab.corrupt_left = 1;
    CHECK(send_cmd(&g_a, 1));
    pump();
    /* Fault 2: ACK for SET_ALARM lost -> timeout retransmit, duplicate dropped. */
    g_ba.drop_type = PROTO_MSG_ACK;
    g_ba.drop_left = 1;
    sa.alarm_unix = 42;
    proto_pack_set_alarm(&sa, pl, sizeof(pl));
    CHECK(proto_link_send(&g_a.link, PROTO_MSG_SET_ALARM, pl, sizeof(pl), g_now));
    pump();
    tick(100);

    CHECK(g_a.link.stats.tx_frames == 2);
    CHECK(g_a.link.stats.rx_frames == 3); /* NACK, ACK(CMD), ACK(dup SET_ALARM) */
    CHECK(g_a.link.stats.retransmits == 2);
    CHECK(g_a.link.stats.send_failures == 0);
    CHECK(g_a.link.stats.crc_errors == 0 && g_a.link.stats.len_errors == 0);
    CHECK(g_a.link.stats.dup_drops == 0 && g_a.link.stats.nacks_sent == 0);

    CHECK(g_b.link.stats.tx_frames == 4); /* NACK + 3 ACKs */
    CHECK(g_b.link.stats.rx_frames == 3); /* CMD, SET_ALARM, SET_ALARM dup */
    CHECK(g_b.link.stats.crc_errors == 1 && g_b.link.stats.len_errors == 0);
    CHECK(g_b.link.stats.nacks_sent == 1);
    CHECK(g_b.link.stats.dup_drops == 1);
    CHECK(g_b.link.stats.retransmits == 0);
    CHECK(g_b.n_delivered == 2);
}

int main(void)
{
    test_reliable_delivery_and_ack();
    test_unreliable_types_not_acked();
    test_unreliable_sent_while_reliable_outstanding();
    test_retransmit_after_timeout();
    test_send_failed_after_max_retries_then_queue_advances();
    test_duplicate_suppressed_when_ack_lost();
    test_nack_on_crc_error_triggers_immediate_retransmit();
    test_len_error_sends_nack_reason_2();
    test_unknown_type_gets_reason_3_nack();
    test_ack_with_wrong_seq_ignored();
    test_fifo_capacity_and_argument_checks();
    test_fifo_order_preserved();
    test_queued_payload_is_copied();
    test_heartbeat_every_second();
    test_link_down_after_silence_and_up_again();
    test_timing_across_now_ms_wraparound();
    test_stats_match_injected_faults();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
