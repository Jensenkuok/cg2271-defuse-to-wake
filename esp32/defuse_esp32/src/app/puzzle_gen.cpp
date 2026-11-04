/* Defuse to Wake - puzzle slots: bank refill of spent pairs, continuous AI upgrade. See puzzle_gen.h. */
#include "puzzle_gen.h"

#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#include "config.h"
#include "flash_store.h"
#include "llm_client.h"
#include "mem_big.h"
#include "puzzle_bank.h"
#include "puzzle_gate.h"
#include "puzzle_rules.h"

namespace {

#define SPENT_MASK_ALL      ((1u << PUZZLE_PAIRS) - 1u)
#define SET_LOCK_WAIT_MS    100u
#define MS_PER_S            1000u
#define RETRY_S_MAX         65535u
#define SYS_PROMPT_MAX      1600        /* the setter prompts (grammar + examples) are ~1.1 KB; PSRAM, see puzzle_gen_init */
#define USER_PROMPT_MAX     96
#define LATENCY_MAX_MS      65535u
#define BANK_SLOW_LOG_MS    1000u       /* a bank puzzle slower than this is worth a log line */

/* ---- Prompts. The riddle answer is given by the ESP32; the checker never sees it. The setter writes
 * the riddle AND the same facts as rules in the puzzle_rules.h language; the ESP32 proves the rules
 * (one solution = our answer) and rates them, so the level is computed, never taken from the LLM. ---- */
const char kSetterDialFmt[] =
    "You write one logic puzzle for a bomb-defusal game. The user gives a secret 4-digit code (d1 is the first digit). "
    "Requested level: %s. "
    "Reply ONLY with JSON: {\"puzzle\":\"<riddle>\",\"rules\":\"<rules>\"}. "
    "RULES use this language, rules separated by ';' (at most 6): terms d1 d2 d3 d4 (digits 0-9), sum (= d1+d2+d3+d4), "
    "whole numbers 0-99; operators + - *; relations = != < > <= >=; odd(x) even(x); alldiff (all four digits differ). "
    "The rules must have exactly ONE solution: the secret code. "
    "PUZZLE: the same facts as the rules, in words or short notation, at most 63 plain ASCII characters; write 1st 2nd 3rd 4th "
    "for d1 d2 d3 d4. Never write the code or list its digits. "
    "Levels: EASY = a chain, every rule has one unknown once the earlier digits are known, e.g. "
    "d1=3;d2=d1+4;d3=d2-5;d4=d1*3 . "
    "NORMAL = rules that must be solved in pairs, e.g. d1+d2=9;d1-d2=1;d3=2*d4;d3+d4=12 . "
    "HARD = needs case elimination (trying cases), e.g. sum=17;d4-d1=1;alldiff;d1-d2=4 . "
    "Write exactly the requested level.";
const char kSetterWireFmt[] =
    "You write one riddle for a bomb-defusal game. The user gives the number of wires n (numbered 1..n from the left), "
    "the wire w to cut and the requested level. Requested level: %s. "
    "Reply ONLY with JSON: {\"puzzle\":\"<riddle>\",\"rules\":\"<rules>\"}. "
    "RULES use this language, rules separated by ';' (at most 6): terms w (the wire), n (the wire count), "
    "whole numbers 0-99; operators + - *; relations = != < > <= >=; odd(x) even(x). "
    "The rules must have exactly ONE solution w in 1..n: the wire to cut. "
    "PUZZLE: the same facts as the rules, in words or short notation (w = wire number from the left, n = number of wires), "
    "at most 42 plain ASCII characters. Never write the wire number itself (no 3, no 3rd). "
    "Levels: EASY = one rule is enough, e.g. w=n-2 . "
    "NORMAL = two rules are needed together, neither is enough alone, e.g. odd(w);w>3 for n=5 . "
    "HARD = three rules are needed, no two are enough, e.g. even(w);w>3;w<6 for n=6 . "
    "Write exactly the requested level.";
const char kCheckerDialSys[] =
    "Solve the puzzle. The answer is a 4-digit code (digits 0-9, leading zeros allowed); 1st 2nd 3rd 4th are its digits "
    "from the left. "
    "Reply ONLY with JSON: {\"answer\":\"1234\"}";
const char kCheckerWireFmt[] =
    "Solve the puzzle. There are %d wires numbered 1..%d from the left; w is the wire to cut, n the number of wires. "
    "The answer is one wire number. "
    "Reply ONLY with JSON: {\"answer\":\"2\"}";

const char *const k_setter_chain[APP_LLM_CHAIN_LEN] = APP_LLM_SETTER_CHAIN;
const char *const k_checker_chain[APP_LLM_CHAIN_LEN] = APP_LLM_CHECKER_CHAIN;

enum Phase { PH_IDLE = 0, PH_NEXT, PH_ATTEMPT, PH_SETTER, PH_CHECKER };

/* The AI upgrade of ONE slot in progress (a bank puzzle being replaced by an LLM-written one). Only the network
 * task touches it. The set itself is not touched until the puzzle is verified: then it is copied in place. */
struct Gen {
    Phase phase;
    Puzzle cand;                        /* the AI puzzle being built */
    Puzzle fill;                        /* scratch for the bank refill (kept off the network task's stack) */
    uint32_t started_unix;
    uint8_t idx;                        /* slot: 0..PAIRS-1 dial, PAIRS..2*PAIRS-1 wire */
    uint8_t attempt;                    /* 1..APP_PUZZLE_ATTEMPTS */
    uint8_t level;                      /* requested level of this slot (PrLevel), fixed when the slot starts */
    uint8_t code[PROTO_DIAL_DIGITS];    /* the answer being worked on */
    int wire_n;
    int wire_k;
    char answer[PUZZLE_ANSWER_LEN];
    char text[PUZZLE_TEXT_BUF];         /* candidate riddle */
    char setter[PUZZLE_MODEL_LEN];
    uint16_t setter_ms;
    int setter_pos;
    int order[APP_LLM_CHAIN_LEN];       /* checker models in the order they are tried */
    int order_n;
    int checker_pos;
    bool chain_replied;                 /* some model of the current chain answered at all */
    uint8_t dead_chains;                /* chains in a row where no model answered */
    bool abort_ai;
};

uint8_t s_target_level = APP_PUZZLE_DEFAULT_LEVEL;  /* the level new puzzles ask for (network task only) */
SemaphoreHandle_t s_mutex;
PuzzleSet *s_set;                       /* the active set (PSRAM); guarded by s_mutex */
uint32_t s_set_gen;                     /* constant 1: per-slot changes happen IN PLACE and never bump it (R1), see puzzle_gen.h */

/* Spent pairs (D4). Written by the link task (consume) and the network task (refill), all guarded by s_mutex. */
uint8_t s_spent;                        /* bit per pair: a bomb started with it, the bank has not refilled it yet */
uint32_t s_spend_order[PUZZLE_PAIRS];   /* grows with every spend; 0 = unknown (after a reboot): D6 reuses the smallest */
uint32_t s_spend_counter;
uint8_t s_refilled;                     /* bit per SLOT: refilled by the bank since its pair was spent */
bool s_used_dirty;                      /* s_spent changed in RAM and is not in flash yet */
/* The pair bomb_config last took (the MCX holds its answers, the OLED shows its text: they are one send).
 * -1 = none (it started a bomb, or a D6 reuse took a spent pair). Guarded. */
int8_t s_reserved_pair = -1;
volatile bool s_reserved_dirty;         /* a slot of the reserved pair changed: it must be sent again. Set under the lock;
                                         * the link task reads it without one (a single byte) and clears it under the lock */

/* Network task only from here (the web task reads some of them as single words, see puzzle_gen_info). */
uint32_t s_runs_done;                   /* whole-set events finished (a manual redo or a level refill); only puzzle_gen_runs_done() reads it and nothing calls that now */
PuzzleSet s_blob;                       /* flash buffer */
bool s_in_session;                      /* a bomb session or the ring is running (set by puzzle_gen_apply_level every pass) */
uint8_t s_force_slots;                  /* bit per slot: upgrade it again even if it is AI already (manual redo) */
bool s_redo_active;                     /* a manual redo is in progress: count it in s_runs_done when s_force_slots empties */
int s_last_slot = -1;                   /* the slot the last upgrade worked on: round-robin continues after it */
bool s_wait_active;                     /* an upgrade attempt failed: no new one before s_next_upgrade_ms */
uint32_t s_next_upgrade_ms;
int s_lvl_cursor;                       /* level refill: next slot to look at */
bool s_lvl_announced;
uint32_t s_now_ms;

Gen *s_gen;                             /* PSRAM */
PuzzleSet *s_snap;                      /* PSRAM: scratch copy for puzzle_print_set (~1.1 KB, off the stack) */
char *s_reply;                          /* PSRAM, APP_LLM_REPLY_MAX bytes */
char *s_json;                           /* PSRAM, APP_LLM_REPLY_MAX bytes */
char *s_sys;                            /* PSRAM, SYS_PROMPT_MAX bytes */
char s_user[USER_PROMPT_MAX];

uint16_t clamp_ms(uint32_t ms)
{
    return (uint16_t)(ms > LATENCY_MAX_MS ? LATENCY_MAX_MS : ms);
}

void copy_str(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

void random_words(uint32_t *w, unsigned n)
{
    unsigned i;

    for (i = 0u; i < n; i++) {
        w[i] = esp_random();
    }
}

/* ================= printing ================= */
void print_puzzle(char tag, unsigned i, const Puzzle &p)
{
    char ans[PUZZLE_ANSWER_LEN];

    if (!puzzle_answer_text(p, ans, sizeof(ans))) {
        copy_str(ans, sizeof(ans), "?");
    }
    Serial.printf("[ai] %c%u %-4s %-6s %-10s setter=%s checker=%s said=%s tries=%u %ums/%ums ans=%s", tag, i,
                  p.source == PUZZLE_SRC_AI ? "AI" : "BANK",
                  p.level != 0u ? pr_level_name(p.level) : "-",
                  p.verified ? "verified" : (p.setter[0] != '\0' ? "rejected" : "unverified"),
                  p.setter[0] != '\0' ? p.setter : "-", p.checker[0] != '\0' ? p.checker : "-",
                  p.checker_answer[0] != '\0' ? p.checker_answer : "-", (unsigned)p.attempts, (unsigned)p.setter_ms,
                  (unsigned)p.checker_ms, ans);
    if (p.kind == PUZZLE_WIRE) {
        Serial.printf(" wires=%u", (unsigned)p.wire_count);
    }
    Serial.printf(" \"%s\"\n", p.text);
}

void print_set(const PuzzleSet &s, uint32_t gen)
{
    unsigned i;

    Serial.printf("[ai] set gen=%u: %u of %u puzzles AI-verified, generated at unix %u\n", (unsigned)gen,
                  (unsigned)s.ai_count, (unsigned)(2u * PUZZLE_PAIRS), (unsigned)s.generated_unix);
    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        print_puzzle('D', i, s.dial[i]);
    }
    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        print_puzzle('W', i, s.wire[i]);
    }
}

