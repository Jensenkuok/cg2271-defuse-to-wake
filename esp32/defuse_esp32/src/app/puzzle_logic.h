/*
 * Defuse to Wake - puzzle pipeline logic (pure, no Arduino headers, no heap, no globals).
 * Compiled by the sketch and by the host tests (tools/test_puzzle.cpp, C++11).
 *
 * The ANSWER of every puzzle is picked by the ESP32 (dial code / wire number); the LLM only
 * writes a riddle for it and a second model checks that the riddle really leads to that answer.
 * Whatever the LLM does, the template bank in this file is the safe fallback: its clues are
 * correct by construction (host tests solve every dial code 0000..9999 and every wire k of n).
 *
 * Contents: JSON/text helpers for model replies, puzzle validation, the template bank, the
 * difficulty rule, BOMB_CONFIG assembly and the small pair-selection rules.
 */
#ifndef PUZZLE_LOGIC_H
#define PUZZLE_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#include "history_ring.h"
#include "../game/game_config.h"
#include "../shared/protocol.h"

/* ---- Sizes ---- */
#define PUZZLE_PAIRS            4       /* dial + wire puzzles per set (pair i = dial[i] + wire[i]) */
#define PUZZLE_DIAL_TEXT_MAX    80      /* OLED budget for the dial clue */
#define PUZZLE_WIRE_TEXT_MAX    60
#define PUZZLE_TEXT_MIN         10
#define PUZZLE_TEXT_BUF         (PUZZLE_DIAL_TEXT_MAX + 1)
#define PUZZLE_MODEL_LEN        16      /* "qwen3.6:35b" + NUL */
#define PUZZLE_ANSWER_LEN       8
#define PUZZLE_EXPLAIN_MAX      60
#define PUZZLE_MAGIC            0x505A4C31u /* "PZL1": bump when PuzzleSet changes */

/* ---- Difficulty rule (CLAUDE.md "Difficulty"): deterministic, from the last mornings ---- */
#define DIFF_WINDOW             7       /* mornings looked at */
#define DIFF_BASE_MODULES       3
#define DIFF_BASE_TIME_S        180
#define DIFF_SNOOZE_LIMIT       3       /* snoozes in the window that add a module */
#define DIFF_STRIKE_AVG         2       /* average strikes per morning that adds time pressure... */
#define DIFF_TIME_STEP_S        30      /* ...as one step less/more time */
#define DIFF_TIME_MAX_S         240
#define DIFF_TIME_MIN_S         120
#define DIFF_CLEAN_RUN          3       /* recent mornings with no snooze that earn more time */
#define DIFF_MODULES_MAX        PROTO_MAX_MODULES

enum PuzzleKind { PUZZLE_DIAL = 1, PUZZLE_WIRE = 2 };
enum PuzzleSource { PUZZLE_SRC_BANK = 0, PUZZLE_SRC_AI = 1 };

/* One puzzle. memset the whole struct before filling it (it is stored in flash as a blob). */
struct Puzzle {
    uint16_t setter_ms;                     /* LLM latency, 0 for the bank */
    uint16_t checker_ms;
    char text[PUZZLE_TEXT_BUF];
    uint8_t kind;                           /* PuzzleKind */
    uint8_t dial_code[PROTO_DIAL_DIGITS];   /* DIAL: the answer */
    uint8_t wire_count;                     /* WIRE: 3..6 */
    uint8_t wire_k;                         /* WIRE: the answer, 1-based from the left */
    uint8_t source;                         /* PuzzleSource */
    uint8_t verified;                       /* 1 = the checker model reproduced the answer */
    uint8_t attempts;                       /* setter/checker rounds used (0 for the bank) */
    char setter[PUZZLE_MODEL_LEN];          /* model that wrote the text */
    char checker[PUZZLE_MODEL_LEN];         /* model that solved it */
    char checker_answer[PUZZLE_ANSWER_LEN]; /* what the checker said (also when it was wrong) */
};

