/* Defuse to Wake - shared ESP32 state. See app_state.h. */
#include "app_state.h"

#include <Arduino.h>
#include <string.h>

#include "mem_big.h"

static SemaphoreHandle_t s_mutex;
static AppState *s_state;       /* PSRAM (mem_big.h): ~1.3 KB, allocated once in app_state_init */
static uint32_t s_lock_timeouts; /* diagnostic counter; 32-bit reads/writes are atomic */

bool app_state_init(void)
{
    s_state = mem_big_new<AppState>(1u, "app state");   /* zeroed */
    if (s_state == NULL) {
        return false;
    }
    /* A mutex (not a critical section): the struct is copied by several tasks, and a
     * FreeRTOS mutex adds priority inheritance so the prio-4 link task is never stuck
     * behind a low-priority reader that holds the lock. */
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

/* Run fn with the lock held; the bounded wait keeps a stuck task from freezing others. */
template <typename Fn>
static bool with_lock(Fn fn)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_STATE_MUTEX_WAIT_MS)) != pdTRUE) {
        s_lock_timeouts = s_lock_timeouts + 1u;
        return false;
    }
    fn();
    xSemaphoreGive(s_mutex);
    return true;
}

bool app_state_read(AppState *out)
{
    return with_lock([&]() { memcpy(out, s_state, sizeof(AppState)); });
}

void app_state_set_game(const proto_game_state_t &g)
{
    with_lock([&]() {
        s_state->game = g;
        s_state->game_valid = true;
        s_state->game_version++;
        /* Session tracking lives here so it is updated in the same locked step as the state it
         * derives from; the network task only reads the flag and the two counters. */
        const bool was_active = s_state->session_active;
        s_state->session_active = session_next(was_active, g.state);
        if (s_state->session_active && !was_active) {
            s_state->session_starts++;
        } else if (!s_state->session_active && was_active) {
            s_state->session_ends++;
        }
    });
}

void app_state_set_telemetry(const proto_telemetry_t &t)
{
    with_lock([&]() { s_state->telem = t; s_state->telem_valid = true; });
}

void app_state_set_result(const proto_result_msg_t &r)
{
    with_lock([&]() { s_state->last_result = r; s_state->result_valid = true; });
}

void app_state_set_link(bool up)
{
    with_lock([&]() {
        if (up && !s_state->link_up) {
            s_state->link_up_count++;
        }
        s_state->link_up = up;
    });
}

void app_state_set_link_stats(const proto_link_stats_t &s, uint32_t tx_backpressure)
{
    with_lock([&]() { s_state->link_stats = s; s_state->tx_backpressure = tx_backpressure; });
}

void app_state_set_wifi(bool up)
{
    with_lock([&]() { s_state->wifi_up = up; });
}

void app_state_set_ip(const char *ip)
{
    with_lock([&]() { snprintf(s_state->ip, sizeof(s_state->ip), "%s", ip != NULL ? ip : ""); });
}

void app_state_set_time_valid(bool valid)
{
    with_lock([&]() { s_state->time_valid = valid; });
}

void app_state_set_alarm(uint32_t alarm_unix)
{
    with_lock([&]() { s_state->alarm_unix = alarm_unix; });
}

void app_state_set_schedule(uint32_t alarm_unix, uint32_t class_start_unix, const Lesson *lesson, bool is_demo)
{
    with_lock([&]() {
        s_state->alarm_unix = alarm_unix;
        s_state->next_valid = (alarm_unix != 0u) && (lesson != NULL);
        s_state->alarm_is_demo = is_demo && s_state->next_valid;
        s_state->next_start_unix = s_state->next_valid ? class_start_unix : 0u;
        memset(s_state->next_module, 0, sizeof(s_state->next_module));
        memset(s_state->next_type, 0, sizeof(s_state->next_type));
        memset(s_state->next_venue, 0, sizeof(s_state->next_venue));
        if (s_state->next_valid) {
            memcpy(s_state->next_module, lesson->module, sizeof(s_state->next_module) - 1u);
            memcpy(s_state->next_type, lesson->type, sizeof(s_state->next_type) - 1u);
            memcpy(s_state->next_venue, lesson->venue, sizeof(s_state->next_venue) - 1u);
        }
    });
}

void app_state_set_demo_msg(const char *msg)
{
    with_lock([&]() { snprintf(s_state->demo_msg, sizeof(s_state->demo_msg), "%s", msg != NULL ? msg : ""); });
}

void app_state_set_timetable_info(uint8_t lessons, uint32_t fetched_unix)
{
    with_lock([&]() { s_state->tt_lessons = lessons; s_state->tt_fetched_unix = fetched_unix; });
}

void app_state_set_mirror_pending(uint8_t pending)
{
    with_lock([&]() { s_state->mirror_pending = pending; });
}

void app_state_note_config_result(bool acked)
{
    with_lock([&]() {
        if (acked) {
            s_state->cfg_acks++;
        } else {
            s_state->cfg_fails++;
        }
    });
}

void app_state_set_report(uint8_t slot, const CfgReport &r)
{
    with_lock([&]() {
        if (slot == PROTO_REPORT_HELD) {
            s_state->held = r;
        } else if (slot == PROTO_REPORT_RUNNING) {
            s_state->running = r;
        }
    });
}

void app_state_note_bad_report(void)
{
    with_lock([&]() { s_state->report_bad++; });
}

void app_state_set_brief(const BriefText &b)
{
    with_lock([&]() { s_state->brief = b; });
}

void app_state_set_brief_today(const BriefText &b)
{
    with_lock([&]() { s_state->brief_today = b; });
}

uint32_t app_state_lock_timeouts(void)
{
    return s_lock_timeouts;
}