/* ================= storage ================= */
void save_spent(uint8_t spent)
{
    if (!flash_write_int(APP_KEY_PUZZLE_USED, (int32_t)spent)) {
        Serial.println("[ai] could not write the spent-pair mask to flash");
    }
}

void save_set(void)
{
    memset(static_cast<void *>(&s_blob), 0, sizeof(s_blob));   /* no uninitialised padding in flash */
    s_blob = *s_set;
    if (!flash_write_blob(APP_KEY_PUZZLES, &s_blob, sizeof(s_blob))) {
        Serial.println("[ai] could not write the puzzle set to flash");
    }
}

Puzzle &set_slot(PuzzleSet &s, int idx)
{
    return idx < PUZZLE_PAIRS ? s.dial[idx] : s.wire[idx - PUZZLE_PAIRS];
}

void slot_tag(int idx, char *buf, size_t size)
{
    snprintf(buf, size, "%c%u", idx < PUZZLE_PAIRS ? 'D' : 'W', (unsigned)(idx < PUZZLE_PAIRS ? idx : idx - PUZZLE_PAIRS));
}

/* Put ONE puzzle into the active set, in place (network task). The puzzle was built in a buffer no other task reads,
 * so the lock is held only for one ~140-byte copy: the link task, which reads the set to build a BOMB_CONFIG, never
 * waits for a flash write, an LLM call or the bank. s_set_gen is NOT bumped (R1: a new generation would make the
 * link task ignore the bomb's pair when it marks it spent).
 *
 * Why the reserved pair is re-sent (R2): the MCX holds the ANSWERS of the pair bomb_config last took and the OLED
 * shows its TEXT, both from one send. If a slot of that pair changed in the set only, the next bomb would still run
 * the old pair although the set holds a better (or, after a level change, the right) one. s_reserved_dirty makes
 * bomb_config send the whole pair again (it does so only while ARMED outside a session). For the same reason this
 * never changes the reserved pair while a session runs: dial and wire would then come from two different sends.
 *
 * for_spent: the bank refills a slot of a SPENT pair (allowed during a session, the pair is not reserved); every
 * other change skips a pair that was spent meanwhile (its old answer must never come back, D4). */