struct PuzzleSet {
    uint32_t magic;                         /* PUZZLE_MAGIC */
    uint32_t generated_unix;                /* 0 = built from the bank only, not stored */
    uint8_t ai_count;                       /* puzzles of source AI */
    uint8_t reserved[3];
    Puzzle dial[PUZZLE_PAIRS];
    Puzzle wire[PUZZLE_PAIRS];
};

/* ================= text helpers for model replies ================= */

/* Copy text to out without <think>...</think> blocks. A stray "</think>" (the opening tag was
 * eaten by the server) drops everything before it; an unterminated "<think>" drops everything
 * after it. out is always NUL-terminated and truncated to out_size - 1. Returns its length. */
size_t strip_think(const char *text, char *out, size_t out_size);

/* The first balanced {...} after stripping <think> blocks; tolerant of prose or markdown fences
 * around it and of braces inside JSON strings. False if there is none or it does not fit. */
bool extract_json_object(const char *text, char *out, size_t out_size);

/* Value of a top-level key of a flat JSON object: a string (escapes decoded) or a bare token
 * such as 4271. False if the key is missing, malformed, or the value does not fit. */
bool json_get_string(const char *json, const char *key, char *out, size_t out_size);

/* Text length 10..80 (wire 10..60), printable ASCII only (0x20..0x7E) and, for DIAL, the 4-digit
 * answer must not appear as a literal substring. answer: "4271" for DIAL, "3" for WIRE. */
bool validate_puzzle(const char *text, int kind, const char *answer);

/* Checker reply {"answer":"..."} -> normalised answer in out. DIAL: exactly 4 digits (spaces
 * around are ignored). WIRE: an integer 1..wire_count (written back without leading zeros). */
bool parse_checker_answer(const char *json, int kind, int wire_count, char *out, size_t out_size);

/* ================= answers ================= */
void dial_code_text(const uint8_t code[PROTO_DIAL_DIGITS], char out[PROTO_DIAL_DIGITS + 1]);
/* "4271" for a DIAL puzzle, "3" for a WIRE puzzle; false if the puzzle kind is unknown. */
bool puzzle_answer_text(const Puzzle &p, char *out, size_t out_size);
/* Random picks from one 32-bit random word (esp_random on the board). */
void dial_code_from_random(uint32_t r, uint8_t code[PROTO_DIAL_DIGITS]);
void wire_pick_from_random(uint32_t r, int *wire_count, int *k);

/* ================= template bank (correct by construction) ================= */
#define DIAL_FAMILIES   4
#define WIRE_FAMILIES   4
#define DIAL_MAX_RULES  5

enum DialRel {
    DREL_EQ = 0,        /* d[a] == c */
    DREL_ADD,           /* d[a] == d[b] + c            (c may be negative) */
    DREL_MUL,           /* d[a] == d[b] * c            (c >= 2) */
    DREL_SUM_ALL,       /* d[0] + d[1] + d[2] + d[3] == c */
    DREL_SUM_PAIR,      /* d[a] + d[b] == c */
    DREL_DIFF_PAIR      /* d[a] - d[b] == c            (c >= 0) */
};

struct DialRule {
    uint8_t rel;
    uint8_t a;          /* digit index 0..3 */
    uint8_t b;
    int8_t c;
};

/* The clue as data: the rules together have exactly one solution, the code it was built from. */
struct DialClue {
    uint8_t family;
    uint8_t n;
    DialRule rule[DIAL_MAX_RULES];
};

/* variant: family = variant % 4 (chain / reverse chain / pivot + sum / pair sums and differences),
 * variant / 4 % 4 changes the details (start digit, multiply instead of add, pairing, pivot). */
DialClue dial_clue_build(const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant);
/* Renders to e.g. "1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3". Returns the length, 0 if it does not fit. */
size_t dial_clue_render(const DialClue &clue, char *out, size_t out_size);
size_t dial_template(const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant, char *out, size_t out_size);

