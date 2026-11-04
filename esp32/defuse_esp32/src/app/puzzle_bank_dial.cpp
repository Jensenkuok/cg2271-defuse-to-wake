/*
 * Defuse to Wake - procedural puzzle bank: dial rule sets (see puzzle_bank.h).
 *
 * A dial rule set is built from structured candidates (Cand) that are true at the answer, by one
 * recipe per level, and handed on as rule language text. Nothing here is trusted: the caller
 * converts the display text back and rates it with pr_check.
 *   EASY    a chain: one digit is given and every other digit is pinned by a rule with a single unknown
 *   NORMAL  rules that must be solved in pairs (two rules over the same two digits, then a chain/sum)
 *   HARD    no pinned digit and no solved pair: sum / alldiff / pair relations until unique
 */
#include <stdio.h>

#include "puzzle_bank_int.h"
#include "puzzle_rules.h"

#define SUM_TRIVIAL         1           /* sums this close to 0 or 36 force (almost) every digit */
#define HARD_ADD_GUARD      24          /* attempts to add rules to one HARD set before restarting it */
#define HARD_PICK_ATTEMPTS  8
#define NORMAL_PAIR_TRIES   12

using namespace bank_int;

namespace {

void digits_from_code(int code, int d[DIGITS])
{
    d[0] = code / 1000;
    d[1] = (code / 100) % 10;
    d[2] = (code / 10) % 10;
    d[3] = code % 10;
}

bool all_distinct(const int d[DIGITS])
{
    for (int i = 0; i < DIGITS; i++) {
        for (int j = i + 1; j < DIGITS; j++) {
            if (d[i] == d[j]) {
                return false;
            }
        }
    }
    return true;
}

/* ======================================================================================== */
/* Dial rule candidates: structured rules that are true at the answer                       */
/* ======================================================================================== */

enum CandKind {
    C_PIN = 0,      /* d[i] = c */
    C_LIN,          /* d[i] = d[j] + c */
    C_MULC,         /* d[i] = d[j] * c */
    C_SUMP,         /* d[i] + d[j] = c */
    C_DIFF,         /* d[i] - d[j] = c */
    C_PRODP,        /* d[i] * d[j] = c */
    C_SUM,          /* sum = c */
    C_CMP,          /* d[i] rel d[j] */
    C_PAR,          /* odd(d[i]) when c = 1, even otherwise */
    C_ALLD,         /* alldiff */
    C_CREL,         /* d[i] rel c */
    C_SUMREL        /* sum rel c */
};

struct Cand {
    uint8_t kind;
    uint8_t i;
    uint8_t j;
    uint8_t rel;
    int8_t c;
};

struct CandList {
    Cand c[PR_MAX_RULES];
    int n;
};

Cand make_cand(int kind, int i, int j, int rel, int c)
{
    Cand k;

    k.kind = (uint8_t)kind;
    k.i = (uint8_t)i;
    k.j = (uint8_t)j;
    k.rel = (uint8_t)rel;
    k.c = (int8_t)c;
    return k;
}

int digit_sum(const int d[DIGITS])
{
    return d[0] + d[1] + d[2] + d[3];
}

bool cand_holds(const Cand &k, const int d[DIGITS])
{
    switch (k.kind) {
    case C_PIN:    return d[k.i] == k.c;
    case C_LIN:    return d[k.i] == d[k.j] + k.c;
    case C_MULC:   return d[k.i] == d[k.j] * k.c;
    case C_SUMP:   return d[k.i] + d[k.j] == k.c;
    case C_DIFF:   return d[k.i] - d[k.j] == k.c;
    case C_PRODP:  return d[k.i] * d[k.j] == k.c;
    case C_SUM:    return digit_sum(d) == k.c;
    case C_CMP:    return rel_holds(d[k.i], k.rel, d[k.j]);
    case C_PAR:    return (d[k.i] & 1) == k.c;
    case C_ALLD:   return all_distinct(d);
    case C_CREL:   return rel_holds(d[k.i], k.rel, k.c);
    default:       return rel_holds(digit_sum(d), k.rel, k.c);      /* C_SUMREL */
    }
}

/* The rule in the rule language (puzzle_rules.h). */
void cand_render(const Cand &k, char *out, size_t size)
{
    const int i = k.i + 1;
    const int j = k.j + 1;

    switch (k.kind) {
    case C_PIN:
        snprintf(out, size, "d%d=%d", i, k.c);
        break;
    case C_LIN:
        if (k.c == 0) {
            snprintf(out, size, "d%d=d%d", i, j);
        } else {
            snprintf(out, size, "d%d=d%d%c%d", i, j, k.c > 0 ? '+' : '-', k.c > 0 ? k.c : -k.c);
        }
        break;
    case C_MULC:
        snprintf(out, size, "d%d=d%d*%d", i, j, k.c);
        break;
    case C_SUMP:
        snprintf(out, size, "d%d+d%d=%d", i, j, k.c);
        break;
    case C_DIFF:
        snprintf(out, size, "d%d-d%d=%d", i, j, k.c);
        break;
    case C_PRODP:
        snprintf(out, size, "d%d*d%d=%d", i, j, k.c);
        break;
    case C_SUM:
        snprintf(out, size, "sum=%d", k.c);
        break;
    case C_CMP:
        snprintf(out, size, "d%d%sd%d", i, rel_text(k.rel), j);
        break;
    case C_PAR:
        snprintf(out, size, "%s(d%d)", k.c == 1 ? "odd" : "even", i);
        break;
    case C_ALLD:
        snprintf(out, size, "alldiff");
        break;
    case C_CREL:
        snprintf(out, size, "d%d%s%d", i, rel_text(k.rel), k.c);
        break;
    default:
        snprintf(out, size, "sum%s%d", rel_text(k.rel), k.c);
        break;
    }
}

/* ---- makers: each returns a rule true at d ---- */

Cand make_link(const int d[DIGITS], int target, int src, Rng *rng)
{
    /* one unknown (target) once src is known: an equation that pins it */
    const bool mul_ok = d[src] >= 1 && d[target] >= 2 * d[src] && (d[target] % d[src]) == 0;

    switch (rng_below(rng, mul_ok ? 4 : 3)) {  /* mul only when it divides exactly */
    case 0:
        return make_cand(C_LIN, target, src, 0, d[target] - d[src]);
    case 1:
        return make_cand(C_SUMP, target, src, 0, d[target] + d[src]);
    case 2:
        return d[target] >= d[src] ? make_cand(C_DIFF, target, src, 0, d[target] - d[src])
                                   : make_cand(C_DIFF, src, target, 0, d[src] - d[target]);
    default:
        return make_cand(C_MULC, target, src, 0, d[target] / d[src]);
    }
}

Cand make_diff(const int d[DIGITS], int i, int j)
{
    return d[i] >= d[j] ? make_cand(C_DIFF, i, j, 0, d[i] - d[j]) : make_cand(C_DIFF, j, i, 0, d[j] - d[i]);
}

bool make_cmp(const int d[DIGITS], int i, int j, Cand *out)
{
    if (d[i] == d[j]) {
        return false;
    }
    *out = make_cand(C_CMP, i, j, d[i] < d[j] ? PR_REL_LT : PR_REL_GT, 0);
    return true;
}

/* d[i] against a constant that is NOT the digit itself (a bound or an exclusion). */
bool make_crel(const int d[DIGITS], int i, Rng *rng, Cand *out)
{
    const int v = d[i];
    const int pick = rng_below(rng, 3);

    if (pick == 0 && v < DIGIT_VALUES - 1) {
        *out = make_cand(C_CREL, i, 0, PR_REL_LT, v + 1 + rng_below(rng, DIGIT_VALUES - 1 - v));
    } else if (pick == 1 && v > 0) {
        *out = make_cand(C_CREL, i, 0, PR_REL_GT, v - 1 - rng_below(rng, v));
    } else {
        int c = rng_below(rng, DIGIT_VALUES);

        if (c == v) {
            c = (c + 1) % DIGIT_VALUES;
        }
        *out = make_cand(C_CREL, i, 0, PR_REL_NE, c);
    }
    return true;
}

Cand make_sumrel(const int d[DIGITS], Rng *rng)
{
    const int s = digit_sum(d);

    if (rng_below(rng, 2) == 0 && s < SUM_MAX_VALUE) {
        return make_cand(C_SUMREL, 0, 0, PR_REL_LT, s + 1 + rng_below(rng, SUM_MAX_VALUE - s));
    }
    if (s > 0) {
        return make_cand(C_SUMREL, 0, 0, PR_REL_GT, s - 1 - rng_below(rng, s));
    }
    return make_cand(C_SUMREL, 0, 0, PR_REL_LT, 1 + rng_below(rng, SUM_MAX_VALUE));
}

void join_rules(const CandList &cl, char *rules, size_t size)
{
    size_t len = 0u;
    char piece[RULE_PIECE_MAX];

    rules[0] = '\0';
    for (int i = 0; i < cl.n; i++) {
        cand_render(cl.c[i], piece, sizeof(piece));
        if ((i > 0 && !append_str(rules, size, &len, ";")) || !append_str(rules, size, &len, piece)) {
            rules[0] = '\0';
            return;
        }
    }
}

/* Solutions of the structured rules over all 10,000 codes, counting stops at `limit`. */
int cands_count(const CandList &cl, int limit)
{
    int count = 0;
    int d[DIGITS];

    for (int code = 0; code < PR_DIAL_CODES; code++) {
        bool ok = true;

        digits_from_code(code, d);
        for (int i = 0; i < cl.n && ok; i++) {
            ok = cand_holds(cl.c[i], d);
        }
        if (ok && ++count >= limit) {
            break;
        }
    }
    return count;
}

void shuffle_cands(CandList *cl, Rng *rng)
{
    for (int i = cl->n - 1; i > 0; i--) {
        const int j = rng_below(rng, i + 1);
        const Cand t = cl->c[i];

        cl->c[i] = cl->c[j];
        cl->c[j] = t;
    }
}

void random_order(int order[DIGITS], Rng *rng)
{
    for (int i = 0; i < DIGITS; i++) {
        order[i] = i;
    }
    for (int i = DIGITS - 1; i > 0; i--) {
        const int j = rng_below(rng, i + 1);
        const int t = order[i];

        order[i] = order[j];
        order[j] = t;
    }
}

/* ---- recipes ---- */

/* EASY: a chain. One digit is given, every other digit is pinned by a rule that has a single unknown
 * once an earlier digit is known; sometimes the last one comes from "sum = ..." instead. */
void build_easy(const int d[DIGITS], Rng *rng, CandList *cl)
{
    int order[DIGITS];

    random_order(order, rng);
    cl->n = 0;
    cl->c[cl->n++] = make_cand(C_PIN, order[0], 0, PR_REL_EQ, d[order[0]]);
    for (int t = 1; t < DIGITS; t++) {
        if (t == DIGITS - 1 && rng_below(rng, 3) == 0) {
            cl->c[cl->n++] = make_cand(C_SUM, 0, 0, PR_REL_EQ, digit_sum(d));
        } else {
            cl->c[cl->n++] = make_link(d, order[t], order[rng_below(rng, t)], rng);
        }
    }
    if (rng_below(rng, 2) == 0) {
        shuffle_cands(cl, rng);
    }
}

/* Two rules over the digits x, y that together have exactly one (x, y). */
bool pair_rules(const int d[DIGITS], int x, int y, Rng *rng, Cand *first, Cand *second)
{
    for (int attempt = 0; attempt < NORMAL_PAIR_TRIES; attempt++) {
        Cand k[2];

        for (int s = 0; s < 2; s++) {
            switch (rng_below(rng, 5)) {
            case 0:  k[s] = make_cand(C_SUMP, x, y, 0, d[x] + d[y]); break;
            case 1:  k[s] = make_diff(d, x, y); break;
            case 2:  k[s] = make_cand(C_PRODP, x, y, 0, d[x] * d[y]); break;
            case 3:  k[s] = make_cand(C_LIN, x, y, 0, d[x] - d[y]); break;
            default: k[s] = make_cand(C_LIN, y, x, 0, d[y] - d[x]); break;
            }
        }
        if (k[0].kind == k[1].kind && k[0].i == k[1].i) {
            continue;
        }
        int hits = 0;
        int t[DIGITS] = { d[0], d[1], d[2], d[3] };

        for (int a = 0; a < DIGIT_VALUES && hits < 2; a++) {
            for (int b = 0; b < DIGIT_VALUES && hits < 2; b++) {
                t[x] = a;
                t[y] = b;
                if (cand_holds(k[0], t) && cand_holds(k[1], t)) {
                    hits++;
                }
            }
        }
        if (hits == 1) {
            *first = k[0];
            *second = k[1];
            return true;
        }
    }
    return false;
}

/* NORMAL: rules that have to be solved in pairs. Three shapes: two solved pairs; a solved pair and
 * a chain; a solved pair, a related second pair and the sum. */
bool build_normal(const int d[DIGITS], Rng *rng, CandList *cl)
{
    static const uint8_t k_pairing[3][4] = { { 0, 1, 2, 3 }, { 0, 2, 1, 3 }, { 0, 3, 1, 2 } };
    const uint8_t *pr = k_pairing[rng_below(rng, 3)];
    const int x = pr[0];
    const int y = pr[1];
    const int z = pr[2];
    const int u = pr[3];
    const int shape = rng_below(rng, 3);
    Cand a;
    Cand b;

    cl->n = 0;
    if (!pair_rules(d, x, y, rng, &a, &b)) {
        return false;
    }
    cl->c[cl->n++] = a;
    cl->c[cl->n++] = b;
    if (shape == 0) {
        if (!pair_rules(d, z, u, rng, &a, &b)) {
            return false;
        }
        cl->c[cl->n++] = a;
        cl->c[cl->n++] = b;
    } else if (shape == 1) {
        cl->c[cl->n++] = make_link(d, z, rng_below(rng, 2) == 0 ? x : y, rng);
        cl->c[cl->n++] = make_link(d, u, rng_below(rng, 2) == 0 ? z : x, rng);
    } else {
        cl->c[cl->n++] = make_cand(C_SUM, 0, 0, PR_REL_EQ, digit_sum(d));
        cl->c[cl->n++] = rng_below(rng, 2) == 0 ? make_diff(d, z, u) : make_cand(C_LIN, z, u, 0, d[z] - d[u]);
    }
    if (rng_below(rng, 2) == 0) {
        shuffle_cands(cl, rng);
    }
    return cands_count(*cl, 2) == 1;
}

unsigned pair_bit(int i, int j)
{
    return 1u << (i * DIGITS + j);
}

/* HARD: no pinned digit and no solved pair; global rules (sum, alldiff), pair relations and bounds
 * are added one at a time until the set has exactly one solution. The rating decides afterwards. */
bool build_hard(const int d[DIGITS], Rng *rng, CandList *cl)
{
    unsigned pairs_used = 0u;
    bool have_sum = false;
    bool have_alld = false;
    const bool distinct = all_distinct(d);

    cl->n = 0;
    for (int guard = 0; guard < HARD_ADD_GUARD && cl->n < PR_MAX_RULES; guard++) {
        Cand k;
        bool ok = false;

        for (int attempt = 0; attempt < HARD_PICK_ATTEMPTS && !ok; attempt++) {
            const int roll = rng_below(rng, 100);
            const int i = rng_below(rng, DIGITS);
            int j = rng_below(rng, DIGITS - 1);
            unsigned bit;

            j = j >= i ? j + 1 : j;
            bit = pair_bit(i < j ? i : j, i < j ? j : i);
            if (roll < 16) {
                ok = !have_sum && digit_sum(d) > SUM_TRIVIAL && digit_sum(d) < SUM_MAX_VALUE - SUM_TRIVIAL;
                          /* "Sum 0" or "Sum 36" would give the whole code away to a human */
                k = make_cand(C_SUM, 0, 0, PR_REL_EQ, digit_sum(d));
            } else if (roll < 26) {
                ok = distinct && !have_alld;
                k = make_cand(C_ALLD, 0, 0, 0, 0);
            } else if (roll < 44) {
                ok = (pairs_used & bit) == 0u;
                k = make_cand(C_SUMP, i, j, 0, d[i] + d[j]);
            } else if (roll < 62) {
                ok = (pairs_used & bit) == 0u;
                k = make_diff(d, i, j);
            } else if (roll < 70) {
                ok = (pairs_used & bit) == 0u;
                k = make_cand(C_PRODP, i, j, 0, d[i] * d[j]);
            } else if (roll < 80) {
                ok = (pairs_used & bit) == 0u && make_cmp(d, i, j, &k);
            } else if (roll < 87) {
                ok = true;
                k = make_cand(C_PAR, i, 0, 0, d[i] & 1);
            } else if (roll < 93) {
                ok = make_crel(d, i, rng, &k);
            } else if (roll < 97) {
                ok = true;
                k = make_sumrel(d, rng);
            } else {
                ok = (pairs_used & bit) == 0u;
                k = make_cand(C_LIN, i, j, 0, d[i] - d[j]);
            }
            if (ok && (k.kind == C_SUMP || k.kind == C_DIFF || k.kind == C_PRODP || k.kind == C_CMP || k.kind == C_LIN)) {
                pairs_used |= pair_bit(k.i < k.j ? k.i : k.j, k.i < k.j ? k.j : k.i);
            }
        }
        if (!ok) {
            continue;
        }
        have_sum = have_sum || k.kind == C_SUM;
        have_alld = have_alld || k.kind == C_ALLD;
        cl->c[cl->n++] = k;
        if (cl->n >= 3 && cands_count(*cl, 2) == 1) {
            return true;
        }
    }
    return false;
}


}  // namespace

namespace bank_int {

bool dial_try_rules(int level, const uint8_t code[DIGITS], Rng *rng, char *rules, size_t size)
{
    CandList cl;
    int d[DIGITS];
    bool built;

    digits_from_code(code[0] * 1000 + code[1] * 100 + code[2] * 10 + code[3], d);
    memset(&cl, 0, sizeof(cl));
    if (level == PR_EASY) {
        build_easy(d, rng, &cl);
        built = true;
    } else if (level == PR_NORMAL) {
        built = build_normal(d, rng, &cl);
    } else {
        built = build_hard(d, rng, &cl);
    }
    if (!built) {
        return false;
    }
    join_rules(cl, rules, size);
    return rules[0] != '\0';
}

}  // namespace bank_int