bool install_slot(int idx, const Puzzle &p, bool for_spent, uint32_t unix_now)
{
    const int pair = idx % PUZZLE_PAIRS;
    bool installed = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    if (!for_spent && ((s_spent >> pair) & 1u) != 0u) {
        /* spent meanwhile: the bank owns this slot now */
    } else if (!for_spent && s_in_session && pair == (int)s_reserved_pair) {
        /* never patch the pair the MCX holds during a session */
    } else {
        set_slot(*s_set, idx) = p;
        s_set->ai_count = puzzle_set_count_ai(*s_set);
        if (!for_spent && unix_now != 0u) {
            s_set->generated_unix = unix_now;
        }
        if (for_spent) {
            s_refilled = (uint8_t)(s_refilled | (1u << idx));
        }
        if (pair == (int)s_reserved_pair) {
            s_reserved_dirty = true;
        }
        installed = true;
    }
    xSemaphoreGive(s_mutex);
    if (installed) {
        save_set();                     /* flash writes stay in the network task */
    }
    return installed;
}

/* ================= bank refill (spent pairs, level change) ================= */
/* Lets the idle task (and its watchdog) run during the bank's compute loop: one tick every few tries. */
void bank_yield(void)
{
    vTaskDelay(1);
}

/* A new bank puzzle for one slot at `level`: the ESP32 picks the answer, the bank writes a proven text. Offline, CPU
 * only (network task, lowest priority; the link task preempts it). */
void fill_bank_slot(int idx, uint8_t level, Puzzle *p)
{
    BankRequest req;
    BankResult res;
    char tag[8];
    const bool is_dial = idx < PUZZLE_PAIRS;
    uint8_t code[PROTO_DIAL_DIGITS];
    int wire_n = 0;
    int wire_k = 0;
    const uint32_t t0 = millis();

    memset(&req, 0, sizeof(req));
    req.kind = is_dial ? PUZZLE_DIAL : PUZZLE_WIRE;
    if (is_dial) {
        dial_code_from_random(esp_random(), code);
        memcpy(req.code, code, sizeof(req.code));
    } else {
        wire_pick_from_random(level, esp_random(), &wire_n, &wire_k);   /* wire count of the level */
        req.wire_count = wire_n;
        req.wire_k = wire_k;
    }
    req.level = level;
    req.seed = esp_random();
    req.yield = bank_yield;
    bank_generate(req, &res);
    memset(p, 0, sizeof(*p));
    p->kind = (uint8_t)req.kind;
    if (is_dial) {
        memcpy(p->dial_code, code, sizeof(p->dial_code));
    } else {
        p->wire_count = (uint8_t)wire_n;
        p->wire_k = (uint8_t)wire_k;
    }
    copy_str(p->text, sizeof(p->text), res.text);
    p->source = PUZZLE_SRC_BANK;
    p->level = res.level;
    slot_tag(idx, tag, sizeof(tag));
    if (res.fallback != 0u) {
        Serial.printf("[ai] %s: no %s rule set found in %u tries: old template used (rated %s)\n", tag,
                      pr_level_name(level), (unsigned)res.tries, pr_level_name(res.level));
    }
    if (millis() - t0 > BANK_SLOW_LOG_MS) {
        Serial.printf("[ai] bank puzzle took %u ms (%u tries)\n", (unsigned)(millis() - t0), (unsigned)res.tries);
    }
}

