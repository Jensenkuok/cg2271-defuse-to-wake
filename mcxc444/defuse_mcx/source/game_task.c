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

/* BOMB_REPORTs the ESP32 draws its puzzle from. One pending slot per report kind: a newer
 * report replaces an older unsent one of the same slot (the old one is stale by definition). */
typedef struct {
    bool pending;
    uint8_t status;
    proto_bomb_config_t cfg;
} report_slot_t;

enum { REPORT_SLOT_HELD = 0, REPORT_SLOT_RUNNING, REPORT_SLOT_COUNT };

static report_slot_t s_reports[REPORT_SLOT_COUNT];

static void queue_report(uint32_t slot, uint8_t status, const proto_bomb_config_t *cfg)
{
    s_reports[slot].pending = true;
    s_reports[slot].status = status;
    s_reports[slot].cfg = *cfg;
}

static void queue_held(const game_t *g, uint8_t status)
{
    queue_report(REPORT_SLOT_HELD, status, game_held_config(g));
}

static void queue_running(const proto_bomb_config_t *cfg)
{
    queue_report(REPORT_SLOT_RUNNING, PROTO_REPORT_OK, cfg);
}

/* Send what is pending, HELD before RUNNING. uart_link_send fails when the TX FIFO is full;
 * the slot then stays pending and the next loop iteration retries (the loop runs every
 * APP_GAME_TICK_MS), so a report is never dropped silently. */
static void flush_reports(void)
{
    static const uint8_t proto_slot[REPORT_SLOT_COUNT] = { PROTO_REPORT_HELD, PROTO_REPORT_RUNNING };
    proto_bomb_report_t rep;
    uint8_t pl[PROTO_SIZE_BOMB_REPORT];
    uint32_t i;

    for (i = 0; i < REPORT_SLOT_COUNT; i++) {
        if (!s_reports[i].pending) {
            continue;
        }
        rep.slot = proto_slot[i];
        rep.status = s_reports[i].status;
        rep.cfg = s_reports[i].cfg;
        if (proto_pack_bomb_report(&rep, pl, sizeof(pl)) == 0u) {
            s_reports[i].pending = false; /* cannot happen for our own values; do not spin on it */
        } else if (uart_link_send(PROTO_MSG_BOMB_REPORT, pl, PROTO_SIZE_BOMB_REPORT)) {
            s_reports[i].pending = false;
            s_stats.reports_sent++;
        } else {
            s_stats.report_retries++;
            return; /* keep the order: do not let RUNNING overtake a stuck HELD */
        }
    }
}

static void apply_ctrl(game_t *g, const app_ctrl_t *c)
{
    bool ok = false;
    const proto_bomb_config_t *run;

    switch (c->kind) {
    case APP_CTRL_TIME_SYNC:
        ok = game_set_time(g, c->u.unix_s, now_ms());
        break;
    case APP_CTRL_SET_ALARM:
        ok = game_set_alarm(g, c->u.alarm_unix);
        break;
    case APP_CTRL_BOMB_CONFIG:
        ok = game_set_config(g, &c->u.cfg); /* range-checks every ESP32-sent field */
        /* Application-level answer (the transport ACK only says the frame arrived): what the
         * game holds after the attempt, so the ESP32 never assumes a rejected config. */
        queue_held(g, ok ? PROTO_REPORT_OK : PROTO_REPORT_REJECTED);
        break;
    case APP_CTRL_REPORT:
        queue_held(g, PROTO_REPORT_OK);
        run = game_running_config(g);
        if (run != NULL) {
            queue_running(run);
        }
        ok = true;
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

/* Priority 4, the highest: an input preempts every other task and is handled at once.
 * Its only blocking point is the input queue, with a timeout up to the next 50 ms tick. */
static void game_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(APP_GAME_TICK_MS);
    game_t g;
    game_event_t ev;
    app_ctrl_t ctrl;
    game_sound_t snd;
    proto_result_msg_t res;
    proto_bomb_config_t started;
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

        /* A new bomb started: tell the ESP32 which config it checks (RUNNING report). */
        while (game_pop_bomb_started(&g, &started)) {
            queue_running(&started);
        }
        flush_reports();

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
