/*
 * Defuse to Wake - MCXC444 firmware entry point.
 *
 * Tasks (priorities from CLAUDE.md, configMAX_PRIORITIES = 5):
 *   game (4)        - owns game_t, consumes the input + control queues (not in DRIVER_TEST)
 *   joystick (3)    - 50 Hz ADC sampler, joystick.c
 *   comm (2)        - owns the ACK/retransmit link on UART2, uart_link.c
 *   ultrasonic (2)  - 60 ms HC-SR04 ping; equal to comm so time slicing shares the CPU
 *   feedback (1)    - only owner of TPM0 (RGB LED) and TPM2 (buzzer), feedback.c
 *   debug (1)       - stats / driver test prints, debug.c
 *   loopback (1)    - only with APP_LOOPBACK_TEST
 * ISRs: UART2 (64), TPM1 echo capture (64), ADC0 (128), PORTA encoder+SW3 (128),
 *       PORTC_PORTD buttons (128).
 * All input drivers post game_event_t into ONE input queue (input_queue.c).
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
#include "app_state.h"
#include "buttons.h"
#include "debug.h"
#include "encoder.h"
#include "feedback.h"
#include "game_task.h"
#include "input_queue.h"
#include "joystick.h"
#include "protocol.h"
#include "uart_link.h"
#include "ultrasonic.h"

/* Fatal error: RED LED solid, everything stopped. The red pin is forced back to
 * GPIO because TPM0 normally drives it; register writes are fine here since the
 * scheduler is dead. Used by the FreeRTOS hooks and failed start-up. */
static void fatal_halt(void)
{
    taskDISABLE_INTERRUPTS();
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
    PORTE->PCR[APP_LED_RED_PIN] = PORT_PCR_MUX(APP_GPIO_MUX);
    GPIOE->PDDR |= (1u << APP_LED_RED_PIN);
    GPIOE->PCOR = (1u << APP_LED_RED_PIN); /* active-low: on */
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

/* ---- link hooks (run in the comm task; must not block) ---- */
static void on_link_change(bool up)
{
    feedback_set_link(up); /* shown as a blue blink in LED mode OFF while down */
}

#if APP_LOOPBACK_TEST
static volatile uint32_t s_loop_delivered;

static void on_frame(const proto_frame_t *f)
{
    (void)f;
    s_loop_delivered++;
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
            PRINTF("  uart_err=%u rxq_ovf=%u ring_drop=%u req_drop=%u delivered=%u:",
                   (unsigned)st.uart_errors, (unsigned)st.rx_queue_overflows,
                   (unsigned)st.tx_ring_drops, (unsigned)st.tx_req_drops, (unsigned)s_loop_delivered);
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
    bool ok;

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif
    PRINTF("Defuse to Wake MCX%s%s\r\n", APP_LOOPBACK_TEST ? " [LOOPBACK TEST]" : "",
           APP_DRIVER_TEST ? " [DRIVER TEST]" : "");

    /* Queues and peripherals first (no task runs yet), then the tasks. */
    ok = input_queue_init();
    ok = ok && app_state_init();
    ok = ok && feedback_init();
    ok = ok && uart_link_init(&hooks);
    if (!ok) {
        fatal_halt();
    }
    if (!joystick_init()) {
        PRINTF("ADC0 calibration failed; joystick readings may be offset\r\n");
    }
    ultrasonic_init();
    encoder_init();
    buttons_init();

    ok = feedback_start() && uart_link_start() && joystick_start() && ultrasonic_start() && debug_start();
#if !APP_DRIVER_TEST
    ok = ok && game_task_start(); /* in driver-test mode the debug task consumes the input queue instead */
#endif
#if APP_LOOPBACK_TEST
    ok = ok && (xTaskCreate(loopback_task, "loopback", APP_STACK_LOOPBACK, NULL, APP_PRIO_LOOPBACK, NULL) == pdPASS);
#endif
    if (!ok) {
        fatal_halt();
    }

    vTaskStartScheduler();
    fatal_halt(); /* only reached if the idle task could not be created */
    return 0;
}