/* One slot of a spent pair per pass (D4): the two slots come from the bank at the target level, the set is saved,
 * and only then the spent bit is cleared and written (a power cut in between refills once more, never brings back a
 * used puzzle). Runs offline and during a bomb session (R3): the bank needs no network and a spent pair is never the
 * reserved one. Returns true when it worked on a slot (the pass is used up). */
bool service_spent_refill(void)
{
    uint8_t spent = 0u;
    uint8_t refilled = 0u;
    uint8_t freed = 0u;
    int slot;
    int pair;
    char tag[8];

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    for (pair = 0; pair < PUZZLE_PAIRS; pair++) {           /* a pair whose two slots are both refilled is free again */
        const uint8_t both = (uint8_t)((1u << pair) | (1u << (PUZZLE_PAIRS + pair)));

        if (((s_spent >> pair) & 1u) != 0u && (s_refilled & both) == both) {
            s_spent = pair_clear_spent(s_spent, pair, PUZZLE_PAIRS);
            s_refilled = (uint8_t)(s_refilled & ~both);
            s_used_dirty = true;        /* written by puzzle_flush_used(), after the set (saved by install_slot) */
            freed = (uint8_t)(freed | (1u << pair));
        }
    }
    spent = s_spent;
    refilled = s_refilled;
    xSemaphoreGive(s_mutex);
    for (pair = 0; pair < PUZZLE_PAIRS; pair++) {
        if ((freed >> pair) & 1u) {
            Serial.printf("[ai] pair %d refilled from the bank: available again\n", pair);
        }
    }
    slot = refill_next_slot(spent, refilled);
    if (slot < 0) {
        return false;
    }
    fill_bank_slot(slot, s_target_level, &s_gen->fill);
    if (install_slot(slot, s_gen->fill, true, 0u)) {
        slot_tag(slot, tag, sizeof(tag));
        Serial.printf("[ai] %s of spent pair %d refilled from the bank (level %s)\n", tag, slot % PUZZLE_PAIRS,
                      pr_level_name(s_target_level));
    }
    return true;                        /* on a busy lock the slot is drawn again on the next pass */
}

/* Level of the active set. A set stored before PZL2 got its level byte has 0: it was built for NORMAL. */
uint8_t set_level(void)
{
    return s_set->level != 0u ? s_set->level : (uint8_t)PR_NORMAL;
}

/* After a level change (outside a session): every slot that is not for the target level and whose pair is not
 * spent is replaced from the bank, one slot per pass; then the set's level byte moves and the upgrades go on at the
 * new level. A slot already at the target level (for example an AI puzzle) is kept. Returns true when it worked. */
