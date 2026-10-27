/*
 * Defuse to Wake - difficulty: one 0..100 score replayed from the morning history, the user's
 * Auto / Easy / Normal / Hard setting, and the bomb each level asks for (pure, no Arduino headers,
 * no heap, no globals; compiled by the sketch and by tools/test_difficulty.cpp, C++11).
 *
 * Rubric: "ESP32 use" (the ESP32 decides the game's difficulty and tells the MCX through BOMB_CONFIG;
 * the MCX only range-checks it). The rule is deterministic and explainable, the LLM has no say in it.
 *
 * Score (planner decision O9): start at DIFF_SCORE_START, then replay the stored mornings from the
 * oldest to the newest. Per morning
 *     fast clean defuse (defused, 0 strikes, 0 snoozes, 0 explosions, duration <= DIFF_FAST_S)  +8
 *     any other defuse                                                                            0
 *     each explosion                                                                            -10
 *     each snooze                                                                                -5
 * and the running score is clamped to 0..100 after EVERY morning (so a long good streak cannot bank
 * more than 100 and one bad morning always bites). Bands: 0-33 EASY, 34-66 NORMAL, 67-100 HARD.
 * The history ring holds only DEFUSED mornings (explosions and snoozes of a morning are summed into
 * its record), so the score needs nothing else.
 *
 * Mode: Auto uses the band of the score, a manual mode forces its level. The level then sets the
 * bomb (kLevelTable below): time, module count and the wire-count range of the wire puzzle.
 */
#ifndef DIFFICULTY_H
#define DIFFICULTY_H

#include <stddef.h>
#include <stdint.h>

#include "history_ring.h"
#include "puzzle_rules.h"       /* PrLevel: PR_EASY / PR_NORMAL / PR_HARD are the level numbers everywhere */
#include "../shared/protocol.h"

/* ---- Score (O9) ---- */
#define DIFF_SCORE_START        50
#define DIFF_SCORE_MIN          0
#define DIFF_SCORE_MAX          100
#define DIFF_FAST_S             90      /* a defuse within this many seconds counts as fast */
#define DIFF_FAST_BONUS         8
#define DIFF_EXPLOSION_PENALTY  10
#define DIFF_SNOOZE_PENALTY     5
#define DIFF_BAND_EASY_MAX      33      /* 0..33 EASY */
#define DIFF_BAND_NORMAL_MAX    66      /* 34..66 NORMAL, above: HARD */

/* ---- What each level asks for ---- */
#define DIFF_EASY_TIME_S        240
#define DIFF_NORMAL_TIME_S      180
#define DIFF_HARD_TIME_S        150
/* Every level uses all four sensor modules (dial, wires, tripwire, lights): the light module is the
 * wake-up mechanic, so no level skips it. Levels differ by time, wire count and puzzle tier. */
#define DIFF_EASY_MODULES       4
#define DIFF_NORMAL_MODULES     4
#define DIFF_HARD_MODULES       4
#define DIFF_EASY_WIRES_MIN     3
#define DIFF_EASY_WIRES_MAX     4
#define DIFF_NORMAL_WIRES_MIN   4
#define DIFF_NORMAL_WIRES_MAX   5
#define DIFF_HARD_WIRES_MIN     6
#define DIFF_HARD_WIRES_MAX     6

#define DIFF_EXPLAIN_MAX        60      /* "Normal (Auto, score 52)" fits easily */

static_assert(DIFF_HARD_MODULES <= PROTO_MAX_MODULES, "the MCX holds at most PROTO_MAX_MODULES modules");
static_assert(DIFF_SCORE_START >= DIFF_SCORE_MIN && DIFF_SCORE_START <= DIFF_SCORE_MAX, "start inside the clamp");

/* The setting on the web page ("/"), kept in Preferences. Numbers are stored in flash: do not reorder. */
enum DiffMode {
    DIFF_MODE_AUTO = 0,
    DIFF_MODE_EASY = 1,
    DIFF_MODE_NORMAL = 2,
    DIFF_MODE_HARD = 3
};
#define DIFF_MODE_DEFAULT       DIFF_MODE_AUTO
#define DIFF_MODE_COUNT         4

