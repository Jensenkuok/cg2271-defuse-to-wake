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
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "queue.h"
#include "task.h"

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} tx_request_t;

#define UART_ERROR_FLAGS (UART_S1_OR_MASK | UART_S1_NF_MASK | UART_S1_FE_MASK | UART_S1_PF_MASK)

/* Owned by the comm task (proto_link_t is single-owner by design). */
static proto_link_t s_link;
static uart_link_hooks_t s_hooks;

static QueueHandle_t s_rx_queue;     /* ISR -> comm task, uint8_t items */
static QueueHandle_t s_tx_req_queue; /* any task -> comm task, tx_request_t items */

/* TX ring: written by the comm task in a critical section, read by the ISR. */
static uint8_t s_tx_ring[APP_TX_RING_SIZE];
static volatile uint16_t s_tx_head; /* next write (task) */
static volatile uint16_t s_tx_tail; /* next read (ISR) */

/* Counters; ISR-written ones are read under a critical section. */
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
    return xTaskCreate(comm_task, "comm", APP_STACK_COMM, NULL, APP_PRIO_COMM, NULL) == pdPASS;
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
