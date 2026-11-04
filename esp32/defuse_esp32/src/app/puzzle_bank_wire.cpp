/*
 * Defuse to Wake - procedural puzzle bank: wire rule sets (see puzzle_bank.h).
 *
 * Every wire rule the generator can write is enumerated by an index (WDesc): "lhs rel rhs" with
 * lhs = w*a+b and rhs = n*c+d, or odd(w) / even(w). For wire k of n the pool holds the rules that are
 * true at k, say something, and do not print k; a level picks from it:
 *   EASY    one rule that pins the wire alone
 *   NORMAL  two rules, neither pins the wire alone, together they do
 *   HARD    rules that each remove at least one wire, until only k is left (three or more needed)
 * Wire sets are tiny (<= 6 wires), so thousands of tries cost almost nothing.
 */
#include <stdio.h>

#include "puzzle_bank_int.h"
#include "puzzle_rules.h"

#define WIRE_RULES_HARD_MAX 4

using namespace bank_int;

namespace {


/* A wire rule "lhs rel rhs": lhs = w * a + b, rhs = n * c + d (c = 0: a plain constant d); or odd(w) /
 * even(w). All of them are enumerated by index so the pool for (n, k) needs no storage but indexes. */
struct WDesc {
    uint8_t kind;           /* 0 = relation, 1 = odd(w), 2 = even(w) */
    uint8_t rel;
    int8_t a;
    int8_t b;
    int8_t c;
    int8_t d;
};

const int8_t k_lhs[][2] = { { 1, 0 }, { 1, 1 }, { 1, -1 }, { 2, 0 }, { 2, -1 } };
#define W_LHS_COUNT     5
#define W_REL_COUNT     6
#define W_CONST_COUNT   9       /* constants 1..9 */
#define W_NOFF_COUNT    8       /* n-5 .. n+2 */
#define W_NOFF_MIN      (-5)
#define W_RHS_COUNT     (W_CONST_COUNT + W_NOFF_COUNT)
#define W_REL_CANDS     (W_LHS_COUNT * W_REL_COUNT * W_RHS_COUNT)
#define W_CAND_COUNT    (W_REL_CANDS + 2)

WDesc wire_desc(int idx)
{
    WDesc w;

    memset(&w, 0, sizeof(w));
    if (idx >= W_REL_CANDS) {
        w.kind = (uint8_t)(idx == W_REL_CANDS ? 1 : 2);
        return w;
    }
    const int rhs = idx % W_RHS_COUNT;
    const int rel = (idx / W_RHS_COUNT) % W_REL_COUNT;
    const int lhs = idx / (W_RHS_COUNT * W_REL_COUNT);

    w.rel = (uint8_t)rel;
    w.a = k_lhs[lhs][0];
    w.b = k_lhs[lhs][1];
    if (rhs < W_CONST_COUNT) {
        w.c = 0;
        w.d = (int8_t)(rhs + 1);
    } else {
        w.c = 1;
        w.d = (int8_t)(W_NOFF_MIN + (rhs - W_CONST_COUNT));
    }
    return w;
}

bool wire_desc_holds(const WDesc &w, int n, int wire)
{
    if (w.kind == 1) {
        return (wire & 1) == 1;
    }
    if (w.kind == 2) {
        return (wire & 1) == 0;
    }
    return rel_holds(w.a * wire + w.b, w.rel, w.c * n + w.d);
}

/* Bit i = the rule holds for wire i (bits 1..n). */
unsigned wire_desc_mask(const WDesc &w, int n)
{
    unsigned m = 0u;

    for (int wire = 1; wire <= n; wire++) {
        if (wire_desc_holds(w, n, wire)) {
            m |= 1u << wire;
        }
    }
    return m;
}

void wire_desc_render(const WDesc &w, char *out, size_t size)
{
    char lhs[12];
    char rhs[12];

    if (w.kind != 0) {
        snprintf(out, size, "%s(w)", w.kind == 1 ? "odd" : "even");
        return;
    }
    snprintf(lhs, sizeof(lhs), "%s", w.a == 2 ? "w*2" : "w");
    if (w.b != 0) {
        const size_t n = strlen(lhs);

        snprintf(lhs + n, sizeof(lhs) - n, "%c%d", w.b > 0 ? '+' : '-', w.b > 0 ? w.b : -w.b);
    }
    if (w.c == 0) {
        snprintf(rhs, sizeof(rhs), "%d", w.d);
    } else if (w.d == 0) {
        snprintf(rhs, sizeof(rhs), "n");
    } else {
        snprintf(rhs, sizeof(rhs), "n%c%d", w.d > 0 ? '+' : '-', w.d > 0 ? w.d : -w.d);
    }
    snprintf(out, size, "%s%s%s", lhs, rel_text(w.rel), rhs);
}

unsigned wire_full_mask(int n)
{
    return ((1u << (n + 1)) - 1u) & ~1u;           /* bits 1..n */
}

/* Readability: of two random pool entries the one with the plainer left side ("w" before "w+1", "w*2")
 * is used, so most rules read "w>n-2" and only some "w*2-1>n". */
int wire_pick(const uint16_t *pool, int pool_n, Rng *rng)
{
    const int a = pool[rng_below(rng, pool_n)];
    const int b = pool[rng_below(rng, pool_n)];
    const int ca = a >= W_REL_CANDS ? 0 : a / (W_RHS_COUNT * W_REL_COUNT);
    const int cb = b >= W_REL_CANDS ? 0 : b / (W_RHS_COUNT * W_REL_COUNT);

    return cb < ca ? b : a;
}

}  // namespace