bool service_level_refill(void)
{
    int slot = s_lvl_cursor;

    if (s_in_session || set_level() == s_target_level) {
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    while (slot < PUZZLE_SLOTS &&
           (((s_spent >> (slot % PUZZLE_PAIRS)) & 1u) != 0u || set_slot(*s_set, slot).level == s_target_level)) {
        slot++;                         /* a spent pair is refilled at the target level anyway */
    }
    xSemaphoreGive(s_mutex);
    if (!s_lvl_announced) {
        s_lvl_announced = true;
        Serial.printf("[ai] level changed (set was built for %s, now %s): refilling the other slots from the bank\n",
                      pr_level_name(set_level()), pr_level_name(s_target_level));
    }
    if (slot < PUZZLE_SLOTS) {
        fill_bank_slot(slot, s_target_level, &s_gen->fill);
        if (install_slot(slot, s_gen->fill, false, 0u)) {
            s_lvl_cursor = slot + 1;
        } else {
            s_lvl_cursor = slot;        /* lock busy or the pair was spent meanwhile: look again from here */
        }
        return true;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    s_set->level = s_target_level;
    xSemaphoreGive(s_mutex);
    save_set();
    s_lvl_cursor = 0;
    s_lvl_announced = false;
    s_runs_done++;                      /* a whole-set event finished */
    Serial.printf("[ai] bank refill at level %s finished: %u of %u puzzles are AI\n", pr_level_name(s_target_level),
                  (unsigned)s_set->ai_count, (unsigned)PUZZLE_SLOTS);
    return true;
}

/* ================= AI upgrade of one slot (state machine) ================= */
bool cur_is_dial(void)
{
    return s_gen->idx < PUZZLE_PAIRS;
}

void cur_tag(char *buf, size_t size)
{
    snprintf(buf, size, "%c%u", cur_is_dial() ? 'D' : 'W',
             (unsigned)(cur_is_dial() ? s_gen->idx : s_gen->idx - PUZZLE_PAIRS));
}

void fill_target(Puzzle *p)
{
    memset(p, 0, sizeof(*p));
    p->kind = (uint8_t)(cur_is_dial() ? PUZZLE_DIAL : PUZZLE_WIRE);
    if (cur_is_dial()) {
        memcpy(p->dial_code, s_gen->code, sizeof(p->dial_code));
    } else {
        p->wire_count = (uint8_t)s_gen->wire_n;
        p->wire_k = (uint8_t)s_gen->wire_k;
    }
}

/* Start the AI upgrade of one slot at the current target level. The slot's bank puzzle stays in the set until a
 * verified AI puzzle replaces it; the ESP32 picks a new answer for it (step_next). */
void upgrade_begin(int slot, uint32_t unix_now)
{
    char tag[8];

    memset(s_gen, 0, sizeof(Gen));
    s_gen->level = s_target_level;
    s_gen->started_unix = unix_now;
    s_gen->idx = (uint8_t)slot;
    s_gen->phase = PH_NEXT;
    s_last_slot = slot;
    slot_tag(slot, tag, sizeof(tag));
    Serial.printf("[ai] upgrading %s to AI at level %s (setter %s, checker %s first)\n", tag,
                  pr_level_name(s_gen->level), k_setter_chain[0], k_checker_chain[0]);
}

void gen_store_ai(const char *checker_model, uint32_t checker_ms)
{
    Puzzle *p = &s_gen->cand;

    fill_target(p);
    copy_str(p->text, sizeof(p->text), s_gen->text);
    p->source = PUZZLE_SRC_AI;
    p->verified = 1u;
    p->level = s_gen->level;            /* the solver's rating of the rules equals the requested level (gate) */
    p->attempts = s_gen->attempt;
    copy_str(p->setter, sizeof(p->setter), s_gen->setter);
    copy_str(p->checker, sizeof(p->checker), checker_model);
    copy_str(p->checker_answer, sizeof(p->checker_answer), s_gen->answer);
    p->setter_ms = s_gen->setter_ms;
    p->checker_ms = clamp_ms(checker_ms);
}

/* A manual redo is finished when no slot is left in s_force_slots. */
void note_redo_progress(void)
{
    if (s_redo_active && s_force_slots == 0u) {
        s_redo_active = false;
        s_runs_done++;                  /* a whole-set event finished (not one per slot, R5) */
        Serial.println("[ai] manual redo finished: every slot was upgraded again");
    }
}

/* The verified AI puzzle replaces the slot's bank puzzle in place (and is saved). A next slot follows at once. */
void upgrade_succeeded(void)
{
    char tag[8];

    cur_tag(tag, sizeof(tag));
    if (install_slot((int)s_gen->idx, s_gen->cand, false, s_gen->started_unix)) {
        s_force_slots = (uint8_t)(s_force_slots & ~(1u << s_gen->idx));
        Serial.printf("[ai] %s is now an AI puzzle: %u of %u AI\n", tag, (unsigned)s_set->ai_count,
                      (unsigned)PUZZLE_SLOTS);
    } else {
        Serial.printf("[ai] %s: verified but not installed (its pair was used, a session runs, or the set was busy)\n", tag);
    }
    s_gen->phase = PH_IDLE;
    s_wait_active = false;              /* success: go straight on with the next slot */
    note_redo_progress();
}

/* Every attempt of this slot failed (or the models are unreachable): the bank puzzle stays, the next try waits (D1). */
void upgrade_failed(const char *why)
{
    char tag[8];

    cur_tag(tag, sizeof(tag));
    s_gen->phase = PH_IDLE;
    s_wait_active = true;
    s_next_upgrade_ms = s_now_ms + APP_PUZZLE_UPGRADE_RETRY_MS;
    Serial.printf("[ai] %s: %s: the bank puzzle stays, next try in %u s\n", tag, why,
                  (unsigned)(APP_PUZZLE_UPGRADE_RETRY_MS / MS_PER_S));
}

/* The slot is dropped without a retry wait: Wi-Fi or the clock went away, the level changed, or its pair was spent. */
void upgrade_abandon(const char *why)
{
    char tag[8];

    cur_tag(tag, sizeof(tag));
    s_gen->phase = PH_IDLE;
    Serial.printf("[ai] %s: upgrade abandoned (%s), the bank puzzle stays\n", tag, why);
}

/* The attempt failed (bad riddle, wrong checker answer, nobody answered): next attempt, or give the slot up and wait. */
void gen_fail_attempt(void)
{
    char why[40];

    if (s_gen->attempt < APP_PUZZLE_ATTEMPTS && !s_gen->abort_ai) {
        s_gen->phase = PH_ATTEMPT;
        return;
    }
    if (s_gen->abort_ai) {
        upgrade_failed("models unreachable");
        return;
    }
    snprintf(why, sizeof(why), "%u attempts failed", (unsigned)s_gen->attempt);
    upgrade_failed(why);
}

/* Every model of a chain failed without a single reply. Twice in a row = the LLM is unreachable. */
void gen_chain_exhausted(void)
{
    if (!s_gen->chain_replied) {
        s_gen->dead_chains++;
        if (s_gen->dead_chains >= APP_PUZZLE_DEAD_CHAINS && !s_gen->abort_ai) {
            s_gen->abort_ai = true;
            Serial.println("[ai] models unreachable: this slot is given up");
        }
    }
    gen_fail_attempt();
}

void step_next(void)
{
    if (cur_is_dial()) {
        dial_code_from_random(esp_random(), s_gen->code);
        dial_code_text(s_gen->code, s_gen->answer);
    } else {
        wire_pick_from_random(s_gen->level, esp_random(), &s_gen->wire_n, &s_gen->wire_k);   /* wire count of the level */
        snprintf(s_gen->answer, sizeof(s_gen->answer), "%d", s_gen->wire_k);
    }
    s_gen->attempt = 0u;
    s_gen->phase = PH_ATTEMPT;
}

/* What the ESP32 asked this attempt for; the gate checks the setter's reply against it. */
GateTarget cur_target(void)
{
    GateTarget t;

    t.kind = cur_is_dial() ? PUZZLE_DIAL : PUZZLE_WIRE;
    t.wire_count = s_gen->wire_n;
    t.answer = cur_is_dial() ? (s_gen->code[0] * 1000 + s_gen->code[1] * 100 + s_gen->code[2] * 10 + s_gen->code[3])
                             : s_gen->wire_k;
    t.level = s_gen->level;
    return t;
}

/* The setter chain: one model per pass; a failing model moves on to the next one. */
void step_setter(void)
{
    const char *model = k_setter_chain[s_gen->setter_pos];
    char tag[8];
    char why[GATE_WHY_MAX];
    LlmReply r;

    if (cur_is_dial()) {
        snprintf(s_sys, SYS_PROMPT_MAX, kSetterDialFmt, pr_level_name(s_gen->level));
        snprintf(s_user, sizeof(s_user), "Level: %s. Code: %s", pr_level_name(s_gen->level), s_gen->answer);
    } else {
        snprintf(s_sys, SYS_PROMPT_MAX, kSetterWireFmt, pr_level_name(s_gen->level));
        snprintf(s_user, sizeof(s_user), "Level: %s. Wires: %d. Cut wire: %d", pr_level_name(s_gen->level),
                 s_gen->wire_n, s_gen->wire_k);
    }
    cur_tag(tag, sizeof(tag));
    r = llm_chat(model, s_sys, s_user, APP_LLM_MAX_TOKENS, APP_LLM_SETTER_TEMP, s_reply, APP_LLM_REPLY_MAX);
    if (r.status != LLM_OK) {
        s_gen->setter_pos++;
        if (s_gen->setter_pos >= APP_LLM_CHAIN_LEN) {
            gen_chain_exhausted();
        }
        return;
    }
    s_gen->chain_replied = true;
    s_gen->dead_chains = 0u;
    /* The gate (puzzle_gate.cpp, host-tested): JSON fields, text validation, rules parse + exactly one
     * solution = our answer + rating = requested level. Cheapest check first; the reason is logged. */
    if (gate_check_setter(s_reply, cur_target(), s_json, APP_LLM_REPLY_MAX, s_gen->text, sizeof(s_gen->text), why,
                          sizeof(why)) != GATE_OK) {
        Serial.printf("[ai] %s attempt %u: %s: %s rejected: %s\n", tag, (unsigned)s_gen->attempt, model,
                      cur_is_dial() ? "dial" : "wire", why);
        gen_fail_attempt();
        return;
    }
    copy_str(s_gen->setter, sizeof(s_gen->setter), model);
    s_gen->setter_ms = clamp_ms(r.latency_ms);
    /* O5: the checker is never the setter's own model. If the chain holds no other model the
     * attempt fails here and the slot keeps its bank puzzle, it is not self-checked. */
    s_gen->order_n = checker_order(k_checker_chain, APP_LLM_CHAIN_LEN, model, s_gen->order);
    s_gen->checker_pos = 0;
    s_gen->chain_replied = false;
    if (s_gen->order_n == 0) {
        Serial.printf("[ai] %s attempt %u: no checker model differs from the setter %s: rejected\n", tag,
                      (unsigned)s_gen->attempt, model);
        gen_fail_attempt();
        return;
    }
    s_gen->phase = PH_CHECKER;
}

void step_checker(void)
{
    const char *model = k_checker_chain[s_gen->order[s_gen->checker_pos]];
    char got[PUZZLE_ANSWER_LEN] = "";
    char why[GATE_WHY_MAX] = "";
    char tag[8];
    LlmReply r;
    int verdict = GATE_CHECKER_UNUSABLE;

    if (cur_is_dial()) {
        copy_str(s_sys, SYS_PROMPT_MAX, kCheckerDialSys);
    } else {
        snprintf(s_sys, SYS_PROMPT_MAX, kCheckerWireFmt, s_gen->wire_n, s_gen->wire_n);
    }
    cur_tag(tag, sizeof(tag));
    /* The checker gets only the riddle TEXT: never the rules, never the answer. */
    r = llm_chat(model, s_sys, s_gen->text, APP_LLM_MAX_TOKENS, APP_LLM_CHECKER_TEMP, s_reply, APP_LLM_REPLY_MAX);
    if (r.status == LLM_OK) {
        s_gen->chain_replied = true;
        s_gen->dead_chains = 0u;
        verdict = gate_check_checker(s_reply, cur_target(), s_gen->setter, model, s_json, APP_LLM_REPLY_MAX, got,
                                     sizeof(got), why, sizeof(why));
    }
    if (verdict == GATE_CHECKER_UNUSABLE || verdict == GATE_CHECKER_SAME_MODEL) {
        if (r.status == LLM_OK) {
            Serial.printf("[ai] %s attempt %u: %s\n", tag, (unsigned)s_gen->attempt, why);
        }
        s_gen->checker_pos++;
        if (s_gen->checker_pos >= s_gen->order_n) {
            gen_chain_exhausted();
        }
        return;
    }
    if (verdict == GATE_OK) {
        gen_store_ai(model, r.latency_ms);
        Serial.printf("[ai] %s verified (%s): %s wrote it, %s solved it (attempt %u)\n", tag,
                      pr_level_name(s_gen->level), s_gen->setter, model, (unsigned)s_gen->attempt);
        upgrade_succeeded();
        return;
    }
    Serial.printf("[ai] %s attempt %u: %s: rejected\n", tag, (unsigned)s_gen->attempt, why);
    gen_fail_attempt();
}

void gen_step(void)
{
    switch (s_gen->phase) {
    case PH_NEXT:
        step_next();
        break;
    case PH_ATTEMPT:
        s_gen->attempt++;
        s_gen->setter_pos = 0;
        s_gen->chain_replied = false;
        s_gen->phase = PH_SETTER;
        break;
    case PH_SETTER:
        step_setter();
        break;
    case PH_CHECKER:
        step_checker();
        break;
    case PH_IDLE:
        break;
    }
}

}  // namespace

/* ================= public ================= */
bool puzzle_gen_init(int level)
{
    uint32_t rnd[PUZZLE_BANK_WORDS];

    s_mutex = xSemaphoreCreateMutex();
    s_set = mem_big_new<PuzzleSet>(1u, "puzzle set");
    s_gen = mem_big_new<Gen>(1u, "puzzle generator");
    s_snap = mem_big_new<PuzzleSet>(1u, "puzzle snapshot");
    s_reply = mem_big_new<char>(APP_LLM_REPLY_MAX, "puzzle LLM reply");
    s_json = mem_big_new<char>(APP_LLM_REPLY_MAX, "puzzle LLM json");
    s_sys = mem_big_new<char>(SYS_PROMPT_MAX, "puzzle LLM prompt");
    if (s_mutex == NULL || s_set == NULL || s_gen == NULL || s_snap == NULL || s_reply == NULL || s_json == NULL ||
        s_sys == NULL) {
        return false;
    }
    memset(s_gen, 0, sizeof(Gen));
    memset(static_cast<void *>(s_set), 0, sizeof(PuzzleSet));
    if (flash_read_blob(APP_KEY_PUZZLES, &s_blob, sizeof(s_blob)) && puzzle_set_valid(s_blob)) {
        *s_set = s_blob;
        s_spent = (uint8_t)((uint32_t)flash_read_int(APP_KEY_PUZZLE_USED, 0) & SPENT_MASK_ALL);
        Serial.printf("[ai] puzzle set loaded from flash: %u AI-verified puzzles, generated at unix %u, spent mask 0x%x\n",
                      (unsigned)s_set->ai_count, (unsigned)s_set->generated_unix, (unsigned)s_spent);
    } else {
        random_words(rnd, PUZZLE_BANK_WORDS);
        bank_fill_set(s_set, rnd, level);
        s_spent = 0u;
        Serial.println("[ai] no stored puzzle set: using template-bank puzzles, upgraded to AI one slot at a time");
    }
    s_target_level = (uint8_t)level;
    s_set_gen = 1u;
    s_reserved_pair = -1;
    return true;
}

void puzzle_gen_request(void)
{
    s_force_slots = (uint8_t)((1u << PUZZLE_SLOTS) - 1u);       /* every slot is redone by the LLM, AI or not */
    s_redo_active = true;
    s_wait_active = false;                                  /* no waiting: start with the next pass that allows it */
    Serial.println("[ai] manual redo: every slot will be upgraded again as soon as the network allows");
}

bool puzzle_gen_busy(void)
{
    /* Between two slots (PH_NEXT, nothing sent yet) this is false, so a briefing waiting for the LLM gets its turn
     * before the next slot's first call (R4). Once a slot is under way the briefing waits for it, as before. */
    return s_gen->phase == PH_ATTEMPT || s_gen->phase == PH_SETTER || s_gen->phase == PH_CHECKER;
}

/* The slot's pair was spent (a session consumed it while the upgrade was paused): its old answer must not come back. */
static bool slot_pair_spent(int idx)
{
    bool spent = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) == pdTRUE) {
        spent = ((s_spent >> (idx % PUZZLE_PAIRS)) & 1u) != 0u;
        xSemaphoreGive(s_mutex);
    }
    return spent;
}

