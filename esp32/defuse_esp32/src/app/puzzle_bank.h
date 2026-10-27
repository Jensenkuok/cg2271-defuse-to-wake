/*
 * Defuse to Wake - procedural puzzle bank (pure C++11, no Arduino headers, no heap, no globals).
 * Compiled by the sketch and by the host tests (tools/test_puzzle.cpp).
 *
 * The bank is what the nightly run falls back to when the LLM fails, so it has to be as strict as the
 * LLM path: for a given ANSWER and LEVEL it builds a rule set in the puzzle_rules.h language that is
 * true at the answer, renders it as compact OLED text, and keeps it only when
 *   - the display text passes validate_puzzle (O3 length, no leaked code / wire number, O4),
 *   - the text converts BACK to rules (bank_text_to_rules) - the renderer is never trusted - and
 *   - pr_check on those converted rules says: exactly one solution, equal to the answer, rated at
 *     the requested level.
 * The generator is random search with a bounded number of tries (BANK_TRIES_*) and a seed, so a
 * host test can reproduce any result. If no rule set is found the result is flagged `fallback`
 * (so the caller can log it and the tests can count it) but is STILL held to the same bar: dial
 * falls back to the old template families (chain = EASY, pairs = NORMAL...) and then to a generated
 * EASY chain; wire falls back to the EASY recipe and then to a fixed rule that cannot name the wire.
 * A fallback text is validated, fits its OLED rows, is proven by pr_check and carries its MEASURED level.
 *
 * Display text:
 *   dial  "1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3", "Sum 17; 4th-1st=1; All differ"
 *         (rule language with 1st..4th for d1..d4, "Sum N" for sum=N, "All differ" for alldiff)
 *   wire  "Cut wire w of n: w>2; w<n; odd(w)"   (w = wire from the left, 1 = leftmost; n = wire count)
 */
#ifndef PUZZLE_BANK_H
#define PUZZLE_BANK_H

#include <stddef.h>
#include <stdint.h>

#include "puzzle_logic.h"
#include "puzzle_rules.h"

#define BANK_TRIES_DIAL     150     /* random rule sets tried per dial puzzle before the template fallback */
#define BANK_TRIES_WIRE     1500    /* wire rule sets are tiny (<= 6 wires), so many more tries are cheap */
#define BANK_YIELD_EVERY    8       /* the caller's yield hook runs every this many tries */
#define BANK_RULES_MAX      (PR_MAX_CHARS + 8)  /* buffer for a rule text */
#define BANK_WIRE_PREFIX    "Cut wire w of n: "

struct BankRequest {
    int kind;                       /* PUZZLE_DIAL or PUZZLE_WIRE */
    uint8_t code[PROTO_DIAL_DIGITS];/* dial: the answer */
    int wire_count;                 /* wire: 3..6 */
    int wire_k;                     /* wire: the answer, 1..wire_count from the left */
    int level;                      /* PR_EASY / PR_NORMAL / PR_HARD */
    uint32_t seed;                  /* same request + same seed = same puzzle */
    uint16_t max_tries;             /* 0 = BANK_TRIES_DIAL / BANK_TRIES_WIRE; tests lower it to exercise the fallback */
    void (*yield)(void);            /* optional: called every BANK_YIELD_EVERY tries so a task can let others run */
};

struct BankResult {
    char text[PUZZLE_TEXT_BUF];     /* display text, always set */
    uint8_t level;                  /* measured rating of the text (PrLevel), 0 if even that failed */
    uint8_t fallback;               /* 1 = no generated rule set: a lower level or an old template family was used */
    uint16_t tries;                 /* random rule sets tried */
};

/* Builds one puzzle. Never fails: the worst case is a proven EASY text, flagged fallback. */
void bank_generate(const BankRequest &req, BankResult *out);

/* Display text -> rule language text (see above). False when the text is not in the bank's shape or
 * the rules do not fit. wire_count is not needed for the conversion itself. */
bool bank_text_to_rules(const char *text, int kind, char *rules, size_t rules_size);

/* Rule language text -> display text. The inverse of bank_text_to_rules for texts the generator makes. */
bool bank_rules_to_text(const char *rules, int kind, char *text, size_t text_size);

/* Converts the display text back and runs pr_check on it. True when the text is a bank-shaped text
 * whose rules have exactly one solution equal to `answer` (dial 0..9999, wire 1..n); *level gets the
 * rating (PrLevel), may be NULL. */
bool bank_recheck(const char *text, int kind, int wire_count, int answer, int *level);

/* A whole set at one level: 3 random words per pair as in puzzle_set_fill_bank (dial code, wire pick,
 * seed). Every puzzle is source BANK, Puzzle.level is its measured level. */
void bank_fill_set(PuzzleSet *s, const uint32_t rnd[PUZZLE_BANK_WORDS], int level);

#endif /* PUZZLE_BANK_H */