namespace bank_int {

int wire_pool(int level, int n, int k, uint16_t *pool)
{
    const unsigned full = wire_full_mask(n);
    const unsigned only_k = 1u << k;
    char piece[RULE_PIECE_MAX];
    int count = 0;

    for (int idx = 0; idx < W_CAND_COUNT; idx++) {
        const WDesc w = wire_desc(idx);
        const unsigned m = wire_desc_mask(w, n);

        if ((m & only_k) == 0u || m == full) {
            continue;                               /* false at the answer, or says nothing */
        }
        if (level == PR_EASY ? m != only_k : m == only_k) {
            continue;                               /* EASY: only rules that pin the wire alone; the others: none of those */
        }
        wire_desc_render(w, piece, sizeof(piece));
        if (puzzle_text_has_number(piece, k)) {
            continue;                               /* O4: never print the answer */
        }
        pool[count++] = (uint16_t)idx;
    }
    return count;
}

bool wire_try_rules(int level, int n, int k, const uint16_t *pool, int pool_n, Rng *rng, char *rules, size_t size)
{
    const unsigned only_k = 1u << k;
    int picked[WIRE_RULES_HARD_MAX];
    int count = 0;
    char piece[RULE_PIECE_MAX];
    size_t len = 0u;

    if (pool_n == 0) {
        return false;
    }
    if (level == PR_EASY) {
        picked[count++] = wire_pick(pool, pool_n, rng);
    } else if (level == PR_NORMAL) {
        picked[count++] = wire_pick(pool, pool_n, rng);
        picked[count++] = wire_pick(pool, pool_n, rng);
    } else {
        unsigned left = wire_full_mask(n);

        while (left != only_k && count < WIRE_RULES_HARD_MAX) {
            const int idx = wire_pick(pool, pool_n, rng);
            const unsigned next = left & wire_desc_mask(wire_desc(idx), n);

            if (next != left) {                     /* every rule must remove at least one wire */
                picked[count++] = idx;
                left = next;
            } else if (rng_below(rng, 4) == 0) {
                break;
            }
        }
        if (left != only_k) {
            return false;
        }
    }
    rules[0] = '\0';
    for (int i = 0; i < count; i++) {
        wire_desc_render(wire_desc(picked[i]), piece, sizeof(piece));
        if ((i > 0 && !append_str(rules, size, &len, ";")) || !append_str(rules, size, &len, piece)) {
            rules[0] = '\0';
            return false;
        }
    }
    return true;
}

}  // namespace bank_int