struct LevelSpec {
    uint16_t time_s;
    uint8_t modules;
    uint8_t wire_min;               /* wire count range of the wire puzzle at this level */
    uint8_t wire_max;
};

/* level: PR_EASY / PR_NORMAL / PR_HARD. Anything else is treated as NORMAL (the safe middle). */
static inline LevelSpec difficulty_level_spec(int level)
{
    LevelSpec s;

    switch (level) {
    case PR_EASY:
        s.time_s = DIFF_EASY_TIME_S;
        s.modules = DIFF_EASY_MODULES;
        s.wire_min = DIFF_EASY_WIRES_MIN;
        s.wire_max = DIFF_EASY_WIRES_MAX;
        break;
    case PR_HARD:
        s.time_s = DIFF_HARD_TIME_S;
        s.modules = DIFF_HARD_MODULES;
        s.wire_min = DIFF_HARD_WIRES_MIN;
        s.wire_max = DIFF_HARD_WIRES_MAX;
        break;
    default:
        s.time_s = DIFF_NORMAL_TIME_S;
        s.modules = DIFF_NORMAL_MODULES;
        s.wire_min = DIFF_NORMAL_WIRES_MIN;
        s.wire_max = DIFF_NORMAL_WIRES_MAX;
        break;
    }
    return s;
}

/* The bomb the ESP32 asks the MCX for, and why (briefing, /api/status, Serial "diff"). */
struct Difficulty {
    uint8_t module_count;           /* 4 at every level */
    uint16_t time_limit_s;          /* 150..240 */
    uint8_t level;                  /* effective level, PR_EASY..PR_HARD */
    uint8_t mode;                   /* DiffMode */
    uint8_t score;                  /* 0..100, always computed (shown even when the mode is manual) */
    char explanation[DIFF_EXPLAIN_MAX + 1];     /* "Normal (Auto, score 52)" / "Hard (manual, score 52)" */
};

/* recent: newest first (history_recent order), n records. Replays them oldest -> newest. n <= 0 or
 * recent == NULL: DIFF_SCORE_START. */
int difficulty_score(const HistRecord *recent, int n);

/* 0..33 EASY, 34..66 NORMAL, 67..100 HARD; out-of-range scores are clamped first. */
int difficulty_band_level(int score);

/* Manual mode: its level. Auto (or an unknown mode): the band of `score`. */
int difficulty_effective_level(int mode, int score);

/* "auto" "easy" "normal" "hard" (the ids used in the form and the JSON); "auto" for an unknown mode. */
const char *difficulty_mode_name(int mode);
/* Exact lowercase match of one of the four ids; false (and *mode_out untouched) otherwise. */
bool difficulty_mode_parse(const char *text, int *mode_out);
/* Is this a stored/received mode number? */
bool difficulty_mode_valid(int mode);

/* The whole answer: score, level, time and module count of that level, and the explanation. */
Difficulty difficulty(const HistRecord *recent, int n, int mode);

/* The same answer from a score that is already known (clamped to 0..100). */
Difficulty difficulty_from_score(int score, int mode);

#define DIFF_NO_SCORE           (-1)    /* "no good score seen yet" for difficulty_resolve */

/* difficulty() that survives a history lock timeout. history_ok is false when the zero records read
 * were NOT "no history" (the ring could not be locked in time): the score is then not recomputed but
 * taken from *last_good_score, the score of the last good computation, so an Auto user does not drop
 * to the score-50 NORMAL bomb with puzzles still rated for their real level. With no good score yet
 * the fallback is DIFF_SCORE_START (NORMAL in Auto; a manual mode ignores the score anyway). The
 * score is remembered, not the level, so a mode change still takes effect while the history is
 * unreadable. *last_good_score starts at DIFF_NO_SCORE and is updated on every good computation. */
Difficulty difficulty_resolve(const HistRecord *recent, int n, int mode, bool history_ok, int *last_good_score);

#endif /* DIFFICULTY_H */
