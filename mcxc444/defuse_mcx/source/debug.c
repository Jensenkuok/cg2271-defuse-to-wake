/* Defuse to Wake - debug / stats task. See debug.h. */
#include "debug.h"

#include <stdint.h>

#include "fsl_debug_console.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_config.h"
#include "app_state.h"
#include "encoder.h"
#include "feedback.h"
#include "game_task.h"
#include "input_queue.h"
#include "joystick.h"
#include "uart_link.h"
#include "ultrasonic.h"

#if APP_DRIVER_TEST || APP_DEBUG_STATS || APP_GAME_DEBUG

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
    game_task_stats_t gs;
    uint32_t i;
    static const char *const ctrl_names[APP_CTRL_KIND_COUNT] = { "time_sync", "set_alarm", "bomb_config" };

    uart_link_get_stats(&st);
    PRINTF("[link] tx=%u rx=%u retx=%u fail=%u crc=%u len=%u dup=%u nack=%u uart_err=%u rxq_ovf=%u\r\n",
           (unsigned)st.link.tx_frames, (unsigned)st.link.rx_frames, (unsigned)st.link.retransmits,
           (unsigned)st.link.send_failures, (unsigned)st.link.crc_errors, (unsigned)st.link.len_errors,
           (unsigned)st.link.dup_drops, (unsigned)st.link.nacks_sent, (unsigned)st.uart_errors,
           (unsigned)st.rx_queue_overflows);
    PRINTF("[comm] bad_payload=%u ignored=%u game_state_sent=%u telemetry_sent=%u\r\n",
           (unsigned)st.rx_bad_payload, (unsigned)st.rx_ignored, (unsigned)st.game_state_sent,
           (unsigned)st.telemetry_sent);
    game_task_get_stats(&gs);
    PRINTF("[drops] input_q=%u feedback_q=%u ctrl_q=%u tx_req=%u tx_ring=%u result=%u state_lock=%u\r\n",
           (unsigned)input_queue_drops(), (unsigned)feedback_drops(), (unsigned)st.ctrl_drops,
           (unsigned)st.tx_req_drops, (unsigned)st.tx_ring_drops, (unsigned)gs.result_send_drops,
           (unsigned)app_state_lock_timeouts());
    PRINTF("[ctrl] accepted/rejected:");
    for (i = 0; i < APP_CTRL_KIND_COUNT; i++) {
        PRINTF(" %s=%u/%u", ctrl_names[i], (unsigned)gs.accepted[i], (unsigned)gs.rejected[i]);
    }
    PRINTF(" tick_overruns=%u us_stale=%u\r\n", (unsigned)gs.tick_overruns, (unsigned)ultrasonic_stale());
    PRINTF("[stack words free]");
    print_stack("game", game_task_handle());
    print_stack("joy", joystick_task_handle());
    print_stack("comm", uart_link_task_handle());
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
#if APP_GAME_DEBUG
static const char *const k_state_names[] = { "IDLE", "ARMED", "BOMB_ACTIVE", "DEFUSED", "EXPLODED" };
static const char *const k_module_names[] = { "-", "DIAL", "WIRES", "TRIPWIRE" };
#define NAME_OR(arr, i) (((i) < (sizeof(arr) / sizeof((arr)[0]))) ? (arr)[i] : "?")

/* Decode module_value (docs/game.md) into the part a human wants to see. */
static void print_module_value(uint8_t module_type, int16_t v)
{
    uint32_t lo = (uint32_t)v & 0xFFu;
    uint32_t hi = ((uint32_t)v >> 8) & 0xFFu;
    uint32_t i;

    switch (module_type) {
    case PROTO_MODULE_DIAL:
        PRINTF(" dial digit=%u val=%u", (unsigned)hi, (unsigned)lo);
        break;
    case PROTO_MODULE_WIRES:
        PRINTF(" wires cursor=%u cut=0b", (unsigned)lo);
        for (i = GAME_DEBUG_MASK_BITS; i > 0u; i--) {
            PRINTF("%u", (unsigned)((hi >> (i - 1u)) & 1u));
        }
        break;
    case PROTO_MODULE_TRIPWIRE:
        PRINTF(" trip dist=%ucm hold=%u%%", (unsigned)lo, (unsigned)hi);
        break;
    default:
        break;
    }
}

/* One line per snapshot change: this is the stand-in for the ESP32 OLED on the bench. */
static void print_game_line(void)
{
    static bool s_have;
    static uint32_t s_last_version;
    app_state_t st;

    if (!app_state_read(&st) || (s_have && st.version == s_last_version)) {
        return;
    }
    s_have = true;
    s_last_version = st.version;
    PRINTF("[game] %s", NAME_OR(k_state_names, st.view.state));
    if (st.module_total != 0u && st.view.state >= PROTO_STATE_BOMB_ACTIVE) {
        PRINTF(" %s mod %u/%u t=%us strikes=%u prog=%u%%", NAME_OR(k_module_names, st.view.module_type),
               (unsigned)(st.view.module_idx + 1u), (unsigned)st.module_total, (unsigned)st.view.time_left_s,
               (unsigned)st.view.strikes, (unsigned)st.view.progress_pct);
    }
    if (st.view.state == PROTO_STATE_BOMB_ACTIVE) {
        print_module_value(st.view.module_type, st.view.module_value);
    }
    PRINTF("\r\n");
}
#endif /* APP_GAME_DEBUG */

/* Blocks on a task notification from the game task (snapshot changed) with a timeout
 * equal to the time until the next stats print, so it never polls or spins. */
static void debug_task(void *arg)
{
    TickType_t next = xTaskGetTickCount() + pdMS_TO_TICKS(APP_DEBUG_STATS_MS);
    TickType_t now;
    TickType_t wait;

    (void)arg;
    for (;;) {
        now = xTaskGetTickCount();
        wait = ((int32_t)(next - now) > 0) ? (TickType_t)(next - now) : 0u;
        if (!APP_DEBUG_STATS) {
            wait = portMAX_DELAY;
        }
        if (ulTaskNotifyTake(pdTRUE, wait) != 0u) {
#if APP_GAME_DEBUG
            print_game_line();
#endif
        }
#if APP_DEBUG_STATS
        if ((int32_t)(xTaskGetTickCount() - next) >= 0) {
            next += pdMS_TO_TICKS(APP_DEBUG_STATS_MS);
            print_stats();
        }
#endif
    }
}
#endif /* APP_DRIVER_TEST */

bool debug_start(void)
{
    return xTaskCreate(debug_task, "debug", APP_STACK_DEBUG, NULL, APP_PRIO_DEBUG, &s_task) == pdPASS;
}

void debug_notify_state_changed(void)
{
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task); /* cheap and non-blocking */
    }
}

#else /* neither test mode nor stats: no debug task */

bool debug_start(void)
{
    return true;
}

void debug_notify_state_changed(void)
{
}

#endif