/* Continuous upgrade (D1): while the network works and nothing time-critical runs, take the next bank (or
 * stale-level, or forced) slot of an unspent pair, one LLM call per pass. */
static void service_upgrade(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok)
{
    int slot = -1;

    if (s_gen->phase != PH_IDLE) {
        if (!net_ok) {
            upgrade_abandon("Wi-Fi or clock lost");         /* the bank puzzle stays; no wait, the network may be back soon */
            return;
        }
        if (!gen_ok) {
            return;                                         /* paused: a bomb session or the alarm comes first */
        }
        if (s_gen->level != s_target_level) {
            upgrade_abandon("level changed");
        } else if (slot_pair_spent((int)s_gen->idx)) {
            upgrade_abandon("its pair was used");
        } else {
            gen_step();
        }
        return;
    }
    if (!net_ok || !gen_ok || set_level() != s_target_level) {
        return;                                             /* a level change is refilled from the bank first */
    }
    if (s_wait_active && (int32_t)(now_ms - s_next_upgrade_ms) < 0) {
        return;                                             /* the last attempt failed: wait (D1) */
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) == pdTRUE) {
        slot = upgrade_next_slot(*s_set, s_target_level, s_spent, s_force_slots, s_last_slot);
        xSemaphoreGive(s_mutex);
    }
    if (slot >= 0) {
        upgrade_begin(slot, unix_now);
    }
}

