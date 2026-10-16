/*
 * Defuse to Wake - MCXC444 firmware entry point.
 *
 * This step brings up the inter-board link only:
 *   - comm task (priority APP_PRIO_COMM, just below the future game task) owns
 *     the ACK/retransmit link on UART2 (see uart_link.c);
 *   - GREEN LED: solid while the link is up, off while it is down;
 *   - optional LOOPBACK self-test (APP_LOOPBACK_TEST 1, jumper PTE22<->PTE23):
 *     sends a reliable TIME_SYNC every 2 s, toggles BLUE on each delivered frame
 *     and prints the link counters every 10 s.
 * The RGB LED is plain GPIO here; TPM0 PWM replaces it in a later step.
 */
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "peripherals.h"
#include "pin_mux.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app_config.h"
#include "protocol.h"
#include "uart_link.h"

typedef enum { LED_RED, LED_GREEN, LED_BLUE } led_t;

/* ---- RGB LED, plain GPIO, active-low (clear = on) ---- */
static void leds_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK | SIM_SCGC5_PORTE_MASK;
    PORTE->PCR[APP_LED_RED_PIN] = (PORTE->PCR[APP_LED_RED_PIN] & ~PORT_PCR_MUX_MASK) | PORT_PCR_MUX(APP_GPIO_MUX);
    PORTD->PCR[APP_LED_GREEN_PIN] = (PORTD->PCR[APP_LED_GREEN_PIN] & ~PORT_PCR_MUX_MASK) | PORT_PCR_MUX(APP_GPIO_MUX);
    PORTE->PCR[APP_LED_BLUE_PIN] = (PORTE->PCR[APP_LED_BLUE_PIN] & ~PORT_PCR_MUX_MASK) | PORT_PCR_MUX(APP_GPIO_MUX);
    GPIOE->PSOR = (1u << APP_LED_RED_PIN) | (1u << APP_LED_BLUE_PIN); /* all off */
    GPIOD->PSOR = (1u << APP_LED_GREEN_PIN);
    GPIOE->PDDR |= (1u << APP_LED_RED_PIN) | (1u << APP_LED_BLUE_PIN);
    GPIOD->PDDR |= (1u << APP_LED_GREEN_PIN);
}

static void led_set(led_t led, bool on)
{
    switch (led) {
    case LED_RED:
        if (on) { GPIOE->PCOR = (1u << APP_LED_RED_PIN); } else { GPIOE->PSOR = (1u << APP_LED_RED_PIN); }
        break;
    case LED_GREEN:
        if (on) { GPIOD->PCOR = (1u << APP_LED_GREEN_PIN); } else { GPIOD->PSOR = (1u << APP_LED_GREEN_PIN); }
        break;
    default:
        if (on) { GPIOE->PCOR = (1u << APP_LED_BLUE_PIN); } else { GPIOE->PSOR = (1u << APP_LED_BLUE_PIN); }
        break;
    }
}

/* Fatal error: RED solid, everything else stopped. Used by the FreeRTOS hooks. */
static void fatal_halt(void)
{
    taskDISABLE_INTERRUPTS();
    led_set(LED_GREEN, false);
    led_set(LED_BLUE, false);
    led_set(LED_RED, true);
    for (;;) {
        __asm volatile("nop"); /* halted on purpose; attach the debugger to inspect */
    }
}

/* configCHECK_FOR_STACK_OVERFLOW = 2 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    fatal_halt();
}

/* configUSE_MALLOC_FAILED_HOOK = 1 */
void vApplicationMallocFailedHook(void)
{
    fatal_halt();
}

/* ---- link hooks (run in the comm task) ---- */
static void on_link_change(bool up)
{
    led_set(LED_GREEN, up);
}

#if APP_LOOPBACK_TEST
static volatile bool s_blue_on;

static void on_frame(const proto_frame_t *f)
{
    (void)f;
    s_blue_on = !s_blue_on;
    led_set(LED_BLUE, s_blue_on);
}

/* Lowest app priority: it only generates test traffic and prints counters. */
static void loopback_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    uint32_t elapsed_ms = 0;
    uint32_t i;
    proto_time_sync_t ts;
    uint8_t pl[PROTO_SIZE_TIME_SYNC];
    uart_link_stats_t st;

    (void)arg;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_LOOPBACK_SEND_MS));
        elapsed_ms += APP_LOOPBACK_SEND_MS;

        ts.unix_time = elapsed_ms / 1000u; /* any value; the link ACKs its own frame */
        if (proto_pack_time_sync(&ts, pl, sizeof(pl)) != 0u) {
            (void)uart_link_send(PROTO_MSG_TIME_SYNC, pl, PROTO_SIZE_TIME_SYNC);
        }

        if (elapsed_ms % APP_LOOPBACK_PRINT_MS == 0u) {
            uart_link_get_stats(&st);
            PRINTF("\r\n[loopback t=%us] tx=%u rx=%u retx=%u fail=%u crc=%u len=%u dup=%u nack=%u\r\n",
                   (unsigned)(elapsed_ms / 1000u), (unsigned)st.link.tx_frames, (unsigned)st.link.rx_frames,
                   (unsigned)st.link.retransmits, (unsigned)st.link.send_failures,
                   (unsigned)st.link.crc_errors, (unsigned)st.link.len_errors,
                   (unsigned)st.link.dup_drops, (unsigned)st.link.nacks_sent);
            PRINTF("  uart_err=%u rxq_ovf=%u ring_drop=%u req_drop=%u delivered:",
                   (unsigned)st.uart_errors, (unsigned)st.rx_queue_overflows,
                   (unsigned)st.tx_ring_drops, (unsigned)st.tx_req_drops);
            for (i = 0; i < UART_LINK_TYPE_SLOTS; i++) {
                if (st.rx_by_type[i] != 0u) {
                    PRINTF(" 0x%02x=%u", (unsigned)i, (unsigned)st.rx_by_type[i]);
                }
            }
            PRINTF("\r\n");
        }
    }
}
#else
static void on_frame(const proto_frame_t *f)
{
    (void)f; /* counted per type in uart_link; game logic comes in a later step */
}
#endif

int main(void)
{
    static const uart_link_hooks_t hooks = { on_link_change, on_frame };

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif

    leds_init();
    PRINTF("Defuse to Wake MCX: link bring-up%s\r\n", APP_LOOPBACK_TEST ? " (LOOPBACK TEST)" : "");

    if (!uart_link_init(&hooks) || !uart_link_start()) {
        fatal_halt();
    }
#if APP_LOOPBACK_TEST
    if (xTaskCreate(loopback_task, "loopback", APP_STACK_LOOPBACK, NULL, APP_PRIO_LOOPBACK, NULL) != pdPASS) {
        fatal_halt();
    }
#endif

    vTaskStartScheduler();
    fatal_halt(); /* only reached if the idle task could not be created */
    return 0;
}
