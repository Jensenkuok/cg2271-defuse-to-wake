/* Defuse to Wake - debug / stats task. See debug.h. */
#include "debug.h"

#include <stdint.h>

#include "fsl_debug_console.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_config.h"
#include "encoder.h"
#include "feedback.h"
#include "input_queue.h"
#include "joystick.h"
#include "uart_link.h"
#include "ultrasonic.h"

#if APP_DRIVER_TEST || APP_DEBUG_STATS

static TaskHandle_t s_task;

static void print_stack(const char *name, TaskHandle_t t)
{
    if (t != NULL) {
        PRINTF(" %s=%u", name, (unsigned)uxTaskGetStackHighWaterMark(t)); /* words left */
    }
}

static void print_stats(void)
{
    uart_link_stats_t st;

    uart_link_get_stats(&st);
    PRINTF("[link] tx=%u rx=%u retx=%u fail=%u crc=%u len=%u dup=%u nack=%u uart_err=%u rxq_ovf=%u\r\n",
           (unsigned)st.link.tx_frames, (unsigned)st.link.rx_frames, (unsigned)st.link.retransmits,
           (unsigned)st.link.send_failures, (unsigned)st.link.crc_errors, (unsigned)st.link.len_errors,
           (unsigned)st.link.dup_drops, (unsigned)st.link.nacks_sent, (unsigned)st.uart_errors,
           (unsigned)st.rx_queue_overflows);
    PRINTF("[stack words free]");
    print_stack("comm", uart_link_task_handle());
    print_stack("joy", joystick_task_handle());
    print_stack("us", ultrasonic_task_handle());
    print_stack("fb", feedback_task_handle());
    print_stack("dbg", s_task);
    print_stack("idle", xTaskGetIdleTaskHandle());
    PRINTF("  heap_free=%u\r\n", (unsigned)xPortGetFreeHeapSize());
}

#if APP_DRIVER_TEST
static const char *event_name(game_event_type_t t)
{
    switch (t) {
    case GAME_EVT_ENC_STEP:       return "ENC_STEP";
    case GAME_EVT_ENC_PRESS:      return "ENC_PRESS";
    case GAME_EVT_ENC_LONG_PRESS: return "ENC_LONG_PRESS";
    case GAME_EVT_JOY_LEFT:       return "JOY_LEFT";
    case GAME_EVT_JOY_RIGHT:      return "JOY_RIGHT";
    case GAME_EVT_JOY_PRESS:      return "JOY_PRESS";
    case GAME_EVT_DISTANCE:       return "DISTANCE";
    case GAME_EVT_TAMPER:         return "TAMPER";
    case GAME_EVT_CMD_RING_NOW:   return "CMD_RING_NOW";
    case GAME_EVT_CMD_SNOOZE:     return "CMD_SNOOZE";
    case GAME_EVT_CMD_CANCEL:     return "CMD_CANCEL";
    default:                      return "?";
    }
}

/* Boot demo: one sound and one LED mode per step, 2 s each. */
static void boot_demo(void)
{
    static const game_led_t leds[] = {
        GAME_LED_OFF, GAME_LED_ARMED_DIM_BLUE, GAME_LED_ACTIVE_RED_BLINK,
        GAME_LED_DEFUSED_GREEN, GAME_LED_EXPLODED_RED_WHITE
    };
    static const char *const led_names[] = {
        "OFF", "ARMED_DIM_BLUE", "ACTIVE_RED_BLINK", "DEFUSED_GREEN", "EXPLODED_RED_WHITE"
    };
    static const char *const sound_names[] = {
        "CLICK", "DIGIT_OK", "STRIKE", "MODULE_CLEARED", "DEFUSED", "EXPLODED", "DENIED"
    };
    const uint32_t n_leds = sizeof(leds) / sizeof(leds[0]);
    const uint32_t n_sounds = sizeof(sound_names) / sizeof(sound_names[0]);
    uint32_t i;

    for (i = 0; i < n_sounds || i < n_leds; i++) {
        if (i < n_sounds) {
            feedback_play((game_sound_t)i);
            PRINTF("[demo] sound %s", sound_names[i]);
        }
        if (i < n_leds) {
            feedback_set_led(leds[i]);
            PRINTF("%s led %s", (i < n_sounds) ? "," : "[demo]", led_names[i]);
        }
        PRINTF("\r\n");
        vTaskDelay(pdMS_TO_TICKS(APP_DEBUG_DEMO_STEP_MS));
    }
    feedback_set_led(GAME_LED_OFF);
    PRINTF("[demo] done, printing input events\r\n");
}

static void debug_task(void *arg)
{
    TickType_t next = xTaskGetTickCount() + pdMS_TO_TICKS(APP_DEBUG_TEST_STATS_MS);
    TickType_t now;
    TickType_t wait;
    game_event_t ev;
    uint16_t x;
    uint16_t y;

    (void)arg;
    boot_demo();
    for (;;) {
        /* Block on the input queue until the next stats print is due. */
        now = xTaskGetTickCount();
        wait = ((int32_t)(next - now) > 0) ? (TickType_t)(next - now) : 0u;
        if (xQueueReceive(input_queue_handle(), &ev, wait) == pdTRUE) {
            PRINTF("[evt] %s %d\r\n", event_name(ev.type), (int)ev.value);
            continue;
        }
        next += pdMS_TO_TICKS(APP_DEBUG_TEST_STATS_MS);
        joystick_get_raw(&x, &y);
        PRINTF("[io] joy_x=%u joy_y=%u dist_cm=%d enc=%d drops=%u adc_err=%u us_timeouts=%u fb_drops=%u\r\n",
               (unsigned)x, (unsigned)y, (int)ultrasonic_last_cm(), (int)encoder_count(),
               (unsigned)input_queue_drops(), (unsigned)joystick_errors(),
               (unsigned)ultrasonic_timeouts(), (unsigned)feedback_drops());
        print_stats();
    }
}
#else
static void debug_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();

    (void)arg;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_DEBUG_STATS_MS));
        print_stats();
    }
}
#endif /* APP_DRIVER_TEST */

bool debug_start(void)
{
    return xTaskCreate(debug_task, "debug", APP_STACK_DEBUG, NULL, APP_PRIO_DEBUG, &s_task) == pdPASS;
}

#else /* neither test mode nor stats: no debug task */

bool debug_start(void)
{
    return true;
}

#endif
