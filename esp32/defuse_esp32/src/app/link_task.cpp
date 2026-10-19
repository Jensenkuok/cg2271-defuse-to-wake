/* Defuse to Wake - ESP32 link task. See link_task.h. */
#include "link_task.h"

#include <string.h>

#include "app_state.h"
#include "bomb_config.h"
#include "config.h"
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
static bool s_boot_pressed;        /* set by poll_button, consumed by service_game_config (both in this task) */
static uint32_t s_tx_backpressure;
static uint8_t s_cfg_handed;       /* BOMB_CONFIG frames given to proto_link whose outcome is not reported yet */
static uint8_t s_cfg_failed_events;/* PROTO_EVT_SEND_FAILED for BOMB_CONFIG not yet reported */

/* ---- proto_link callbacks (run inside the link task) ---- */
static void link_write(const uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    Serial1.write(buf, len); /* a frame is <= 37 bytes; the core's TX buffer holds several */
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
        s_boot_pressed = true;
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
    if (s_boot_pressed) {
        s_boot_pressed = false;
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

    memset(&cfg, 0, sizeof(cfg)); /* timeout / retries 0 -> proto_link defaults */
    cfg.write_fn = link_write;
    cfg.deliver_fn = link_deliver;
    cfg.event_fn = link_event;
    proto_link_init(&s_link, &cfg, millis());

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_LINK_LOOP_MS));
        now = millis();

        while (Serial1.available() > 0) {
            proto_link_rx_byte(&s_link, (uint8_t)Serial1.read(), now);
        }
        proto_link_poll(&s_link, now);

        if ((uint32_t)(now - last_button) >= APP_BUTTON_POLL_MS) {
            last_button = now;
            poll_button();
        }
        if (s_boot_pressed || (uint32_t)(now - last_cfg) >= APP_BOMB_CFG_POLL_MS) {
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
