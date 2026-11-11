/*
 * Defuse to Wake - procedural puzzle bank (see puzzle_bank.h): the try loop, the conversion between
 * display text and rules, the acceptance check and the template fallback. The random rule sets
 * themselves come from puzzle_bank_dial.cpp and puzzle_bank_wire.cpp.
 *
 * RTOS note: runs in the network task (lowest priority) for each bank slot refill, never at
 * alarm time. The work is bounded: at most BANK_TRIES_DIAL dial tries (each a few brute-force passes
 * over the 10,000 codes) or BANK_TRIES_WIRE wire tries (<= 6 wires, trivial), and the optional
 * yield hook lets the caller give the CPU back every BANK_YIELD_EVERY tries so the idle task's
 * watchdog is never starved. No recursion, no heap, fixed arrays.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puzzle_bank_int.h"
#include "puzzle_rules.h"
#include "puzzle_gate.h"

#define WIRE_HARD_MIN_WIRES 4           /* three rules that are all needed cannot exist with only 3 wires */
#define TEMPLATE_VARIANTS   PUZZLE_VARIANTS
#define SEED_WIRE_XOR       0x9E3779B9u /* the wire of a set pair gets a different seed than its dial */

using namespace bank_int;

namespace {


int code_of(const uint8_t code[DIGITS])
{
    return code[0] * 1000 + code[1] * 100 + code[2] * 10 + code[3];
}

/* ======================================================================================== */
/* Display text <-> rule language                                                           */
/* ======================================================================================== */

const char *const k_dial_ord[DIGITS] = { "1st", "2nd", "3rd", "4th" };

bool emit(char *out, size_t size, size_t *len, const char *s)
{
    return append_str(out, size, len, s);
}

bool emit_ch(char *out, size_t size, size_t *len, char c)
{
    char s[2] = { c, '\0' };

    return append_str(out, size, len, s);
}

/* "sum=17" at the start of a rule, digits only after the '=': shown as "Sum 17" (the old bank's look). */
bool is_sum_equals_number(const char *p)
{
    if (strncmp(p, "sum=", 4) != 0 || p[4] < '0' || p[4] > '9') {
        return false;
    }
    for (p += 4; *p != '\0' && *p != ';'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

bool dial_rules_to_text(const char *rules, char *out, size_t size)
{
    size_t len = 0u;
    bool rule_start = true;
    bool ok = true;

    out[0] = '\0';
    for (const char *p = rules; *p != '\0' && ok;) {
        if (rule_start && is_sum_equals_number(p)) {
            ok = emit(out, size, &len, "Sum ");
            p += 4;
        } else if (*p == ';') {
            ok = emit(out, size, &len, "; ");
            p++;
            rule_start = true;
            continue;
        } else if (strncmp(p, "alldiff", 7) == 0) {
            ok = emit(out, size, &len, "All differ");
            p += 7;
        } else if (strncmp(p, "sum", 3) == 0) {
            ok = emit(out, size, &len, "Sum");
            p += 3;
        } else if (*p == 'd' && p[1] >= '1' && p[1] <= '4') {
            ok = emit(out, size, &len, k_dial_ord[p[1] - '1']);
            p += 2;
        } else {
            ok = emit_ch(out, size, &len, *p);
            p++;
        }
        rule_start = false;
    }
    return ok;
}

bool dial_text_to_rules(const char *text, char *out, size_t size)
{
    size_t len = 0u;
    bool ok = true;

    out[0] = '\0';
    for (const char *p = text; *p != '\0' && ok;) {
        int ord = -1;

        for (int i = 0; i < DIGITS; i++) {
            if (strncmp(p, k_dial_ord[i], 3) == 0) {
                ord = i;
            }
        }
        if (strncmp(p, "All differ", 10) == 0) {
            ok = emit(out, size, &len, "alldiff");
            p += 10;
        } else if (strncmp(p, "Sum", 3) == 0) {
            if (p[3] == ' ' && p[4] >= '0' && p[4] <= '9') {
                ok = emit(out, size, &len, "sum=");
                p += 4;
            } else {
                ok = emit(out, size, &len, "sum");
                p += 3;
            }
        } else if (ord >= 0) {
            const char name[3] = { 'd', (char)('1' + ord), '\0' };

            ok = emit(out, size, &len, name);
            p += 3;
        } else {
            ok = emit_ch(out, size, &len, *p);
            p++;
        }
    }
    return ok;
}

bool wire_rules_to_text(const char *rules, char *out, size_t size)
{
    size_t len = 0u;
    bool ok = emit(out, size, &len, BANK_WIRE_PREFIX);

    for (const char *p = rules; *p != '\0' && ok; p++) {
        ok = (*p == ';') ? emit(out, size, &len, "; ") : emit_ch(out, size, &len, *p);
    }
    return ok;
}

bool wire_text_to_rules(const char *text, char *out, size_t size)
{
    const size_t prefix = strlen(BANK_WIRE_PREFIX);
    size_t len = 0u;

    out[0] = '\0';
    if (strncmp(text, BANK_WIRE_PREFIX, prefix) != 0) {
        return false;
    }
    return emit(out, size, &len, text + prefix);
}

/* ======================================================================================== */
/* Acceptance of one candidate (shared by dial and wire)                                    */
/* ======================================================================================== */

/* A bank text is only ever returned when ALL of this holds: it passes validate_puzzle (O3 length, no
 * leaked code or wire number, O4), fits its OLED rows after word wrap (no marquee), and converts back
 * to rules with exactly one solution equal to the answer. *measured gets the rating of those rules. */
bool proven_text(const char *text, int kind, int wire_count, int answer, int *measured)
{
    char answer_text[PROTO_DIAL_DIGITS + 1];

    if (kind == PUZZLE_DIAL) {
        snprintf(answer_text, sizeof(answer_text), "%04d", answer);
    } else {
        snprintf(answer_text, sizeof(answer_text), "%d", answer);
    }
    if (!validate_puzzle(text, kind, answer_text) || !puzzle_text_fits_oled(text, kind)) {
        *measured = PR_LEVEL_NONE;
        return false;
    }
    return bank_recheck(text, kind, wire_count, answer, measured);
}

/* proven_text, and rated at exactly the requested level. */
bool accept_text(const char *text, int kind, int wire_count, int answer, int level, int *measured)
{
    return proven_text(text, kind, wire_count, answer, measured) && *measured == level;
}

void store_result(BankResult *out, const char *text, int level, bool fallback, int tries)
{
    snprintf(out->text, sizeof(out->text), "%s", text);
    out->level = (uint8_t)level;
    out->fallback = fallback ? 1u : 0u;
    out->tries = (uint16_t)tries;
}


/* ======================================================================================== */
/* Generation: try loop and template fallback                                               */
/* ======================================================================================== */

/* The old template families, the one closest to the level first. Each text is converted back to rules and
 * rated like a generated one; the first with the requested level wins, otherwise the closest level. */
void dial_fallback(const BankRequest &req, Rng *rng, BankResult *out, int tries)
{
    static const uint8_t k_order[3][DIAL_FAMILIES] = {
        { 0, 1, 2, 3 },     /* EASY: chains first */
        { 3, 2, 1, 0 },     /* NORMAL: pair sums and differences first */
        { 2, 3, 1, 0 }      /* HARD: pivot + sum first */
    };
    const uint8_t *order = k_order[req.level - PR_EASY];
    const int answer = code_of(req.code);
    const unsigned details = TEMPLATE_VARIANTS / DIAL_FAMILIES;
    const unsigned start = (unsigned)rng_below(rng, (int)details);
    int best = -1;
    int best_distance = 100;
    char text[PUZZLE_TEXT_BUF];

    out->text[0] = '\0';
    for (unsigned f = 0u; f < DIAL_FAMILIES && best_distance > 0; f++) {
        for (unsigned s = 0u; s < details && best_distance > 0; s++) {
            const unsigned variant = order[f] + DIAL_FAMILIES * ((start + s) % details);
            int measured = 0;

            if (dial_template(req.code, variant, text, sizeof(text)) == 0u ||
                !proven_text(text, PUZZLE_DIAL, 0, answer, &measured)) {
                continue;                           /* the old templates get the same checks as generated texts */
            }
            const int distance = measured > req.level ? measured - req.level : req.level - measured;

            if (distance < best_distance) {
                best_distance = distance;
                best = measured;
                store_result(out, text, measured, true, tries);
            }
        }
    }
    if (best >= 0) {
        return;
    }
    /* no old template passed (e.g. its text wraps past 3 rows): a generated EASY chain, which always works */
    for (int t = 0; t < BANK_TRIES_DIAL; t++) {
        char rules[BANK_RULES_MAX];
        int measured = 0;

        if (dial_try_rules(PR_EASY, req.code, rng, rules, sizeof(rules)) &&
            bank_rules_to_text(rules, PUZZLE_DIAL, text, sizeof(text)) &&
            accept_text(text, PUZZLE_DIAL, 0, answer, PR_EASY, &measured)) {
            store_result(out, text, measured, true, tries);
            return;
        }
    }
    (void)dial_template(req.code, 0u, text, sizeof(text));     /* cannot happen (host-tested): never return empty */
    store_result(out, text, 0, true, tries);
}

void generate_dial(const BankRequest &req, Rng *rng, BankResult *out)
{
    char rules[BANK_RULES_MAX];
    char text[PUZZLE_TEXT_BUF];
    const int answer = code_of(req.code);
    const int limit = req.max_tries != 0u ? (int)req.max_tries : BANK_TRIES_DIAL;
    int measured = 0;
    int tries = 0;

    while (tries < limit) {
        tries++;
        if (dial_try_rules(req.level, req.code, rng, rules, sizeof(rules)) &&
            bank_rules_to_text(rules, PUZZLE_DIAL, text, sizeof(text)) &&
            accept_text(text, PUZZLE_DIAL, 0, answer, req.level, &measured)) {
            store_result(out, text, measured, false, tries);
            return;
        }
        if (req.yield != NULL && tries % BANK_YIELD_EVERY == 0) {
            req.yield();
        }
    }
    dial_fallback(req, rng, out, tries);
}

/* The procedural search at the requested level failed (or cannot exist: HARD with 3 wires). The
 * answer is still delivered as a PROVEN text, one level easier: first the EASY recipe (a single rule
 * that pins the wire, tried with its own budget), then a fixed rule "w+c=k+c" that cannot name the wire
 * (c and k+c are never k) and is proven and rated like any other. The old wire templates are not
 * used: they name the wire ("Cut the 3rd from the left") and have no rule form to prove. */
void wire_fallback(const BankRequest &req, Rng *rng, BankResult *out, int tries)
{
    uint16_t pool[BANK_WIRE_POOL_MAX];
    char rules[BANK_RULES_MAX];
    char text[PUZZLE_TEXT_BUF];
    const int pool_n = wire_pool(PR_EASY, req.wire_count, req.wire_k, pool);
    int measured = 0;

    for (int t = 0; t < BANK_TRIES_WIRE; t++) {
        if (wire_try_rules(PR_EASY, req.wire_count, req.wire_k, pool, pool_n, rng, rules, sizeof(rules)) &&
            bank_rules_to_text(rules, PUZZLE_WIRE, text, sizeof(text)) &&
            accept_text(text, PUZZLE_WIRE, req.wire_count, req.wire_k, PR_EASY, &measured)) {
            store_result(out, text, measured, true, tries);
            return;
        }
    }
    snprintf(rules, sizeof(rules), "w+%d=%d", req.wire_k == 1 ? 2 : 1, req.wire_k + (req.wire_k == 1 ? 2 : 1));
    (void)bank_rules_to_text(rules, PUZZLE_WIRE, text, sizeof(text));
    (void)proven_text(text, PUZZLE_WIRE, req.wire_count, req.wire_k, &measured);    /* measured level, 0 if it ever failed */
    store_result(out, text, measured, true, tries);
}

void generate_wire(const BankRequest &req, Rng *rng, BankResult *out)
{
    uint16_t pool[BANK_WIRE_POOL_MAX];
    const int limit = req.max_tries != 0u ? (int)req.max_tries : BANK_TRIES_WIRE;
    char rules[BANK_RULES_MAX];
    char text[PUZZLE_TEXT_BUF];
    int measured = 0;
    int tries = 0;
    int pool_n;

    if (req.level == PR_HARD && req.wire_count < WIRE_HARD_MIN_WIRES) {
        wire_fallback(req, rng, out, 0);            /* impossible: with 3 wires two rules always suffice */
        return;
    }
    pool_n = wire_pool(req.level, req.wire_count, req.wire_k, pool);
    while (tries < limit) {
        tries++;
        if (wire_try_rules(req.level, req.wire_count, req.wire_k, pool, pool_n, rng, rules, sizeof(rules)) &&
            bank_rules_to_text(rules, PUZZLE_WIRE, text, sizeof(text)) &&
            accept_text(text, PUZZLE_WIRE, req.wire_count, req.wire_k, req.level, &measured)) {
            store_result(out, text, measured, false, tries);
            return;
        }
        if (req.yield != NULL && tries % BANK_YIELD_EVERY == 0) {
            req.yield();
        }
    }
    wire_fallback(req, rng, out, tries);
}

}  // namespace

/* ======================================================================================== */
/* Public                                                                                   */
/* ======================================================================================== */

bool bank_rules_to_text(const char *rules, int kind, char *text, size_t text_size)
{
    if (rules == NULL || text == NULL || text_size == 0u) {
        return false;
    }
    if (kind == PUZZLE_DIAL) {
        return dial_rules_to_text(rules, text, text_size);
    }
    return kind == PUZZLE_WIRE && wire_rules_to_text(rules, text, text_size);
}

bool bank_text_to_rules(const char *text, int kind, char *rules, size_t rules_size)
{
    if (text == NULL || rules == NULL || rules_size == 0u) {
        return false;
    }
    if (kind == PUZZLE_DIAL) {
        return dial_text_to_rules(text, rules, rules_size);
    }
    return kind == PUZZLE_WIRE && wire_text_to_rules(text, rules, rules_size);
}

bool bank_recheck(const char *text, int kind, int wire_count, int answer, int *level)
{
    char rules[BANK_RULES_MAX];
    PrCheck chk;

    if (level != NULL) {
        *level = PR_LEVEL_NONE;
    }
    if (!bank_text_to_rules(text, kind, rules, sizeof(rules))) {
        return false;
    }
    pr_check(rules, kind == PUZZLE_DIAL ? PR_KIND_DIAL : PR_KIND_WIRE, wire_count, answer, &chk);
    if (level != NULL) {
        *level = chk.level;
    }
    return chk.error == PR_OK && chk.matches_answer != 0;
}

void bank_generate(const BankRequest &req, BankResult *out)
{
    Rng rng;

    memset(out, 0, sizeof(*out));
    rng_seed(&rng, req.seed);
    if (req.kind == PUZZLE_WIRE) {
        BankRequest r = req;

        r.wire_count = req.wire_count < (int)PR_WIRE_MIN ? (int)PR_WIRE_MIN
                                                          : (req.wire_count > (int)PR_WIRE_MAX ? (int)PR_WIRE_MAX : req.wire_count);
        r.wire_k = req.wire_k < 1 ? 1 : (req.wire_k > r.wire_count ? r.wire_count : req.wire_k);
        r.level = (req.level < PR_EASY || req.level > PR_HARD) ? PR_NORMAL : req.level;
        generate_wire(r, &rng, out);
        return;
    }
    BankRequest r = req;

    r.level = (req.level < PR_EASY || req.level > PR_HARD) ? PR_NORMAL : req.level;
    for (int i = 0; i < DIGITS; i++) {
        r.code[i] = (uint8_t)(req.code[i] % DIGIT_VALUES);
    }
    generate_dial(r, &rng, out);
}

void bank_fill_set(PuzzleSet *s, const uint32_t rnd[PUZZLE_BANK_WORDS], int level)
{
    memset(s, 0, sizeof(*s));
    s->magic = PUZZLE_MAGIC;
    s->level = (uint8_t)level;
    for (unsigned i = 0u; i < PUZZLE_PAIRS; i++) {
        BankRequest req;
        BankResult res;

        memset(&req, 0, sizeof(req));
        req.level = level;
        req.kind = PUZZLE_DIAL;
        dial_code_from_random(rnd[3u * i], req.code);
        req.seed = rnd[3u * i + 2u];
        bank_generate(req, &res);
        Puzzle *dp = &s->dial[i];
        dp->kind = PUZZLE_DIAL;
        dp->source = PUZZLE_SRC_BANK;
        memcpy(dp->dial_code, req.code, sizeof(dp->dial_code));
        snprintf(dp->text, sizeof(dp->text), "%s", res.text);
        dp->level = res.level;

        memset(&req, 0, sizeof(req));
        req.level = level;
        req.kind = PUZZLE_WIRE;
        wire_pick_from_random(level, rnd[3u * i + 1u], &req.wire_count, &req.wire_k);
        req.seed = rnd[3u * i + 2u] ^ SEED_WIRE_XOR;
        bank_generate(req, &res);
        Puzzle *wp = &s->wire[i];
        wp->kind = PUZZLE_WIRE;
        wp->source = PUZZLE_SRC_BANK;
        wp->wire_count = (uint8_t)req.wire_count;
        wp->wire_k = (uint8_t)req.wire_k;
        snprintf(wp->text, sizeof(wp->text), "%s", res.text);
        wp->level = res.level;
    }
}
