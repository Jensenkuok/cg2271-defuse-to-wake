/*
 * Defuse to Wake - UART2 inter-board link (register level). See uart_link.h.
 *
 * RTOS design:
 * - RX is interrupt driven: the ISR only reads the byte, clears error flags and
 *   pushes the byte into a FreeRTOS queue (xQueueSendFromISR). All parsing happens
 *   in the comm task, so the ISR stays a few instructions long.
 * - TX is interrupt driven too: the comm task copies a whole frame into a ring
 *   buffer inside a short critical section and enables TIE; the ISR feeds UART2_D
 *   one byte per TDRE and disables TIE when the ring is empty. No task ever
 *   busy-waits on a UART flag.
 * - The comm task blocks on the RX queue with a 10 ms timeout, so it sleeps when
 *   the line is idle yet still runs proto_link_poll (retransmit timer, 1 s
 *   heartbeat, link-down detection) at least every 10 ms.
 */
#include "uart_link.h"

#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "encoder.h"
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "input_queue.h"
#include "joystick.h"
#include "queue.h"
#include "task.h"
#include "ultrasonic.h"

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} tx_request_t;

#define UART_ERROR_FLAGS (UART_S1_OR_MASK | UART_S1_NF_MASK | UART_S1_FE_MASK | UART_S1_PF_MASK)

/* Owned by the comm task (proto_link_t is single-owner by design). */
static proto_link_t s_link;
static uart_link_hooks_t s_hooks;
static TaskHandle_t s_task;

static QueueHandle_t s_rx_queue;     /* ISR -> comm task, uint8_t items */
static QueueHandle_t s_tx_req_queue; /* any task -> comm task, tx_request_t items */

/* TX ring: written by the comm task in a critical section, read by the ISR. */
static uint8_t s_tx_ring[APP_TX_RING_SIZE];
static volatile uint16_t s_tx_head; /* next write (task) */
static volatile uint16_t s_tx_tail; /* next read (ISR) */

/* Outgoing state-report bookkeeping (comm task only). */
static bool s_gs_sent_once;
static uint32_t s_gs_version;     /* snapshot version last sent */
static uint32_t s_gs_last_ms;
static bool s_tel_started;
static uint32_t s_tel_next_ms;

/* Counters; ISR-written ones are read under a critical section. The new rx_* / *_sent
 * counters have a single writer (the comm task) and 32-bit aligned writes are atomic. */
static uart_link_stats_t s_stats;

