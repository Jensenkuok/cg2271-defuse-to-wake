/*
 * Defuse to Wake - acceptance gate for LLM puzzles (pure C++11, no Arduino headers, no heap).
 * Compiled by the sketch and by the host tests (tools/test_puzzle.cpp), so the host tests can feed
 * canned LLM replies through exactly the checks the board runs.
 *
 * A setter reply {"puzzle":"<riddle>","rules":"<rules>"} is kept only if ALL of these hold, cheapest
 * first (the first failure is the reason that gets logged):
 *   1. the reply holds a JSON object with a "puzzle" and a "rules" string
 *   2. validate_puzzle on the riddle text (O3 length, printable ASCII, no leaked code / wire number)
 *      and it fits its OLED rows after word wrap (puzzle_text_fits_oled)
 *   3. the rules parse, have exactly one solution, and that solution is the answer the ESP32 picked
 *   4. the rules' computed rating (puzzle_rules.h) equals the requested level
 * and then, in a second call, a checker reply {"answer":".."} given only the riddle TEXT (never the
 * rules) must equal the answer, from a model whose ID differs from the setter's (O5).
 * The answer always comes from the ESP32, the level always from the solver, never from the LLM.
 */
#ifndef PUZZLE_GATE_H
#define PUZZLE_GATE_H

#include <stddef.h>

#include "puzzle_logic.h"
#include "puzzle_rules.h"

#define GATE_WHY_MAX        96      /* buffer for the one-line reason */
#define GATE_RULES_BUF      (PR_MAX_CHARS + 24) /* a rules string longer than this cannot be valid anyway */

enum GateStatus {
    GATE_OK = 0,
    GATE_NO_JSON,               /* no JSON object in the reply */
    GATE_NO_PUZZLE,             /* "puzzle" missing, not a string, or too long to keep */
    GATE_NO_RULES,              /* "rules" missing, not a string, or too long to keep */
    GATE_TEXT_REJECTED,         /* validate_puzzle failed (length, characters, leaked code or number) */
    GATE_RULES_SYNTAX,          /* the rules do not parse in the rule language */
    GATE_RULES_UNSOLVABLE,      /* the rules have no solution */
    GATE_RULES_AMBIGUOUS,       /* the rules have two or more solutions */
    GATE_RULES_WRONG_ANSWER,    /* exactly one solution, but not the ESP32's answer */
    GATE_WRONG_LEVEL,           /* the computed rating is not the requested level */
    GATE_CHECKER_SAME_MODEL,    /* the checker's model ID equals the setter's (O5) */
    GATE_CHECKER_UNUSABLE,      /* the checker reply has no usable {"answer"} */
    GATE_CHECKER_DISAGREES      /* the checker solved the text to a different answer */
};

/* What the ESP32 asked for. */
struct GateTarget {
    int kind;                   /* PUZZLE_DIAL or PUZZLE_WIRE */
    int wire_count;             /* wire: 3..6 */
    int answer;                 /* dial: the code 0..9999; wire: the wire 1..wire_count */
    int level;                  /* PR_EASY / PR_NORMAL / PR_HARD */
};

/* The checks 1-4 on a setter reply. json_buf (json_size bytes) is scratch for the extracted object.
 * On GATE_OK text_out holds the riddle. why always gets a one-line reason (also on success). */
int gate_check_setter(const char *reply, const GateTarget &target, char *json_buf, size_t json_size,
                      char *text_out, size_t text_size, char *why, size_t why_size);

/* The checker step. said (if not NULL) receives the checker's normalised answer when there is one. */
int gate_check_checker(const char *reply, const GateTarget &target, const char *setter_model,
                       const char *checker_model, char *json_buf, size_t json_size, char *said,
                       size_t said_size, char *why, size_t why_size);

/* O3 counts characters, but the OLED wraps at spaces: does the text fit its rows (3 dial, 2 wire)
 * after the display's own word wrap, i.e. without the scrolling marquee? Used by the gate for LLM
 * texts and by the procedural bank for its own. */
#define GATE_DIAL_ROWS      3
#define GATE_WIRE_ROWS      2
bool puzzle_text_fits_oled(const char *text, int kind);

/* "4271" for a dial answer (leading zeros kept), "3" for a wire. */
void gate_answer_text(const GateTarget &target, char *out, size_t size);

#endif /* PUZZLE_GATE_H */
