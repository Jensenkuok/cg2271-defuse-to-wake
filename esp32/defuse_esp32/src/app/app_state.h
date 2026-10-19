/*
 * Defuse to Wake - state shared between the ESP32 tasks.
 * One struct guarded by a FreeRTOS mutex (priority inheritance, bounded wait). Writers
 * are the link, network and display-independent tasks through the setters below;
 * readers take a COPY with app_state_read() and work on that.
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "schedule.h"
#include "../shared/proto_link.h"
#include "../shared/protocol.h"

struct AppState {
    /* from the MCX */
    proto_game_state_t game;
    bool game_valid;
    uint32_t game_version;        /* ESP32-side counter: +1 per GAME_STATE received */
    bool session_active;          /* first BOMB_ACTIVE seen, MCX not back in IDLE yet (session_next) */
    uint32_t session_starts;      /* +1 per session start */
    uint32_t session_ends;        /* +1 per session end (IDLE reached) */
    proto_telemetry_t telem;
    bool telem_valid;
    proto_result_msg_t last_result;
    bool result_valid;

    /* link */
    bool link_up;
    uint32_t link_up_count;       /* +1 per down->up transition */
    proto_link_stats_t link_stats;
    uint32_t tx_backpressure;     /* times the TX queue head could not enter proto_link */

    /* network */
    bool wifi_up;
    bool time_valid;
    uint32_t alarm_unix;          /* 0 = none */
    bool next_valid;              /* a next class exists (alarm_unix != 0) */
    uint32_t next_start_unix;     /* start of that class */
    char next_module[SCHED_MODULE_LEN];
    char next_type[SCHED_ABBREV_LEN];
    char next_venue[SCHED_VENUE_LEN];
    uint8_t tt_lessons;           /* lessons in the timetable cache */
    uint32_t tt_fetched_unix;     /* last successful NUSMods fetch, 0 = never */
    uint8_t mirror_pending;       /* morning records waiting for Firebase */

    /* current bomb as the display knows it (default until BOMB_CONFIG is tracked) */
    uint8_t wire_count;
    uint8_t trip_min_cm;
    uint8_t trip_max_cm;
    uint8_t max_strikes;
};

bool app_state_init(void);

/* Copy of the whole state. False if the mutex could not be taken in time (out untouched). */
bool app_state_read(AppState *out);

void app_state_set_game(const proto_game_state_t &g);
void app_state_set_telemetry(const proto_telemetry_t &t);
void app_state_set_result(const proto_result_msg_t &r);
void app_state_set_link(bool up);
void app_state_set_link_stats(const proto_link_stats_t &s, uint32_t tx_backpressure);
void app_state_set_wifi(bool up);
void app_state_set_time_valid(bool valid);
void app_state_set_alarm(uint32_t alarm_unix);
/* The scheduler's answer: the alarm and the class it is for. alarm_unix = 0 / lesson NULL = no class. */
void app_state_set_schedule(uint32_t alarm_unix, uint32_t class_start_unix, const Lesson *lesson);
void app_state_set_timetable_info(uint8_t lessons, uint32_t fetched_unix);
void app_state_set_mirror_pending(uint8_t pending);
void app_state_set_bomb_config(uint8_t wires, uint8_t trip_min, uint8_t trip_max, uint8_t max_strikes);

uint32_t app_state_lock_timeouts(void);

#endif /* APP_STATE_H */