void puzzle_gen_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok)
{
    s_now_ms = now_ms;
    if (service_spent_refill() || service_level_refill()) {
        return;                                             /* bank work: one slot per pass, offline, in a session too */
    }
    service_upgrade(now_ms, unix_now, net_ok, gen_ok);
}

bool puzzle_gen_apply_level(int level, bool session_active)
{
    if (level < PR_EASY || level > PR_HARD) {
        return false;
    }
    s_in_session = session_active;
    if ((int)s_target_level == level) {
        return false;
    }
    s_target_level = (uint8_t)level;                    /* every later puzzle asks for it */
    s_lvl_cursor = 0;                                   /* the bank refill of the other slots starts over */
    s_lvl_announced = false;
    s_wait_active = false;
    if (s_gen->phase != PH_IDLE) {
        upgrade_abandon("level changed");               /* its puzzle would be for the old level */
    }
    /* CLAUDE.md session guard: the refill of the other slots (service_level_refill) waits for DEFUSED -> IDLE; the
     * bank refill of SPENT pairs does not wait (it never touches the reserved pair). */
    return !session_active && set_level() != s_target_level;
}

bool puzzle_set_snapshot(PuzzleSet *out, uint32_t *gen)
{
    if (out == NULL || s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    *out = *s_set;
    if (gen != NULL) {
        *gen = s_set_gen;
    }
    xSemaphoreGive(s_mutex);
    return true;
}

uint32_t puzzle_set_generation(void)
{
    return s_set_gen;                                   /* 32-bit read: atomic */
}

int puzzle_next_pair(void)
{
    int pair = 0;

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) == pdTRUE) {
        pair = pair_pick(s_spent, s_spend_order, PUZZLE_PAIRS, NULL);
        xSemaphoreGive(s_mutex);
    }
    return pair < 0 ? 0 : pair;
}

