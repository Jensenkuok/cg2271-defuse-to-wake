/* Defuse to Wake - game task. See game_task.h. */
#include "game_task.h"

#include "queue.h"

#include "app_config.h"
#include "app_state.h"
#include "debug.h"
#include "feedback.h"
#include "game.h"
#include "input_queue.h"
#include "protocol.h"
#include "uart_link.h"

/* Stack = the game_t that lives on it + a fixed margin for the engine's call chain. */
#define GAME_STACK_WORDS (APP_STACK_GAME_MARGIN_WORDS + ((sizeof(game_t) + 3u) / 4u))

static TaskHandle_t s_task;
static game_task_stats_t s_stats; /* single writer (this task); readers use a critical section */

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void apply_ctrl(game_t *g, const app_ctrl_t *c)
{
    bool ok = false;

    switch (c->kind) {
    case APP_CTRL_TIME_SYNC:
        ok = game_set_time(g, c->u.unix_s, now_ms());
        break;
    case APP_CTRL_SET_ALARM:
        ok = game_set_alarm(g, c->u.alarm_unix);
        break;
    case APP_CTRL_BOMB_CONFIG:
        ok = game_set_config(g, &c->u.cfg); /* range-checks every ESP32-sent field */
        break;
    default:
        return;
    }
    if (ok) {
        s_stats.accepted[c->kind]++;
    } else {
        s_stats.rejected[c->kind]++;
    }
}

/* Publish the snapshot when the engine says the view changed. */
static void publish(const game_t *g, uint32_t now, uint32_t *last_version, bool force)
{
    uint32_t version = game_view_version(g);
    app_state_t s;

    if (!force && version == *last_version) {
        return;
    }
    *last_version = version;
    game_get_view(g, &s.view, now);
    s.version = version;
    s.tick_ms = game_tick_interval_ms(g, now);
    s.led = game_led_mode(g);
    s.module_total = g->module_total; /* read-only peek for the bench display */
    app_state_write(&s);
    debug_notify_state_changed();
}

static void game_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(APP_GAME_TICK_MS);
    game_t g;
    game_event_t ev;
    app_ctrl_t ctrl;
    game_sound_t snd;
    proto_result_msg_t res;
    uint8_t pl[PROTO_SIZE_RESULT];
    uint32_t last_version = 0;
    uint16_t last_tick_ms = 0;
    game_led_t last_led = GAME_LED_OFF;
    TickType_t next;
    TickType_t now_t;
    TickType_t wait;
    uint32_t now;
    uint16_t tick_ms;
    game_led_t led;

    (void)arg;
    game_init(&g, now_ms());
    next = xTaskGetTickCount() + period;
    publish(&g, now_ms(), &last_version, true);

    for (;;) {
        /* Sleep until an input arrives or the next tick boundary, whichever is first.
         * This is the task's only blocking point (queue wait with a timeout). */
        now_t = xTaskGetTickCount();
        wait = ((int32_t)(next - now_t) > 0) ? (TickType_t)(next - now_t) : 0u;
        if (xQueueReceive(input_queue_handle(), &ev, wait) == pdTRUE) {
            game_handle_event(&g, &ev, now_ms());
        }

        /* Control messages from the ESP32; rare, so draining every iteration is cheap. */
        while (app_ctrl_receive(&ctrl)) {
            apply_ctrl(&g, &ctrl);
        }

        /* Fixed-rate tick, vTaskDelayUntil style but wraparound-safe and event-driven. */
        now_t = xTaskGetTickCount();
        if ((int32_t)(now_t - next) >= 0) {
            game_tick(&g, now_ms());
            next += period;
            if ((int32_t)(now_t - next) >= (int32_t)(APP_GAME_CATCHUP_TICKS * period)) {
                next = now_t + period; /* far behind: resync rather than burst ticks */
                s_stats.tick_overruns++;
            }
        }

        now = now_ms();
        publish(&g, now, &last_version, false);

        /* Buzzer tick period and LED mode: only on change, to keep the feedback queue quiet. */
        tick_ms = game_tick_interval_ms(&g, now);
        if (tick_ms != last_tick_ms) {
            last_tick_ms = tick_ms;
            feedback_set_tick_interval(tick_ms);
        }
        led = game_led_mode(&g);
        if (led != last_led) {
            last_led = led;
            feedback_set_led(led);
        }

        while (game_pop_sound(&g, &snd)) {
            feedback_play(snd);
        }

        /* RESULT is reliable: the link layer retransmits it until the ESP32 ACKs. */
        while (game_pop_result(&g, &res)) {
            if (proto_pack_result(&res, pl, sizeof(pl)) == 0u ||
                !uart_link_send(PROTO_MSG_RESULT, pl, PROTO_SIZE_RESULT)) {
                s_stats.result_send_drops++;
            }
        }
    }
}

bool game_task_start(void)
{
    return xTaskCreate(game_task, "game", (configSTACK_DEPTH_TYPE)GAME_STACK_WORDS, NULL, APP_PRIO_GAME, &s_task) == pdPASS;
}

TaskHandle_t game_task_handle(void)
{
    return s_task;
}

void game_task_get_stats(game_task_stats_t *out)
{
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}
