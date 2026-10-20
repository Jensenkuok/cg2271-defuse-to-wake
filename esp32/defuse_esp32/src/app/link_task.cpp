/* Defuse to Wake - ESP32 link task. See link_task.h. */
#include "link_task.h"

#include <string.h>

#include "app_state.h"
#include "bomb_config.h"
#include "config.h"
#include "fault.h"
#include "frame_log.h"
#include "../shared/proto_link.h"
#include "../shared/protocol.h"

struct TxMsg {
    uint8_t type;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
};

static QueueHandle_t s_tx_queue;
static QueueHandle_t s_result_queue;   /* RESULT frames for the network task (it records them in flash) */
static TaskHandle_t s_task;
static proto_link_t s_link;        /* owned by the link task only */
static AppState s_st;              /* link task only: a local copy would take ~1 KB of its 4 KB stack */
static uint32_t s_tx_backpressure;
static uint8_t s_cfg_handed;       /* BOMB_CONFIG frames given to proto_link whose outcome is not reported yet */
static uint8_t s_cfg_failed_events;/* PROTO_EVT_SEND_FAILED for BOMB_CONFIG not yet reported */

/* ---- demo-ring request: the BOOT button (this task) or the web page's "Ring now" (web task) ---- */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ring_requested;      /* guarded by s_ring_mux; consumed by service_game_config */

static void ring_request_set(void)
{
    taskENTER_CRITICAL(&s_ring_mux);
    s_ring_requested = true;
    taskEXIT_CRITICAL(&s_ring_mux);
}

static bool ring_request_pending(void)
{
    bool pending;

    taskENTER_CRITICAL(&s_ring_mux);
    pending = s_ring_requested;
    taskEXIT_CRITICAL(&s_ring_mux);
    return pending;
}

static bool ring_request_take(void)
{
    bool pending;

    taskENTER_CRITICAL(&s_ring_mux);
    pending = s_ring_requested;
    s_ring_requested = false;
    taskEXIT_CRITICAL(&s_ring_mux);
    return pending;
}

/* ---- frame log (Engineering tab): the last FRAME_LOG_LEN frames of the MCX link ----
 * The link task writes, the web task copies. A critical section (not a mutex): the push and the copy
 * are a few dozen bytes, and the priority-4 link task must never wait on a lower-priority reader. */
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static FrameLogRing s_frame_log;   /* guarded by s_log_mux */
static proto_decoder_t s_rx_tap;   /* link task only: a second decoder fed the same bytes as proto_link, so ACK /
                                    * NACK / duplicate frames and CRC errors (which proto_link consumes) are logged */
static uint32_t s_seen_retx;       /* proto_link's retransmit counter at the previous write */

static void log_frame(uint8_t dir, uint8_t type, uint8_t seq, uint8_t len, uint8_t outcome)
{
    const FrameLogEntry e = frame_log_entry(millis(), dir, type, seq, len, outcome);

    taskENTER_CRITICAL(&s_log_mux);
    (void)frame_log_record(&s_frame_log, APP_FRAME_LOG_PERIODIC != 0, e);
    taskEXIT_CRITICAL(&s_log_mux);
}

/* Every byte that goes to proto_link goes through here first. A CRC or length error is recorded as
 * CRC_ERR (the frame's type is unknown); a good frame by its type, seq and outcome. */
static void rx_tap_byte(uint8_t byte)
{
    proto_frame_t f;
    const uint32_t errors_before = s_rx_tap.crc_errors + s_rx_tap.len_errors;
    const proto_result_t res = proto_decoder_feed(&s_rx_tap, byte, &f);

    if (s_rx_tap.crc_errors + s_rx_tap.len_errors != errors_before) {
        log_frame(FLOG_RX, 0u, 0u, 0u, FLOG_CRC_ERR);
    }
    if (res == PROTO_FRAME_OK) {
        log_frame(FLOG_RX, f.type, frame_log_seq(f.type, f.seq, f.payload, f.len), f.len, frame_rx_outcome(f.type));
    }
}