/* Link task: one short lock, two ~140-byte copies. The pair is the lowest unspent one; with every pair spent
 * and none refilled yet (D6: more than four bombs within seconds) it is the least recently spent one, so the
 * bomb never waits. A reused pair is not made the reserved pair: the bank refills it right away. */
bool puzzle_take_next_pair(Puzzle *dial, Puzzle *wire, int *pair, uint32_t *gen)
{
    int p;
    bool reused = false;

    if (dial == NULL || wire == NULL || pair == NULL || gen == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    p = pair_pick(s_spent, s_spend_order, PUZZLE_PAIRS, &reused);
    p = (p < 0) ? 0 : p;
    *dial = s_set->dial[p];                              /* ~120 bytes each: not the whole 1.1 KB set */
    *wire = s_set->wire[p];
    *pair = p;
    *gen = s_set_gen;
    s_reserved_pair = reused ? (int8_t)-1 : (int8_t)p;   /* the network task re-sends this pair if it changes it */
    xSemaphoreGive(s_mutex);
    if (reused) {
        Serial.printf("[ai] all pairs spent: reusing pair %d\n", p);
    }
    return true;
}

/* Link task (through bomb_config_service): RAM only. The flash write waits for the network task
 * (puzzle_flush_used), because a flash write can take tens of ms and the link task must not stall.
 * The pair is spent for good (D4): the network task refills its two slots from the bank, then frees it. */
bool puzzle_consume_pair(int pair)
{
    uint8_t spent;

    if (pair < 0 || pair >= PUZZLE_PAIRS) {
        return true;                                    /* nothing to mark */
    }
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;                                   /* lock busy: the caller tries again */
    }
    s_spent = pair_mark_spent(s_spent, pair, PUZZLE_PAIRS);
    s_spend_order[pair] = ++s_spend_counter;
    s_refilled = (uint8_t)(s_refilled & ~((1u << pair) | (1u << (PUZZLE_PAIRS + pair))));   /* both slots need the bank again */
    if (s_reserved_pair == (int8_t)pair) {
        s_reserved_pair = -1;                           /* the bomb started with it: nothing left to re-send */
    }
    s_used_dirty = true;
    spent = s_spent;
    xSemaphoreGive(s_mutex);
    Serial.printf("[ai] pair %d spent, mask 0x%x\n", pair, (unsigned)spent);
    return true;
}

bool puzzle_reserved_changed_take(void)
{
    bool changed = false;

    if (!s_reserved_dirty || s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;                                   /* the flag is read without the lock (one byte), cleared with it */
    }
    changed = s_reserved_dirty;
    s_reserved_dirty = false;
    xSemaphoreGive(s_mutex);
    return changed;
}

void puzzle_flush_used(void)
{
    uint8_t spent = 0u;
    bool dirty = false;

    if (!s_used_dirty || s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return;
    }
    if (s_used_dirty) {
        spent = s_spent;
        s_used_dirty = false;           /* cleared under the lock: a later change sets it again */
        dirty = true;
    }
    xSemaphoreGive(s_mutex);
    if (dirty) {
        save_spent(spent);
    }
}

uint32_t puzzle_gen_runs_done(void)
{
    return s_runs_done;                                 /* 32-bit read: atomic */
}

void puzzle_gen_info(PuzzleRunInfo *out)
{
    if (out == NULL) {
        return;
    }
    const bool working = (s_gen->phase != PH_IDLE);
    uint32_t wait_ms;

    out->busy = working;
    out->queued = s_redo_active;
    out->slot = (working && s_gen->idx < PUZZLE_SLOTS) ? (int8_t)s_gen->idx : (int8_t)-1;
    out->spent = s_spent;
    /* Seconds until the next attempt after a failure; 0 = no wait pending. s_now_ms is the last pass's time, so
     * the value is at most one pass stale, which is plenty for a status page. */
    out->retry_s = 0u;
    if (!working && s_wait_active) {
        wait_ms = s_next_upgrade_ms - s_now_ms;
        if ((int32_t)wait_ms > 0) {
            wait_ms = wait_ms / MS_PER_S + 1u;
            out->retry_s = (uint16_t)(wait_ms < RETRY_S_MAX ? wait_ms : RETRY_S_MAX);
        }
    }
}

void puzzle_print_set(void)
{
    uint32_t gen = 0u;

    if (!puzzle_set_snapshot(s_snap, &gen)) {
        Serial.println("[ai] puzzle set busy, try again");
        return;
    }
    print_set(*s_snap, gen);
    PuzzleRunInfo info;
    char tag[8];

    puzzle_gen_info(&info);
    Serial.printf("[ai] AI %u/%d, spent mask 0x%x, next pair %d\n", (unsigned)puzzle_set_count_ai(*s_snap),
                  PUZZLE_SLOTS, (unsigned)info.spent, puzzle_next_pair());
    if (info.slot >= 0) {
        slot_tag((int)info.slot, tag, sizeof(tag));
        Serial.printf("[ai] upgrading slot %s\n", tag);
    } else if (info.retry_s > 0u) {
        Serial.printf("[ai] idle, next upgrade attempt in %us\n", (unsigned)info.retry_s);
    } else {
        Serial.println("[ai] idle");
    }
}