static uint32_t now_ms(void)
{
    /* Wraps consistently with uint32_t arithmetic; proto_link compares wrap-safely. */
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- hardware ---- */
static void uart2_hw_init(uint32_t baud)
{
    uint32_t bus_clk;
    uint32_t sbr;

    NVIC_DisableIRQ(UART2_FLEXIO_IRQn);

    SIM->SCGC4 |= SIM_SCGC4_UART2_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;

    UART2->C2 &= ~(UART_C2_TE_MASK | UART_C2_RE_MASK | UART_C2_TIE_MASK | UART_C2_RIE_MASK);

    PORTE->PCR[APP_UART_TX_PIN] &= ~PORT_PCR_MUX_MASK;
    PORTE->PCR[APP_UART_TX_PIN] |= PORT_PCR_MUX(APP_UART_PORT_MUX);
    PORTE->PCR[APP_UART_RX_PIN] &= ~PORT_PCR_MUX_MASK;
    PORTE->PCR[APP_UART_RX_PIN] |= PORT_PCR_MUX(APP_UART_PORT_MUX);

    /* SBR = bus / (16 * baud), rounded to nearest. BDH must be written first. */
    bus_clk = CLOCK_GetBusClkFreq();
    sbr = (bus_clk + (baud * (APP_UART_OVERSAMPLE / 2u))) / (baud * APP_UART_OVERSAMPLE);
    UART2->BDH = (uint8_t)((UART2->BDH & ~UART_BDH_SBR_MASK) |
                           ((sbr >> APP_UART_SBR_HIGH_SHIFT) & UART_BDH_SBR_MASK));
    UART2->BDL = (uint8_t)(sbr & APP_UART_SBR_LOW_MASK);

    /* 8N1, normal (non-loop) mode. */
    UART2->C1 &= ~(UART_C1_LOOPS_MASK | UART_C1_RSRC_MASK | UART_C1_PE_MASK | UART_C1_M_MASK);

    /* RX interrupt on; TIE stays off until there is something to send. */
    UART2->C2 |= UART_C2_RIE_MASK | UART_C2_RE_MASK | UART_C2_TE_MASK;

    /* 64: above GPIO (128) so link bytes are never held up by button/encoder bursts and
     * the 1-byte RX data register cannot overrun. The CM0+ port masks with PRIMASK,
     * so ...FromISR calls are legal at any priority. */
    NVIC_SetPriority(UART2_FLEXIO_IRQn, APP_NVIC_PRIO_UART2);
    NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
    NVIC_EnableIRQ(UART2_FLEXIO_IRQn);
}

/* ISR: no blocking, no loops waiting for input. Read/clear hardware, then FromISR. */
void UART2_FLEXIO_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    uint8_t s1 = UART2->S1;
    uint8_t byte;

    if ((s1 & (UART_S1_RDRF_MASK | UART_ERROR_FLAGS)) != 0u) {
        byte = UART2->D; /* reading S1 then D clears RDRF and OR/NF/FE/PF */
        if ((s1 & UART_ERROR_FLAGS) != 0u) {
            s_stats.uart_errors++;
        }
        if ((s1 & UART_S1_RDRF_MASK) != 0u && (s1 & UART_S1_FE_MASK) == 0u) {
            if (xQueueSendFromISR(s_rx_queue, &byte, &woken) != pdTRUE) {
                s_stats.rx_queue_overflows++;
            }
        }
    }

    if ((UART2->C2 & UART_C2_TIE_MASK) != 0u && (s1 & UART_S1_TDRE_MASK) != 0u) {
        if (s_tx_tail != s_tx_head) {
            UART2->D = s_tx_ring[s_tx_tail];
            s_tx_tail = (uint16_t)((s_tx_tail + 1u) % APP_TX_RING_SIZE);
        } else {
            UART2->C2 &= ~UART_C2_TIE_MASK; /* ring empty: stop TDRE interrupts */
        }
    }

    portYIELD_FROM_ISR(woken);
}

/* ---- proto_link callbacks (run in the comm task) ---- */
static void link_write(const uint8_t *buf, size_t len, void *ctx)
{
    uint16_t used;
    size_t i;

    (void)ctx;
    taskENTER_CRITICAL(); /* short: copy <= 37 bytes; the ISR also touches the ring */
    used = (uint16_t)((s_tx_head + APP_TX_RING_SIZE - s_tx_tail) % APP_TX_RING_SIZE);
    if (len > (size_t)(APP_TX_RING_SIZE - 1u - used)) {
        s_stats.tx_ring_drops++; /* whole frame or nothing; retransmit recovers */
    } else {
        for (i = 0; i < len; i++) {
            s_tx_ring[s_tx_head] = buf[i];
            s_tx_head = (uint16_t)((s_tx_head + 1u) % APP_TX_RING_SIZE);
        }
        UART2->C2 |= UART_C2_TIE_MASK; /* ISR drains the ring */
    }
    taskEXIT_CRITICAL();
}

static void post_ctrl(const app_ctrl_t *c)
{
    if (!app_ctrl_post(c)) {
        s_stats.ctrl_drops++; /* a full queue never blocks the comm task */
    }
}

/* Map one delivered frame to the game: control queue for the rare setup messages,
 * the input queue for commands/tamper (same path as the local buttons). */