/* ---- proto_link callbacks (run inside the link task) ---- */
/* One call per whole frame. Also the single place where the fault button's "corrupt the next packet"
 * is applied: the CRC byte of one outgoing frame is flipped on its way to the UART, proto_link keeps
 * its intact copy for the retransmit. A retransmit is recognised by proto_link's counter, which it
 * increments just before it writes the frame again. */
static void link_write(const uint8_t *buf, size_t len, void *ctx)
{
    uint8_t copy[PROTO_MAX_FRAME];
    const bool whole = (len >= PROTO_HEADER_SIZE + 1u) && (len <= sizeof(copy));
    const uint8_t type = whole ? buf[1] : 0u;
    const uint8_t seq = whole ? buf[2] : 0u;
    const uint8_t plen = whole ? buf[3] : 0u;
    const bool retx = (s_link.stats.retransmits != s_seen_retx);
    bool corrupted = false;

    (void)ctx;
    s_seen_retx = s_link.stats.retransmits;
    if (whole && (APP_FAULT_CORRUPT_RELIABLE_ONLY == 0 || frame_type_is_reliable(type)) && fault_corrupt_take()) {
        memcpy(copy, buf, len);
        frame_corrupt_crc(copy, len);
        Serial1.write(copy, len);
        corrupted = true;
        Serial.printf("[fault] CRC of %s seq=%u corrupted on the wire (the MCX should NACK, we retransmit)\n",
                      frame_type_name(type), (unsigned)seq);
    } else {
        Serial1.write(buf, len); /* a frame is <= 37 bytes; the core's TX buffer holds several */
    }
    if (whole) {
        log_frame(FLOG_TX, type, frame_log_seq(type, seq, buf + PROTO_HEADER_SIZE, plen), plen,
                  frame_tx_outcome(type, retx, corrupted));
    }
}

static void link_deliver(const proto_frame_t *f, void *ctx)
{
    proto_game_state_t gs;
    proto_telemetry_t tel;
    proto_result_msg_t res;

    (void)ctx;
    switch (f->type) {
    case PROTO_MSG_GAME_STATE:
        if (proto_unpack_game_state(f->payload, f->len, &gs)) {
            app_state_set_game(gs);
        }
        break;
    case PROTO_MSG_TELEMETRY:
        if (proto_unpack_telemetry(f->payload, f->len, &tel)) {
            app_state_set_telemetry(tel);
        }
        break;
    case PROTO_MSG_RESULT:
        if (proto_unpack_result(f->payload, f->len, &res)) {
            app_state_set_result(res);
            /* Never write flash from this prio-4 task: hand the result to the network task. */
            if (xQueueSend(s_result_queue, &res, 0) != pdTRUE) {
                Serial.println("[result] queue full: result not recorded");
            }
            Serial.printf("[result] outcome=%u duration=%us strikes=%u snoozes=%u\n", res.outcome,
                          res.duration_s, res.strikes, res.snoozes);
        }
        break;
    default:
        break; /* HEARTBEAT is consumed inside proto_link */
    }
}

static void link_event(proto_event_t evt, uint8_t type, uint8_t seq, void *ctx)
{
    (void)ctx;
    switch (evt) {
    case PROTO_EVT_LINK_UP:
        app_state_set_link(true);
        Serial.println("[link] UP");
        break;
    case PROTO_EVT_LINK_DOWN:
        app_state_set_link(false);
        Serial.println("[link] DOWN");
        break;
    case PROTO_EVT_SEND_FAILED:
        Serial.printf("[link] SEND_FAILED type=0x%02x seq=%u\n", type, seq);
        log_frame(FLOG_TX, type, seq, 0u, FLOG_SEND_FAIL);
        if (type == PROTO_MSG_BOMB_CONFIG && s_cfg_failed_events < 0xFFu) {
            s_cfg_failed_events++;
        }
        break;
    }
}