enum WireOp { WOP_NONE = 0, WOP_ADD, WOP_SUB, WOP_MUL };

/* family 0: "Nth from the left" (a = k), 1: "Nth from the right" (a = n - k + 1),
 * 2: "wire a op b" (value = k), 3: skip a from the left (a = k - 1) or from the right (a = n - k). */
struct WireClue {
    uint8_t family;
    uint8_t n;          /* wire count 3..6 */
    uint8_t a;
    uint8_t b;
    uint8_t op;         /* WireOp, family 2 only */
    uint8_t from_right; /* family 3 only */
};

/* k is the 1-based wire from the left, 1 <= k <= n, 3 <= n <= 6. Out-of-range input is clamped. */
WireClue wire_clue_build(int k, int n, unsigned variant);
size_t wire_clue_render(const WireClue &clue, char *out, size_t out_size);
size_t wire_template(int k, int n, unsigned variant, char *out, size_t out_size);

#define PUZZLE_VARIANTS 16u     /* dial_template / wire_template variants (4 families x 4 details) */

/* Bank puzzles: text from the templates, source BANK, not verified by a model. */
void puzzle_fill_bank_dial(Puzzle *p, const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant);
void puzzle_fill_bank_wire(Puzzle *p, int wire_count, int k, unsigned variant);

/* A whole set from the bank: 3 random words per pair (dial code, wire pick, template variants). */
#define PUZZLE_BANK_WORDS (3u * PUZZLE_PAIRS)
void puzzle_set_fill_bank(PuzzleSet *s, const uint32_t rnd[PUZZLE_BANK_WORDS]);

/* Sanity check of a set read back from flash: magic, kinds, ranges, terminated text. */
bool puzzle_set_valid(const PuzzleSet &s);

/* ================= difficulty ================= */
struct Difficulty {
    uint8_t module_count;                   /* 3..4 */
    uint16_t time_limit_s;                  /* 120..240 */
    char explanation[PUZZLE_EXPLAIN_MAX + 1];
};

/* recent: newest first, n records (only the first DIFF_WINDOW count). Empty history = base. */
Difficulty difficulty(const HistRecord *recent, int n);

/* ================= BOMB_CONFIG ================= */
/* Modules DIAL, WIRES, TRIPWIRE (+ DIAL when 4), the puzzle answers, default tripwire band and
 * strikes. Every field is inside the MCX's range checks. */
void puzzle_build_config(const Difficulty &d, const Puzzle &dial, const Puzzle &wire, proto_bomb_config_t *out);

/* ================= selection rules ================= */
/* Lowest pair index whose bit is clear in used_mask, -1 if all `count` are used. */
int pair_next_unused(uint8_t used_mask, int count);
/* Marks idx used. When that was the last unused pair the mask restarts at 0 and *cycled is true. */
uint8_t pair_mark_used(uint8_t used_mask, int idx, int count, bool *cycled);

/* Order in which the checker chain is tried: every model except the setter's first (chain
 * order), the setter's own model last (only used when no other model answers). Returns the count. */
int checker_order(const char *const *chain, int n, const char *setter, int *order_out);

/* Is a stored set still good? Needs at least one AI puzzle and an age of at most max_age_s.
 * now_unix = 0 (clock unknown): a set with AI puzzles counts as fresh. */
bool puzzle_set_fresh(uint32_t generated_unix, uint8_t ai_count, uint32_t now_unix, uint32_t max_age_s);

/* Does the MCX need a new BOMB_CONFIG for the NEXT bomb? True when, inside a session, the game
 * state just changed to EXPLODED or to ARMED (a snooze). */
bool config_trigger_needed(uint8_t prev_state, uint8_t state, bool session_active);
/* A new bomb just started (the state changed to BOMB_ACTIVE): it used the config it was given. */
bool bomb_just_started(uint8_t prev_state, uint8_t state);

#endif /* PUZZLE_LOGIC_H */
