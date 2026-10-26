/*
 * Defuse to Wake - puzzle rule language: parser, brute-force prover and difficulty rater
 * (pure C++11, no Arduino headers, no heap, no globals, fixed-size arrays).
 * Compiled by the sketch and by the host tests (tools/test_rules.cpp).
 *
 * Why it exists: the ESP32 picks the ANSWER of every puzzle (4-digit dial code, or the wire to
 * cut); the LLM writes the riddle text AND a tiny machine-readable rule set that says the same
 * thing. This module proves, by brute force, how many answers the rule set has (a puzzle is only
 * kept when there is exactly one and it equals ours) and rates how hard it is to solve with a
 * solver whose power defines the tiers, so the difficulty is computed, never taken from the LLM.
 *
 * Rule language (rules separated by ';', whitespace ignored, at most PR_MAX_RULES rules and
 * PR_MAX_CHARS characters; one trailing ';' is tolerated):
 *   terms   d1 d2 d3 d4   dial digits 0..9          (dial only)
 *           sum           d1+d2+d3+d4               (dial only)
 *           w             the wire to cut, 1..n     (wire only)
 *           n             the wire count, a constant (wire only)
 *           0..99         integer constants
 *   exprs   + - * with the usual precedence, left to right, parentheses (no unary minus)
 *   rules   expr REL expr   REL is one of  =  !=  <  >  <=  >=
 *           odd(expr)   even(expr)
 *           alldiff     all four digits differ (dial only)
 *
 * Difficulty tiers (planner decision O1):
 *   Dial   EASY   : propagation of rules that have ONE unassigned digit solves it
 *          NORMAL : also arc consistency of two-digit rules and joint filtering of rule pairs
 *                   over the same two digits solves it
 *          HARD   : exactly one solution, but NORMAL does not find it (needs search)
 *   Wire   level = size of the smallest sub-set of rules that already pins the wire: 1/2/3+.
 * Uniqueness is always decided by brute force (10,000 codes / n wires); the solver only grades.
 */
#ifndef PUZZLE_RULES_H
#define PUZZLE_RULES_H

#include <stdint.h>

/* ---- Limits of the language ---- */
#define PR_MAX_RULES        6       /* rules per set */
#define PR_MAX_CHARS        120     /* characters of rule text */
#define PR_MAX_NODES        128     /* expression nodes per set: every node needs >= 1 character */
#define PR_MAX_DEPTH        6       /* nesting of parentheses */
#define PR_CONST_MAX        99      /* largest integer constant */
#define PR_VALUE_LIMIT      1000000 /* |value| any sub-expression may reach (interval-checked at parse time) */
#define PR_DIAL_DIGITS      4
#define PR_DIGIT_VALUES     10      /* a dial digit is 0..9 */
#define PR_DIAL_CODES       10000   /* brute-force space of the dial */
#define PR_WIRE_MIN         3       /* wire counts the game uses */
#define PR_WIRE_MAX         6
#define PR_NO_SOLUTION      (-1)

/* What the rule set is about: it decides which terms are legal. */
enum PrKind { PR_KIND_DIAL = 1, PR_KIND_WIRE = 2 };

/* Parse result. PR_OK is 0; every other code means the set is unusable. */
enum PrError {
    PR_OK = 0,
    PR_ERR_ARGS,            /* NULL pointer, unknown kind, or wire count outside 3..6 */
    PR_ERR_EMPTY,           /* no rule at all */
    PR_ERR_TOO_LONG,        /* more than PR_MAX_CHARS characters */
    PR_ERR_TOO_MANY_RULES,  /* more than PR_MAX_RULES rules */
    PR_ERR_CHAR,            /* a character that is not part of the language */
    PR_ERR_SYNTAX,          /* the characters are legal but the grammar is not respected */
    PR_ERR_NUMBER,          /* integer constant above PR_CONST_MAX */
    PR_ERR_UNKNOWN_TERM,    /* a name that is not a term of the language (d5, x, foo) */
    PR_ERR_TERM_KIND,       /* a legal term used in the wrong kind (w in a dial set, d1 in a wire set) */
    PR_ERR_TOO_COMPLEX,     /* too many expression nodes or parentheses nested too deep */
    PR_ERR_RANGE            /* a sub-expression can exceed +-PR_VALUE_LIMIT */
};

