/*
 * Defuse to Wake - procedural puzzle bank, internal parts shared by puzzle_bank.cpp (loop, text
 * conversion, acceptance, template fallback), puzzle_bank_dial.cpp and puzzle_bank_wire.cpp (one
 * random rule set per call). Not part of the public interface: use puzzle_bank.h.
 */
#ifndef PUZZLE_BANK_INT_H
#define PUZZLE_BANK_INT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "puzzle_bank.h"

#define DIGITS              4           /* == PROTO_DIAL_DIGITS, as a signed int */
#define DIGIT_VALUES        10
#define SUM_MAX_VALUE       36          /* 4 * 9 */
#define RULE_PIECE_MAX      40          /* one rule in the rule language */
#define BANK_WIRE_POOL_MAX  512         /* every wire rule the generator can write (see puzzle_bank_wire.cpp) */

static_assert(DIGITS == (int)PROTO_DIAL_DIGITS, "the bank assumes 4 dial digits");

namespace bank_int {

/* Seeded xorshift32: same seed, same sequence, so a host test can reproduce any puzzle. */
struct Rng {
    uint32_t s;
};

inline void rng_seed(Rng *r, uint32_t seed)
{
    r->s = seed * 2654435761u + 0x9E3779B9u;
    if (r->s == 0u) {
        r->s = 1u;
    }
}

inline uint32_t rng_next(Rng *r)
{
    uint32_t x = r->s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

/* 0..n-1 (0 when n <= 1). */
inline int rng_below(Rng *r, int n)
{
    return n <= 1 ? 0 : (int)((rng_next(r) >> 8) % (uint32_t)n);
}

inline bool rel_holds(int a, int rel, int b)
{
    switch (rel) {
    case 0:  return a == b;     /* PR_REL_EQ */
    case 1:  return a != b;     /* PR_REL_NE */
    case 2:  return a < b;      /* PR_REL_LT */
    case 3:  return a > b;      /* PR_REL_GT */
    case 4:  return a <= b;     /* PR_REL_LE */
    default: return a >= b;     /* PR_REL_GE */
    }
}

inline const char *rel_text(int rel)
{
    static const char *const k_rel[] = { "=", "!=", "<", ">", "<=", ">=" };

    return rel >= 0 && rel <= 5 ? k_rel[rel] : "?";
}

/* Appends s to out (size bytes, *len chars used). False when it does not fit; out is untouched then. */
inline bool append_str(char *out, size_t size, size_t *len, const char *s)
{
    const size_t n = strlen(s);

    if (*len + n + 1u > size) {
        return false;
    }
    memcpy(out + *len, s, n + 1u);
    *len += n;
    return true;
}

/* ---- dial (puzzle_bank_dial.cpp) ---- */

/* One random rule set for the answer `code` at `level` (PR_EASY..PR_HARD), as rule language text.
 * False when this try produced no candidate; the caller still has to check it with pr_check. */
bool dial_try_rules(int level, const uint8_t code[DIGITS], Rng *rng, char *rules, size_t size);

/* ---- wire (puzzle_bank_wire.cpp) ---- */

/* The rules that are true at wire k of n, say something, are not printed with the answer in them and
 * fit the level (EASY: a single rule pins the wire; NORMAL / HARD: none does). Returns the count
 * (<= BANK_WIRE_POOL_MAX); pool holds opaque rule indexes. */
int wire_pool(int level, int n, int k, uint16_t *pool);

/* One random rule set from the pool as rule language text. False when this try failed. */
bool wire_try_rules(int level, int n, int k, const uint16_t *pool, int pool_n, Rng *rng, char *rules, size_t size);

}  // namespace bank_int

#endif /* PUZZLE_BANK_INT_H */
