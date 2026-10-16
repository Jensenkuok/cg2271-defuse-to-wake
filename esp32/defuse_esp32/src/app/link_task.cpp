/* Defuse to Wake - ESP32 link task. See link_task.h. */
#include "link_task.h"

#include <string.h>

#include "app_state.h"
#include "config.h"
#include "../shared/proto_link.h"
#include "../shared/protocol.h"

struct TxMsg {
    uint8_t type;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
};

static QueueHandle_t s_tx_queue;
static TaskHandle_t s_task;
static proto_link_t s_link;        /* owned by the link task only */
static uint32_t s_tx_backpressure;

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
            /* The next step stores this (Firebase); for now it is only logged. */
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
        break;
    }
}

/* ---- public ---- */
bool link_task_init(void)
{
    s_tx_queue = xQueueCreate(APP_TX_QUEUE_LEN, sizeof(TxMsg));
    return s_tx_queue != NULL;
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
        (void)xQueueReceive(s_tx_queue, &m, 0);
    }
}

/* BOOT button (GPIO0, active low), 50 ms poll = debounce: a press queues RING_NOW. */
static void poll_button(void)
{
    static bool s_was_down;
    bool down = (digitalRead(APP_BOOT_BUTTON_PIN) == LOW);
    proto_cmd_t c;
    uint8_t pl[PROTO_SIZE_CMD];

    if (down && !s_was_down) {
        c.cmd = PROTO_CMD_RING_NOW;
        c.arg = APP_BOOT_RING_DELAY_S;
        if (proto_pack_cmd(&c, pl, sizeof(pl)) != 0) {
            (void)link_send(PROTO_MSG_CMD, pl, PROTO_SIZE_CMD);
            Serial.println("[btn] BOOT pressed: RING_NOW");
        }
    }
    s_was_down = down;
}

static void link_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    uint32_t last_button = 0;
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
        drain_tx_queue(now);

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
