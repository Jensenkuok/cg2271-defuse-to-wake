/*
 * Defuse to Wake - OLED display task (priority 2, 10 fps via vTaskDelayUntil).
 * Renders a COPY of the app state. The rendering is split in two so the same logic
 * runs with or without a screen:
 *   build_model()  state -> ScreenModel (pure, no I/O)
 *   layout_text()  ScreenModel -> the size-1 text rows (21 x 8 characters) of the screen
 *   draw_oled()    ScreenModel + text rows -> SSD1306 framebuffer
 *   print_summary() ScreenModel + the same text rows -> one Serial line, at most once a second
 * If the OLED does not ACK at start-up the task logs once and runs headless. The task never waits
 * for the network or for flash: all texts (puzzle, briefing, coach) are already in the app state.
 */
#ifndef DISPLAY_TASK_H
#define DISPLAY_TASK_H

#include <Arduino.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

enum ScreenId : uint8_t { SCR_IDLE = 0, SCR_ARMED, SCR_ACTIVE, SCR_DEFUSED, SCR_EXPLODED };

/* Everything the renderers need, already decoded from module_value (docs/game.md). */
struct ScreenModel {
    ScreenId screen;
    bool link_down;
    bool wifi_up;
    bool time_valid;
    bool colon_on;            /* IDLE clock colon blink */
    bool invert;              /* EXPLODED flash */
    char clock_text[8];       /* IDLE "HH:MM" or "--:--" */
    char next_text[APP_CLUE_MAX_CHARS + 1]; /* IDLE bottom line, e.g. "Fri 15:00 CG2271 LT19", "" if none */
    int32_t ring_in_s;        /* ARMED: seconds until the alarm, -1 unknown */
    char timer_text[8];       /* ACTIVE "MM:SS" */
    uint8_t strikes;
    uint8_t max_strikes;
    uint8_t module_type;      /* PROTO_MODULE_* */
    /* DIAL */
    uint8_t dial_digit;       /* index of the digit being entered (0..3) */
    uint8_t dial_value;
    /* WIRES */
    uint8_t wire_count;
    uint8_t wire_cursor;
    uint8_t wire_cut_mask;
    /* TRIPWIRE */
    uint8_t trip_cm;          /* min(distance, 255) */
    uint8_t trip_hold_pct;
    uint8_t trip_min_cm;
    uint8_t trip_max_cm;
    char taken_text[8];       /* DEFUSED "MM:SS" */
    char clue[APP_CLUE_MAX_CHARS + 1]; /* TRIPWIRE bottom line (placeholder, unchanged) */
    /* ACTIVE DIAL / WIRES: the riddle of the ACTIVE pair (the config the MCX ACKed) and the hint */
    char puzzle_text[PUZZLE_TEXT_BUF];
    char hint[HINT_BUF];      /* "" until the current bomb has a strike */
    int8_t hint_wire;         /* WIRES: 0-based wire the hint marks with an X, -1 none */
    /* DEFUSED pages (rotate every DEFUSED_PAGE_MS) */
    uint8_t defused_page;     /* 0 result line, 1 briefing, 2 coach */
    char result_text[RESULT_LINE_MAX];
    char brief_text[BRIEF_BRIEFING_MAX + 1];
    char coach_text[BRIEF_COACH_MAX + 1];
};

/* Pure: no I/O, no globals. now_ms drives the blink/flash phases, unix_now the clock, defused_ms
 * the time spent on the DEFUSED screen (page rotation). */
void build_model(const AppState &st, uint32_t now_ms, uint32_t unix_now, uint32_t defused_ms, ScreenModel *out);

bool display_task_start(void);

TaskHandle_t display_task_handle(void);

#endif /* DISPLAY_TASK_H */