static void route_frame(const proto_frame_t *f)
{
    app_ctrl_t c;
    proto_cmd_t cmd;
    proto_tamper_event_t tam;
    proto_time_sync_t ts;
    proto_set_alarm_t sa;

    switch (f->type) {
    case PROTO_MSG_TIME_SYNC:
        if (proto_unpack_time_sync(f->payload, f->len, &ts)) {
            c.kind = APP_CTRL_TIME_SYNC;
            c.u.unix_s = ts.unix_time;
            post_ctrl(&c);
        } else {
            s_stats.rx_bad_payload++;
        }
        break;
    case PROTO_MSG_SET_ALARM:
        if (proto_unpack_set_alarm(f->payload, f->len, &sa)) {
            c.kind = APP_CTRL_SET_ALARM;
            c.u.alarm_unix = sa.alarm_unix;
            post_ctrl(&c);
        } else {
            s_stats.rx_bad_payload++;
        }
        break;
    case PROTO_MSG_BOMB_CONFIG:
        c.kind = APP_CTRL_BOMB_CONFIG;
        if (proto_unpack_bomb_config(f->payload, f->len, &c.u.cfg)) {
            post_ctrl(&c); /* range checks happen in game_set_config */
        } else {
            s_stats.rx_bad_payload++;
        }
        break;
    case PROTO_MSG_CMD:
        if (!proto_unpack_cmd(f->payload, f->len, &cmd)) {
            s_stats.rx_bad_payload++;
        } else if (cmd.cmd == PROTO_CMD_RING_NOW) {
            input_post(GAME_EVT_CMD_RING_NOW, cmd.arg);
        } else if (cmd.cmd == PROTO_CMD_SNOOZE) {
            input_post(GAME_EVT_CMD_SNOOZE, 0);
        } else if (cmd.cmd == PROTO_CMD_CANCEL) {
            input_post(GAME_EVT_CMD_CANCEL, 0);
        } else {
            s_stats.rx_bad_payload++;
        }
        break;
    case PROTO_MSG_TAMPER_EVENT:
        if (proto_unpack_tamper_event(f->payload, f->len, &tam)) {
            input_post(GAME_EVT_TAMPER, tam.magnitude_mg);
        } else {
            s_stats.rx_bad_payload++;
        }
        break;
    default:
        s_stats.rx_ignored++; /* HEARTBEAT is consumed by proto_link; others are not ours */
        break;
    }
}

static void link_deliver(const proto_frame_t *f, void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL();
    if (f->type < UART_LINK_TYPE_SLOTS) {
        s_stats.rx_by_type[f->type]++;
    } else {
        s_stats.rx_other_type++;
    }
    taskEXIT_CRITICAL();
    route_frame(f);
    if (s_hooks.on_frame != NULL) {
        s_hooks.on_frame(f);
    }
}

static void link_event(proto_event_t evt, uint8_t type, uint8_t seq, void *ctx)
{
    (void)type;
    (void)seq;
    (void)ctx;
    if (s_hooks.on_link_change == NULL) {
        return;
    }
    if (evt == PROTO_EVT_LINK_UP) {
        s_hooks.on_link_change(true);
    } else if (evt == PROTO_EVT_LINK_DOWN) {
        s_hooks.on_link_change(false);
    }
}

/* ---- outgoing reports (comm task) ---- */
static int16_t clamp_i16(int32_t v)
{
    return (int16_t)((v > APP_TELEMETRY_ENC_MAX) ? APP_TELEMETRY_ENC_MAX :
                     ((v < -APP_TELEMETRY_ENC_MAX) ? -APP_TELEMETRY_ENC_MAX : v));
}

/* GAME_STATE (unreliable): sent when the snapshot version changed, but at most every
 * APP_GAME_STATE_MIN_MS. A change inside that window stays pending (the version is only
 * marked sent after the frame is queued), so it always goes out later. Only while the
 * link is up, so a reconnecting ESP32 immediately gets the current state. */
static void send_game_state(uint32_t now)
{
    app_state_t st;
    uint8_t pl[PROTO_SIZE_GAME_STATE];

    if (!proto_link_is_up(&s_link)) {
        return;
    }
    if (s_gs_sent_once && (uint32_t)(now - s_gs_last_ms) < APP_GAME_STATE_MIN_MS) {
        return;
    }
    if (!app_state_read(&st)) {
        return;
    }
    if (s_gs_sent_once && st.version == s_gs_version) {
        return;
    }
    if (proto_pack_game_state(&st.view, pl, sizeof(pl)) != 0u &&
        proto_link_send(&s_link, PROTO_MSG_GAME_STATE, pl, PROTO_SIZE_GAME_STATE, now)) {
        s_gs_sent_once = true;
        s_gs_version = st.version;
        s_gs_last_ms = now;
        s_stats.game_state_sent++;
    }
}