/* ---- public ---- */
bool link_task_init(void)
{
    s_tx_queue = xQueueCreate(APP_TX_QUEUE_LEN, sizeof(TxMsg));
    s_result_queue = xQueueCreate(APP_RESULT_QUEUE_LEN, sizeof(proto_result_msg_t));
    return s_tx_queue != NULL && s_result_queue != NULL;
}

bool link_result_pop(proto_result_msg_t *out)
{
    return xQueueReceive(s_result_queue, out, 0) == pdTRUE;
}

bool link_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    TxMsg m;

    if (len > PROTO_MAX_PAYLOAD || (len > 0 && payload == NULL)) {
        return false;
    }
    m.type = type;
    m.len = len;
    if (len > 0) {
        memcpy(m.payload, payload, len);
    }
    return xQueueSend(s_tx_queue, &m, 0) == pdTRUE;
}

uint32_t link_tx_space(void)
{
    return uxQueueSpacesAvailable(s_tx_queue);
}

TaskHandle_t link_task_handle(void)
{
    return s_task;
}

int link_frame_log_copy(FrameLogEntry *out, int max)
{
    int n;
    int i;

    if (out == NULL || max <= 0) {
        return 0;
    }
    taskENTER_CRITICAL(&s_log_mux);
    n = s_frame_log.size() < max ? s_frame_log.size() : max;
    for (i = 0; i < n; i++) {
        out[i] = s_frame_log.newest(i);
    }
    taskEXIT_CRITICAL(&s_log_mux);
    return n;
}

void link_request_demo_ring(void)
{
    ring_request_set();
}

/* Move queued messages into proto_link. If its FIFO is full the head STAYS in the queue
 * (peek, then remove only on success), so nothing is lost; it is retried next loop. */
static void drain_tx_queue(uint32_t now)
{
    TxMsg m;

    while (xQueuePeek(s_tx_queue, &m, 0) == pdTRUE) {
        if (!proto_link_send(&s_link, m.type, m.payload, m.len, now)) {
            s_tx_backpressure++;
            break;
        }
        if (m.type == PROTO_MSG_BOMB_CONFIG && s_cfg_handed < 0xFFu) {
            s_cfg_handed++;
        }
        (void)xQueueReceive(s_tx_queue, &m, 0);
    }
}

/* BOMB_CONFIG frames still inside proto_link (outstanding or waiting in its FIFO). The link task
 * owns the link, so reading these fields here is the owner reading its own state. */
static uint8_t cfg_frames_in_link(void)
{
    uint8_t n = (s_link.out_active && s_link.out_type == PROTO_MSG_BOMB_CONFIG) ? 1u : 0u;
    uint8_t i;

    for (i = 0; i < s_link.fifo_count; i++) {
        if (s_link.fifo[(s_link.fifo_head + i) % PROTO_LINK_TX_DEPTH].type == PROTO_MSG_BOMB_CONFIG) {
            n++;
        }
    }
    return n;
}

/* A BOMB_CONFIG that left proto_link was either ACKed or dropped after the retries (the
 * SEND_FAILED event counted above). The network task learns the outcome through app_state. */
static void report_config_results(void)
{
    const uint8_t in_link = cfg_frames_in_link();

    while (s_cfg_handed > in_link) {
        s_cfg_handed--;
        if (s_cfg_failed_events > 0u) {
            s_cfg_failed_events--;
            app_state_note_config_result(false);
        } else {
            app_state_note_config_result(true);
        }
    }
}

/* BOOT button (GPIO0, active low), 50 ms poll = debounce. A press is only noted here; the demo
 * ring itself runs in service_game_config() of the same pass. */
static void poll_button(void)
{
    static bool s_was_down;
    bool down = (digitalRead(APP_BOOT_BUTTON_PIN) == LOW);

    if (down && !s_was_down) {
        ring_request_set();
        Serial.println("[btn] BOOT pressed: demo ring requested");
    }
    s_was_down = down;
}