/* Rating. PR_LEVEL_NONE = nothing was rated (parse failed). */
enum PrLevel {
    PR_LEVEL_NONE = 0,
    PR_EASY = 1,
    PR_NORMAL = 2,
    PR_HARD = 3,
    PR_UNSOLVABLE = 4,      /* no solution */
    PR_AMBIGUOUS = 5        /* two or more solutions */
};

/* Solver power for pr_propagate_dial (the same numbers as the levels). */
#define PR_TIER_EASY        1
#define PR_TIER_NORMAL      2

/* ---- Compiled form (internal layout, public so the caller can allocate a PrSet on its stack) ---- */
enum PrNodeOp { PR_N_CONST = 0, PR_N_DIGIT, PR_N_SUM, PR_N_WIRE, PR_N_ADD, PR_N_SUB, PR_N_MUL };
enum PrRuleKind { PR_R_REL = 0, PR_R_ODD, PR_R_EVEN, PR_R_ALLDIFF };
enum PrRel { PR_REL_EQ = 0, PR_REL_NE, PR_REL_LT, PR_REL_GT, PR_REL_LE, PR_REL_GE };

struct PrNode {
    uint8_t op;             /* PrNodeOp */
    uint8_t a;              /* DIGIT: index 0..3; ADD/SUB/MUL: left child node */
    uint8_t b;              /* ADD/SUB/MUL: right child node */
    int16_t value;          /* CONST */
};

struct PrRule {
    uint8_t kind;           /* PrRuleKind */
    uint8_t rel;            /* PrRel (REL only) */
    uint8_t lhs;            /* root node of the left expression (the argument for ODD/EVEN) */
    uint8_t rhs;            /* root node of the right expression (REL only) */
    uint8_t vars;           /* bit i = the rule depends on dial digit i (sum and alldiff: all four) */
};

struct PrSet {
    uint8_t kind;           /* PrKind */
    uint8_t wire_count;     /* n (wire sets) */
    uint8_t rule_count;
    uint8_t node_count;
    int16_t error_pos;      /* character index of the first error, -1 when PR_OK */
    PrRule rules[PR_MAX_RULES];
    PrNode nodes[PR_MAX_NODES];
};

/* Everything pr_check learns about one rule text. */
struct PrCheck {
    int error;              /* PrError; PR_OK when the text parsed */
    int level;              /* PrLevel (PR_LEVEL_NONE when parse failed) */
    int count;              /* solutions found, capped at 2 (0, 1 or 2 = "two or more") */
    int solution;           /* the first solution (dial: d1*1000+d2*100+d3*10+d4; wire: w), PR_NO_SOLUTION if none */
    int unique;             /* 1 when count == 1 */
    int matches_answer;     /* 1 when count == 1 and solution == answer */
};

/* Parses text into *out_set. Returns PR_OK or an error code (also stored in nothing else: the set
 * is only meaningful on PR_OK). wire_count (3..6) is only used for PR_KIND_WIRE. */
int pr_parse(const char *text, int kind, int wire_count, PrSet *out_set);

/* Brute-force solution count: all 10,000 codes (dial) or w = 1..n (wire). Counting stops once
 * `limit` solutions are found (limit <= 0: count them all). *out_first receives the first
 * solution found, or PR_NO_SOLUTION; it may be NULL. Dial solutions are d1*1000+d2*100+d3*10+d4. */
int pr_count_solutions(const PrSet *set, int *out_first, int limit);

/* The rating: UNSOLVABLE / AMBIGUOUS from the count, else the lowest tier that solves it. */
int pr_rate(const PrSet *set);

/* Runs the dial solver at one tier (PR_TIER_EASY or PR_TIER_NORMAL) on its own. Returns 1 when
 * every digit domain is reduced to a single value (*out_code gets it, may be NULL), else 0. */
int pr_propagate_dial(const PrSet *set, int tier, int *out_code);

/* Parse + count + rate + compare with the answer we picked (dial: 0..9999, wire: 1..n). */
void pr_check(const char *text, int kind, int wire_count, int answer, PrCheck *out);

/* Name of a level / error for logs and tests ("EASY", "PR_ERR_SYNTAX"...). Never NULL. */
const char *pr_level_name(int level);
const char *pr_error_name(int error);

#endif /* PUZZLE_RULES_H */