/* TELEMETRY (unreliable) every APP_TELEMETRY_MS from the driver getters. */
static void send_telemetry(uint32_t now)
{
    proto_telemetry_t t;
    uint8_t pl[PROTO_SIZE_TELEMETRY];
    uint16_t x;
    uint16_t y;

    if (!proto_link_is_up(&s_link)) {
        return;
    }
    if (s_tel_started && (int32_t)(now - s_tel_next_ms) < 0) {
        return;
    }
    joystick_get_raw(&x, &y);
    t.joy_x = x;
    t.joy_y = y;
    t.distance_cm = (uint16_t)ultrasonic_last_cm();
    t.encoder_count = clamp_i16(encoder_count());
    s_tel_started = true;
    s_tel_next_ms = now + APP_TELEMETRY_MS;
    if (proto_pack_telemetry(&t, pl, sizeof(pl)) != 0u &&
        proto_link_send(&s_link, PROTO_MSG_TELEMETRY, pl, PROTO_SIZE_TELEMETRY, now)) {
        s_stats.telemetry_sent++;
    }
}

static void send_reports(uint32_t now)
{
    send_game_state(now);
    send_telemetry(now);
}

/* ---- comm task ---- */
static void comm_task(void *arg)
{
    uint8_t byte;
    tx_request_t req;

    (void)arg;
    for (;;) {
        /* Block until a byte arrives or 10 ms pass; then drain everything queued. */
        if (xQueueReceive(s_rx_queue, &byte, pdMS_TO_TICKS(APP_COMM_RX_WAIT_MS)) == pdTRUE) {
            do {
                proto_link_rx_byte(&s_link, byte, now_ms());
            } while (xQueueReceive(s_rx_queue, &byte, 0) == pdTRUE);
        }
        while (xQueueReceive(s_tx_req_queue, &req, 0) == pdTRUE) {
            (void)proto_link_send(&s_link, req.type, req.payload, req.len, now_ms());
        }
        proto_link_poll(&s_link, now_ms());
        send_reports(now_ms());
    }
}

/* ---- public API ---- */
bool uart_link_init(const uart_link_hooks_t *hooks)
{
    proto_link_cfg_t cfg;

    memset(&s_hooks, 0, sizeof(s_hooks));
    if (hooks != NULL) {
        s_hooks = *hooks;
    }
    s_rx_queue = xQueueCreate(APP_RX_QUEUE_LEN, sizeof(uint8_t));
    s_tx_req_queue = xQueueCreate(APP_TX_REQ_QUEUE_LEN, sizeof(tx_request_t));
    if (s_rx_queue == NULL || s_tx_req_queue == NULL) {
        return false;
    }

    memset(&cfg, 0, sizeof(cfg)); /* timeout/retries 0 -> proto_link defaults */
    cfg.write_fn = link_write;
    cfg.deliver_fn = link_deliver;
    cfg.event_fn = link_event;
    proto_link_init(&s_link, &cfg, now_ms());

    uart2_hw_init(APP_UART_BAUD);
    return true;
}

bool uart_link_start(void)
{
    return xTaskCreate(comm_task, "comm", APP_STACK_COMM, NULL, APP_PRIO_COMM, &s_task) == pdPASS;
}

bool uart_link_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    tx_request_t req;

    if (len > PROTO_MAX_PAYLOAD || (len > 0u && payload == NULL)) {
        return false;
    }
    req.type = type;
    req.len = len;
    if (len > 0u) {
        memcpy(req.payload, payload, len);
    }
    if (xQueueSend(s_tx_req_queue, &req, 0) != pdTRUE) {
        taskENTER_CRITICAL();
        s_stats.tx_req_drops++;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

void uart_link_get_stats(uart_link_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *out = s_stats;
    out->link = s_link.stats;
    taskEXIT_CRITICAL();
}

TaskHandle_t uart_link_task_handle(void)
{
    return s_task;
}