/* Demo ring: BOMB_CONFIG with the next puzzle pair first, then CMD RING_NOW (3 s). This runs in the
 * LINK task on purpose (priority 4): the old home, the network task (priority 1), can sit in an LLM
 * call for up to 30 s, so a press during a "puzzles" run rang only after it. Both frames go into the
 * TX queue now and leave in this very pass (drain_tx_queue runs right after). */
static void demo_ring(const AppState &st)
{
    proto_cmd_t cmd;
    uint8_t pl[PROTO_SIZE_CMD];

    if (!st.link_up) {
        Serial.println("[btn] ring ignored: MCX link is down");
        return;
    }
    if (game_ringing(st)) {
        Serial.println("[btn] ring ignored: a bomb is already running");
        return;
    }
    if (!bomb_config_send("demo ring")) {
        Serial.println("[btn] no new BOMB_CONFIG queued: the MCX keeps its previous config");
    }
    cmd.cmd = PROTO_CMD_RING_NOW;
    cmd.arg = APP_BOOT_RING_DELAY_S;
    if (proto_pack_cmd(&cmd, pl, sizeof(pl)) != 0u && link_send(PROTO_MSG_CMD, pl, PROTO_SIZE_CMD)) {
        Serial.println("[btn] RING_NOW queued");
    } else {
        Serial.println("[btn] RING_NOW could not be queued (TX queue full)");
    }
}

/* BOMB_CONFIG bookkeeping + triggers and the BOOT demo ring, every APP_BOMB_CFG_POLL_MS. Only RAM
 * and the non-blocking TX queue are touched (see bomb_config.h); app_state_read waits at most
 * APP_STATE_MUTEX_WAIT_MS for a short memcpy. */
static void service_game_config(void)
{
    if (!app_state_read(&s_st)) {
        return;                                    /* retried next period; a press stays pending */
    }
    if (ring_request_take()) {
        demo_ring(s_st);
    }
    bomb_config_service(s_st);
}

static void link_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    uint32_t last_button = 0;
    uint32_t last_cfg = 0;
    uint32_t last_stats = 0;
    proto_link_cfg_t cfg;
    uint32_t now;

    (void)arg;
    Serial1.begin(APP_UART_BAUD, SERIAL_8N1, APP_UART_RX_PIN, APP_UART_TX_PIN);
    pinMode(APP_BOOT_BUTTON_PIN, INPUT_PULLUP);

    proto_decoder_init(&s_rx_tap);
    memset(&cfg, 0, sizeof(cfg)); /* timeout / retries 0 -> proto_link defaults */
    cfg.write_fn = link_write;
    cfg.deliver_fn = link_deliver;
    cfg.event_fn = link_event;
    proto_link_init(&s_link, &cfg, millis());

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_LINK_LOOP_MS));
        now = millis();

        while (Serial1.available() > 0) {
            const uint8_t byte = (uint8_t)Serial1.read();

            rx_tap_byte(byte);
            proto_link_rx_byte(&s_link, byte, now);
        }
        proto_link_poll(&s_link, now);

        if ((uint32_t)(now - last_button) >= APP_BUTTON_POLL_MS) {
            last_button = now;
            poll_button();
        }
        if (ring_request_pending() || (uint32_t)(now - last_cfg) >= APP_BOMB_CFG_POLL_MS) {
            last_cfg = now;
            service_game_config();         /* before the drain: what it queues leaves in this pass */
        }
        drain_tx_queue(now);
        report_config_results();

        if ((uint32_t)(now - last_stats) >= APP_LINK_STATS_MS) {
            last_stats = now;
            app_state_set_link_stats(s_link.stats, s_tx_backpressure);
        }
    }
}

bool link_task_start(void)
{
    return xTaskCreate(link_task, "link", APP_STACK_LINK, NULL, APP_PRIO_LINK, &s_task) == pdPASS;
}
