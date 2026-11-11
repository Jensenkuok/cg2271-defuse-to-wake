/*
 * Defuse to Wake - which puzzle the MCX really checks (pure logic, host-tested by tools/test_cfg_sync.cpp).
 *
 * Why this exists: the transport ACK only proves a BOMB_CONFIG's bytes arrived; the MCX game engine may still
 * refuse the config, and an ESP32 reset forgets which pair the MCX holds. So the ESP32 draws puzzles ONLY from
 * the BOMB_REPORTs the MCX sends back (what it holds for the next bomb = HELD, what the running bomb checks =
 * RUNNING). The riddle TEXT is the one thing a report cannot carry (17 bytes of config, no prose), so every
 * BOMB_CONFIG the ESP32 sends is remembered in a small ring together with the text it was built from; a report
 * is matched to a ring entry by cfg_id AND the packed config bytes. No match (ESP32 reset, ring overwritten,
 * a different id's bytes) means "unknown pair": the screen then uses the built-in clue for the report's code,
 * which is still correct, only less fancy. This file holds no RTOS or Arduino calls so the matching rules can
 * be tested on the PC.
 */
#ifndef CFG_SYNC_H
#define CFG_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "puzzle_logic.h"
#include "../shared/protocol.h"

/* The puzzle pair a BOMB_CONFIG was built from: what the display and the web page show as text. The answers
 * themselves stay on the MCX side; the report's config carries what the screen must check against. */
struct ActivePuzzle {
    bool valid;
    uint8_t pair;                       /* index in the puzzle set */
    uint32_t set_gen;                   /* which generation of the set it came from */
    uint8_t dial_code[PROTO_DIAL_DIGITS]; /* the code the player ENTERS for the AI dial (the puzzle's answer), not the base sent */
    uint8_t dial_pos;                   /* index of the DIAL in the sent module order; the base sent is
                                         * dial_code_preshift(dial_code, dial_pos), so game_dial_code(base, dial_pos) == dial_code */
    uint8_t wire_count;
    uint8_t wire_k;                     /* 1-based from the left */
    uint8_t dial_source;                /* PuzzleSource */
    uint8_t wire_source;
    char dial_text[PUZZLE_TEXT_BUF];
    char wire_text[PUZZLE_WIRE_TEXT_MAX + 1];
};

/* One BOMB_CONFIG the ESP32 sent: its id, the packed bytes (what the MCX will report back if it holds it) and the
 * view of the pair it was built from. */
struct CfgSend {
    bool valid;
    uint8_t cfg_id;
    uint8_t packed[PROTO_SIZE_BOMB_CONFIG];
    ActivePuzzle view;
};

/* The last APP_CFG_RING_LEN sends, newest overwrites the oldest. */
struct CfgRing {
    CfgSend entry[APP_CFG_RING_LEN];
    uint8_t next;                       /* slot the next add writes */
};

/* The latest BOMB_REPORT of one slot (HELD or RUNNING), as kept in AppState. */
struct CfgReport {
    bool valid;
    uint8_t status;                     /* PROTO_REPORT_OK / PROTO_REPORT_REJECTED */
    proto_bomb_config_t cfg;            /* what the MCX holds / runs, incl. cfg_id */
    bool matched;                       /* a ring entry has this id AND these bytes: view is its text */
    ActivePuzzle view;                  /* only meaningful when matched */
};

/* Next config id after prev: 1..255, wraps from 255 to 1 and never gives 0 (0 = the MCX's built-in default). */
uint8_t cfg_next_id(uint8_t prev);

void cfg_ring_clear(CfgRing *ring);
void cfg_ring_add(CfgRing *ring, uint8_t cfg_id, const uint8_t packed[PROTO_SIZE_BOMB_CONFIG], const ActivePuzzle &view);
/* The entry whose id AND packed bytes both equal the arguments, else NULL (the same id with other bytes is
 * NOT a match: it is a config the ESP32 never sent). */
const CfgSend *cfg_ring_find(const CfgRing &ring, uint8_t cfg_id, const uint8_t packed[PROTO_SIZE_BOMB_CONFIG]);

/* Fill out from a report: copies status and config, repacks the config and looks it up in the ring. */
void cfg_report_fill(CfgReport *out, const CfgRing &ring, const proto_bomb_report_t &rep);

/* Which report the screen follows: the RUNNING one while a bomb is shown (BOMB_ACTIVE / DEFUSED / EXPLODED) and
 * it is known, else the HELD one; NULL when neither has arrived yet (the caller then uses the built-in default). */
const CfgReport *cfg_pick_report(bool bomb_shown, const CfgReport &held, const CfgReport &running);

/* What the display and the web page draw for the module at module_idx. Pointers point into the report that was
 * passed in, so it must outlive the struct. NULL text = use the built-in clue (dial_template / wire_template). */
struct CfgFacts {
    bool from_report;                   /* false: the report is not known yet, GAME_DEFAULT_* values */
    uint8_t dial_code[PROTO_DIAL_DIGITS]; /* the code the MCX expects at module_idx (game_dial_code) */
    const char *dial_text;              /* the matched riddle, only when this dial IS the pair's own dial module */
    uint8_t wire_count;                 /* 3..6 */
    uint8_t wire_k;                     /* 1-based from the left, 1..wire_count */
    const char *wire_text;              /* the matched wire riddle */
    uint8_t trip_min_cm;
    uint8_t trip_max_cm;
    uint8_t max_strikes;
};

/* rep NULL -> the default bomb (game_config.h). Never fails: out-of-range report values are clamped. */
void cfg_facts(const CfgReport *rep, uint8_t module_idx, CfgFacts *out);

#endif /* CFG_SYNC_H */
