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
#include "puzzle_logic.h"
#include "schedule.h"
#include "text_logic.h"
#include "../shared/proto_link.h"
#include "../shared/protocol.h"

/* The puzzle pair of the last BOMB_CONFIG the MCX ACKed: what the next (or the running) bomb
 * asks for. The display and the web page show its texts; the answers stay on the MCX side. */
struct ActivePuzzle {
    bool valid;
    uint8_t pair;                       /* index in the puzzle set */
    uint32_t set_gen;                   /* which generation of the set it came from */
    uint8_t dial_code[PROTO_DIAL_DIGITS];
    uint8_t wire_count;
    uint8_t wire_k;                     /* 1-based from the left */
    uint8_t dial_source;                /* PuzzleSource */
    uint8_t wire_source;
    char dial_text[PUZZLE_TEXT_BUF];
    char wire_text[PUZZLE_WIRE_TEXT_MAX + 1];
};

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
    char ip[APP_IP_TEXT_LEN];     /* "192.168.1.23" while the station has an address, "" otherwise (web task writes) */
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

    /* BOMB_CONFIG results reported by the link task (+1 per ACKed / failed config) */
    uint32_t cfg_acks;
    uint32_t cfg_fails;
    ActivePuzzle active;

    /* DEFUSED-screen texts (network task writes, the display picks by date, see brief_pick) */
    BriefText brief;              /* the LLM (or fallback) briefing + coach stored for one day */
    BriefText brief_today;        /* deterministic template built from today's timetable + history */

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
/* The station's IPv4 address as text, "" when there is none (truncated to APP_IP_TEXT_LEN - 1). */
void app_state_set_ip(const char *ip);
void app_state_set_time_valid(bool valid);
void app_state_set_alarm(uint32_t alarm_unix);
/* The scheduler's answer: the alarm and the class it is for. alarm_unix = 0 / lesson NULL = no class. */
void app_state_set_schedule(uint32_t alarm_unix, uint32_t class_start_unix, const Lesson *lesson);
void app_state_set_timetable_info(uint8_t lessons, uint32_t fetched_unix);
void app_state_set_mirror_pending(uint8_t pending);
void app_state_set_bomb_config(uint8_t wires, uint8_t trip_min, uint8_t trip_max, uint8_t max_strikes);
/* Link task: one BOMB_CONFIG finished (ACKed or dropped after the retries). */
void app_state_note_config_result(bool acked);
/* Link task: the pair the MCX now holds (the MCX ACKed that BOMB_CONFIG). */
void app_state_set_active_puzzle(const ActivePuzzle &p);
/* Network task: the stored briefing/coach and today's template. */
void app_state_set_brief(const BriefText &b);
void app_state_set_brief_today(const BriefText &b);

/* Is the MCX in a state where it ignores SET_ALARM and where a demo ring would be refused
 * (BOMB_ACTIVE, DEFUSED hold, EXPLODED hold)? IDLE and ARMED are not "ringing". */
static inline bool game_ringing(const AppState &st)
{
    return st.game_valid && st.game.state != PROTO_STATE_IDLE && st.game.state != PROTO_STATE_ARMED;
}

uint32_t app_state_lock_timeouts(void);

#endif /* APP_STATE_H */
