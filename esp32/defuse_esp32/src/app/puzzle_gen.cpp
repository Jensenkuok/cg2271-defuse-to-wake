/* Defuse to Wake - nightly puzzle generation. See puzzle_gen.h. */
#include "puzzle_gen.h"

#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#include "config.h"
#include "flash_store.h"
#include "llm_client.h"

namespace {

#define SEC_PER_HOUR_PZ     3600u
#define SEC_PER_DAY_PZ      86400u
#define USED_MASK_ALL       ((1u << PUZZLE_PAIRS) - 1u)
#define SET_LOCK_WAIT_MS    100u
#define SYS_PROMPT_MAX      256
#define USER_PROMPT_MAX     48
#define LATENCY_MAX_MS      65535u

/* ---- Prompts. The riddle answer is given by the ESP32; the checker never sees it. ---- */
const char kSetterDialSys[] =
    "You write one short logic puzzle for a bomb-defusal game. The user gives a secret 4-digit code. "
    "Write ONE puzzle of at most 80 characters, plain ASCII only, whose only solution is exactly that code "
    "and which pins down all four digits. Never write the code itself. "
    "Reply ONLY with JSON: {\"puzzle\":\"...\"}";
const char kSetterWireSys[] =
    "You write one short riddle for a bomb-defusal game. The user gives the number of wires (numbered 1..N "
    "from the left) and the wire to cut. Write ONE riddle of at most 60 characters, plain ASCII only, whose "
    "only solution is that wire number. Never state the number directly. "
    "Reply ONLY with JSON: {\"puzzle\":\"...\"}";
const char kCheckerDialSys[] =
    "Solve the puzzle. The answer is a 4-digit code (digits 0-9, leading zeros allowed). "
    "Reply ONLY with JSON: {\"answer\":\"1234\"}";
const char kCheckerWireFmt[] =
    "Solve the puzzle. There are %d wires numbered 1..%d from the left. The answer is one wire number. "
    "Reply ONLY with JSON: {\"answer\":\"2\"}";

const char *const k_setter_chain[APP_LLM_CHAIN_LEN] = APP_LLM_SETTER_CHAIN;
const char *const k_checker_chain[APP_LLM_CHAIN_LEN] = APP_LLM_CHECKER_CHAIN;

enum Phase { PH_IDLE = 0, PH_NEXT, PH_ATTEMPT, PH_SETTER, PH_CHECKER };

/* The run in progress. Only the network task touches it. */
struct Gen {
    Phase phase;
    PuzzleSet work;
    uint32_t started_unix;
    uint8_t idx;                        /* 0..PAIRS-1 dial, PAIRS..2*PAIRS-1 wire */
    uint8_t attempt;                    /* 1..APP_PUZZLE_ATTEMPTS */
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
    uint8_t ai_count;
    char last_setter[PUZZLE_MODEL_LEN]; /* kept on a rejected puzzle for the "ai" listing */
    char last_checker[PUZZLE_MODEL_LEN];
    char last_said[PUZZLE_ANSWER_LEN];
};

SemaphoreHandle_t s_mutex;
PuzzleSet s_set;                        /* the active set; guarded by s_mutex */
uint32_t s_set_gen;                     /* guarded */
uint8_t s_used;                         /* used-pair bit mask; guarded */
bool s_used_dirty;                      /* s_used changed in RAM and is not in flash yet; guarded */
uint32_t s_runs_done;                   /* finished generation runs; written by the network task only */
PuzzleSet s_blob;                       /* flash buffer */

Gen s_gen;
char s_reply[APP_LLM_REPLY_MAX];
char s_json[APP_LLM_REPLY_MAX];
char s_sys[SYS_PROMPT_MAX];
char s_user[USER_PROMPT_MAX];

bool s_requested;
bool s_manual;                          /* the current request came from the serial command */
bool s_boot_checked;
int32_t s_last_night_day = -1;
uint32_t s_next_try_ms;
uint8_t s_retries_left;
uint32_t s_now_ms;

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
    Serial.printf("[ai] %c%u %-4s %-10s setter=%s checker=%s said=%s tries=%u %ums/%ums ans=%s", tag, i,
                  p.source == PUZZLE_SRC_AI ? "AI" : "BANK",
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
void save_used(uint8_t used)
{
    if (!flash_write_int(APP_KEY_PUZZLE_USED, (int32_t)used)) {
        Serial.println("[ai] could not write the used-pair mask to flash");
    }
}

void save_set(void)
{
    memset(static_cast<void *>(&s_blob), 0, sizeof(s_blob));   /* no uninitialised padding in flash */
    s_blob = s_set;
    if (!flash_write_blob(APP_KEY_PUZZLES, &s_blob, sizeof(s_blob))) {
        Serial.println("[ai] could not write the puzzle set to flash");
    }
}

/* Replace the active set (network task). The new set was built in s_gen.work, a buffer no other
 * task reads, so the lock is held only for one struct copy (~1.1 KB, microseconds): the link task,
 * which reads the set to build a BOMB_CONFIG, never waits for a flash write or an LLM call. */
void install_set(const PuzzleSet &work)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        Serial.println("[ai] could not lock the puzzle set: keeping the previous one");
        return;
    }
    s_set = work;
    s_set_gen++;
    s_used = 0u;
    s_used_dirty = false;               /* save_used(0) below covers it */
    xSemaphoreGive(s_mutex);
    save_set();
    save_used(0u);
}

/* ================= generation state machine ================= */
Puzzle &slot(uint8_t idx)
{
    return idx < PUZZLE_PAIRS ? s_gen.work.dial[idx] : s_gen.work.wire[idx - PUZZLE_PAIRS];
}

bool cur_is_dial(void)
{
    return s_gen.idx < PUZZLE_PAIRS;
}

void cur_tag(char *buf, size_t size)
{
    snprintf(buf, size, "%c%u", cur_is_dial() ? 'D' : 'W',
             (unsigned)(cur_is_dial() ? s_gen.idx : s_gen.idx - PUZZLE_PAIRS));
}

void fill_target(Puzzle *p)
{
    memset(p, 0, sizeof(*p));
    p->kind = (uint8_t)(cur_is_dial() ? PUZZLE_DIAL : PUZZLE_WIRE);
    if (cur_is_dial()) {
        memcpy(p->dial_code, s_gen.code, sizeof(p->dial_code));
    } else {
        p->wire_count = (uint8_t)s_gen.wire_n;
        p->wire_k = (uint8_t)s_gen.wire_k;
    }
}

void gen_begin(uint32_t unix_now)
{
    memset(&s_gen, 0, sizeof(s_gen));
    s_gen.work.magic = PUZZLE_MAGIC;
    s_gen.started_unix = unix_now;
    s_gen.phase = PH_NEXT;
    Serial.printf("[ai] generating %u dial + %u wire puzzles (setter %s, checker %s first)\n",
                  (unsigned)PUZZLE_PAIRS, (unsigned)PUZZLE_PAIRS, k_setter_chain[0], k_checker_chain[0]);
}

/* The bank fills the slot: the answer was already picked, only the text comes from a template.
 * What the LLM did before giving up is kept for the listing. */
void gen_store_bank(void)
{
    Puzzle *p = &slot(s_gen.idx);

    if (cur_is_dial()) {
        puzzle_fill_bank_dial(p, s_gen.code, esp_random() % PUZZLE_VARIANTS);
    } else {
        puzzle_fill_bank_wire(p, s_gen.wire_n, s_gen.wire_k, esp_random() % PUZZLE_VARIANTS);
    }
    p->attempts = s_gen.attempt;
    copy_str(p->setter, sizeof(p->setter), s_gen.last_setter);
    copy_str(p->checker, sizeof(p->checker), s_gen.last_checker);
    copy_str(p->checker_answer, sizeof(p->checker_answer), s_gen.last_said);
    s_gen.last_setter[0] = '\0';
    s_gen.last_checker[0] = '\0';
    s_gen.last_said[0] = '\0';
}

void gen_store_ai(const char *checker_model, uint32_t checker_ms)
{
    Puzzle *p = &slot(s_gen.idx);

    fill_target(p);
    copy_str(p->text, sizeof(p->text), s_gen.text);
    p->source = PUZZLE_SRC_AI;
    p->verified = 1u;
    p->attempts = s_gen.attempt;
    copy_str(p->setter, sizeof(p->setter), s_gen.setter);
    copy_str(p->checker, sizeof(p->checker), checker_model);
    copy_str(p->checker_answer, sizeof(p->checker_answer), s_gen.answer);
    p->setter_ms = s_gen.setter_ms;
    p->checker_ms = clamp_ms(checker_ms);
    s_gen.ai_count++;
    s_gen.last_setter[0] = '\0';
    s_gen.last_checker[0] = '\0';
    s_gen.last_said[0] = '\0';
}

void gen_finish(void)
{
    s_gen.work.generated_unix = s_gen.started_unix;
    s_gen.work.ai_count = s_gen.ai_count;
    print_set(s_gen.work, s_set_gen + (s_gen.ai_count > 0u ? 1u : 0u));
    if (s_gen.ai_count > 0u) {
        install_set(s_gen.work);
        Serial.printf("[ai] new set stored in flash (%u AI + %u bank puzzles)\n", (unsigned)s_gen.ai_count,
                      (unsigned)(2u * PUZZLE_PAIRS - s_gen.ai_count));
    } else {
        Serial.println("[ai] no puzzle was verified by the models: the previous set stays active");
        if (s_retries_left > 0u) {
            s_retries_left--;
            s_requested = true;
            s_next_try_ms = s_now_ms + APP_PUZZLE_RETRY_MS;
            Serial.printf("[ai] will retry in %u min\n", (unsigned)(APP_PUZZLE_RETRY_MS / 60000u));
        }
    }
    s_gen.phase = PH_IDLE;
    s_runs_done++;                      /* the briefing generator starts after every finished run */
}

/* The attempt failed (bad riddle, wrong checker answer, nobody answered): next attempt or bank. */
void gen_fail_attempt(void)
{
    char tag[8];

    if (s_gen.attempt < APP_PUZZLE_ATTEMPTS && !s_gen.abort_ai) {
        s_gen.phase = PH_ATTEMPT;
        return;
    }
    cur_tag(tag, sizeof(tag));
    gen_store_bank();
    Serial.printf("[ai] %s: using a bank template after %u attempt(s)\n", tag, (unsigned)s_gen.attempt);
    s_gen.idx++;
    s_gen.phase = PH_NEXT;
}

/* Every model of a chain failed without a single reply. Twice in a row = the LLM is unreachable. */
void gen_chain_exhausted(void)
{
    if (!s_gen.chain_replied) {
        s_gen.dead_chains++;
        if (s_gen.dead_chains >= APP_PUZZLE_DEAD_CHAINS && !s_gen.abort_ai) {
            s_gen.abort_ai = true;
            Serial.println("[ai] models unreachable: the rest of the set comes from the bank");
        }
    }
    gen_fail_attempt();
}

void step_next(void)
{
    if (s_gen.idx >= 2u * PUZZLE_PAIRS) {
        gen_finish();
        return;
    }
    if (cur_is_dial()) {
        dial_code_from_random(esp_random(), s_gen.code);
        dial_code_text(s_gen.code, s_gen.answer);
    } else {
        wire_pick_from_random(esp_random(), &s_gen.wire_n, &s_gen.wire_k);
        snprintf(s_gen.answer, sizeof(s_gen.answer), "%d", s_gen.wire_k);
    }
    s_gen.attempt = 0u;
    s_gen.last_setter[0] = '\0';
    s_gen.last_checker[0] = '\0';
    s_gen.last_said[0] = '\0';
    if (s_gen.abort_ai) {
        gen_store_bank();
        s_gen.idx++;                    /* stay in PH_NEXT: the bank fills the rest at once */
        return;
    }
    s_gen.phase = PH_ATTEMPT;
}

/* The setter chain: one model per pass; a failing model moves on to the next one. */
void step_setter(void)
{
    const char *model = k_setter_chain[s_gen.setter_pos];
    const char *sys;
    char tag[8];
    LlmReply r;
    const int kind = cur_is_dial() ? PUZZLE_DIAL : PUZZLE_WIRE;

    if (cur_is_dial()) {
        sys = kSetterDialSys;
        snprintf(s_user, sizeof(s_user), "Code: %s", s_gen.answer);
    } else {
        sys = kSetterWireSys;
        snprintf(s_user, sizeof(s_user), "Wires: %d. Cut wire: %d", s_gen.wire_n, s_gen.wire_k);
    }
    cur_tag(tag, sizeof(tag));
    r = llm_chat(model, sys, s_user, APP_LLM_MAX_TOKENS, APP_LLM_SETTER_TEMP, s_reply, sizeof(s_reply));
    if (r.status != LLM_OK) {
        s_gen.setter_pos++;
        if (s_gen.setter_pos >= APP_LLM_CHAIN_LEN) {
            gen_chain_exhausted();
        }
        return;
    }
    s_gen.chain_replied = true;
    s_gen.dead_chains = 0u;
    if (!extract_json_object(s_reply, s_json, sizeof(s_json)) ||
        !json_get_string(s_json, "puzzle", s_gen.text, sizeof(s_gen.text))) {
        Serial.printf("[ai] %s attempt %u: %s did not return a usable {\"puzzle\"} (too long or not JSON)\n", tag,
                      (unsigned)s_gen.attempt, model);
        gen_fail_attempt();
        return;
    }
    if (!validate_puzzle(s_gen.text, kind, s_gen.answer)) {
        Serial.printf("[ai] %s attempt %u: %s riddle rejected by validation: \"%s\"\n", tag, (unsigned)s_gen.attempt,
                      model, s_gen.text);
        gen_fail_attempt();
        return;
    }
    copy_str(s_gen.setter, sizeof(s_gen.setter), model);
    copy_str(s_gen.last_setter, sizeof(s_gen.last_setter), model);
    s_gen.setter_ms = clamp_ms(r.latency_ms);
    /* The checker must be a different model whenever another one of its chain answers. */
    s_gen.order_n = checker_order(k_checker_chain, APP_LLM_CHAIN_LEN, model, s_gen.order);
    s_gen.checker_pos = 0;
    s_gen.chain_replied = false;
    s_gen.phase = PH_CHECKER;
}

void step_checker(void)
{
    const char *model = k_checker_chain[s_gen.order[s_gen.checker_pos]];
    const int kind = cur_is_dial() ? PUZZLE_DIAL : PUZZLE_WIRE;
    char got[PUZZLE_ANSWER_LEN];
    char tag[8];
    LlmReply r;

    if (cur_is_dial()) {
        copy_str(s_sys, sizeof(s_sys), kCheckerDialSys);
    } else {
        snprintf(s_sys, sizeof(s_sys), kCheckerWireFmt, s_gen.wire_n, s_gen.wire_n);
    }
    cur_tag(tag, sizeof(tag));
    r = llm_chat(model, s_sys, s_gen.text, APP_LLM_MAX_TOKENS, APP_LLM_CHECKER_TEMP, s_reply, sizeof(s_reply));
    if (r.status == LLM_OK) {
        s_gen.chain_replied = true;
        s_gen.dead_chains = 0u;
    }
    if (r.status != LLM_OK ||
        !extract_json_object(s_reply, s_json, sizeof(s_json)) ||
        !parse_checker_answer(s_json, kind, s_gen.wire_n, got, sizeof(got))) {
        if (r.status == LLM_OK) {
            Serial.printf("[ai] %s attempt %u: %s gave no usable answer\n", tag, (unsigned)s_gen.attempt, model);
        }
        s_gen.checker_pos++;
        if (s_gen.checker_pos >= s_gen.order_n) {
            gen_chain_exhausted();
        }
        return;
    }
    copy_str(s_gen.last_checker, sizeof(s_gen.last_checker), model);
    copy_str(s_gen.last_said, sizeof(s_gen.last_said), got);
    if (strcmp(got, s_gen.answer) == 0) {
        gen_store_ai(model, r.latency_ms);
        Serial.printf("[ai] %s verified: %s wrote it, %s solved it (attempt %u)\n", tag, s_gen.setter, model,
                      (unsigned)s_gen.attempt);
        s_gen.idx++;
        s_gen.phase = PH_NEXT;
        return;
    }
    Serial.printf("[ai] %s attempt %u: %s answered %s, expected %s: rejected\n", tag, (unsigned)s_gen.attempt, model,
                  got, s_gen.answer);
    gen_fail_attempt();
}

void gen_step(void)
{
    switch (s_gen.phase) {
    case PH_NEXT:
        step_next();
        break;
    case PH_ATTEMPT:
        s_gen.attempt++;
        s_gen.setter_pos = 0;
        s_gen.chain_replied = false;
        s_gen.phase = PH_SETTER;
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
bool puzzle_gen_init(void)
{
    uint32_t rnd[PUZZLE_BANK_WORDS];

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return false;
    }
    memset(&s_gen, 0, sizeof(s_gen));
    memset(static_cast<void *>(&s_set), 0, sizeof(s_set));
    if (flash_read_blob(APP_KEY_PUZZLES, &s_blob, sizeof(s_blob)) && puzzle_set_valid(s_blob)) {
        s_set = s_blob;
        s_used = (uint8_t)((uint32_t)flash_read_int(APP_KEY_PUZZLE_USED, 0) & USED_MASK_ALL);
        Serial.printf("[ai] puzzle set loaded from flash: %u AI-verified puzzles, generated at unix %u, used mask 0x%x\n",
                      (unsigned)s_set.ai_count, (unsigned)s_set.generated_unix, (unsigned)s_used);
    } else {
        random_words(rnd, PUZZLE_BANK_WORDS);
        puzzle_set_fill_bank(&s_set, rnd);
        s_used = 0u;
        Serial.println("[ai] no stored puzzle set: using template-bank puzzles until the first run");
    }
    s_set_gen = 1u;
    return true;
}

void puzzle_gen_request(void)
{
    s_requested = true;
    s_manual = true;
    s_retries_left = 0u;
}

bool puzzle_gen_busy(void)
{
    return s_gen.phase != PH_IDLE;
}

static void request_auto(const char *why)
{
    s_requested = true;
    s_manual = false;
    s_retries_left = APP_PUZZLE_RETRIES;
    s_next_try_ms = s_now_ms;
    Serial.printf("[ai] puzzle run wanted (%s)\n", why);
}

void puzzle_gen_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok)
{
    s_now_ms = now_ms;
    if (s_gen.phase != PH_IDLE) {
        if (!gen_ok) {
            return;                                     /* paused: a bomb session or the alarm comes first */
        }
        if (!net_ok) {
            s_gen.abort_ai = true;                      /* Wi-Fi or clock lost: finish with the bank */
        }
        gen_step();
        return;
    }
    if (net_ok && unix_now != 0u) {
        const uint32_t local = unix_now + (uint32_t)APP_TZ_OFFSET_S;
        const int32_t day = (int32_t)(local / SEC_PER_DAY_PZ);

        if (!s_boot_checked) {
            s_boot_checked = true;
            if (!puzzle_set_fresh(s_set.generated_unix, s_set.ai_count, unix_now, APP_PUZZLE_FRESH_S)) {
                request_auto("boot: no fresh set");
            }
        }
        if (((local % SEC_PER_DAY_PZ) / SEC_PER_HOUR_PZ) >= (uint32_t)APP_PUZZLE_NIGHTLY_HOUR && day != s_last_night_day) {
            s_last_night_day = day;
            request_auto("nightly");
        }
    }
    if (!s_requested) {
        return;
    }
    if (!net_ok) {
        if (s_manual) {
            s_requested = false;
            s_manual = false;
            Serial.printf("[ai] no Wi-Fi or clock: the cached set stays active (%u AI-verified puzzles)\n",
                          (unsigned)s_set.ai_count);
        }
        return;                                         /* automatic runs wait for the network */
    }
    if (!gen_ok || (!s_manual && (int32_t)(now_ms - s_next_try_ms) < 0)) {
        return;
    }
    s_requested = false;
    s_manual = false;
    gen_begin(unix_now);
}

bool puzzle_set_snapshot(PuzzleSet *out, uint32_t *gen)
{
    if (out == NULL || s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    *out = s_set;
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
        pair = pair_next_unused(s_used, PUZZLE_PAIRS);
        xSemaphoreGive(s_mutex);
    }
    return pair < 0 ? 0 : pair;
}

bool puzzle_take_next_pair(Puzzle *dial, Puzzle *wire, int *pair, uint32_t *gen)
{
    int p;

    if (dial == NULL || wire == NULL || pair == NULL || gen == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    p = pair_next_unused(s_used, PUZZLE_PAIRS);
    p = (p < 0) ? 0 : p;
    *dial = s_set.dial[p];                              /* ~120 bytes each: not the whole 1.1 KB set */
    *wire = s_set.wire[p];
    *pair = p;
    *gen = s_set_gen;
    xSemaphoreGive(s_mutex);
    return true;
}

/* Link task (through bomb_config_service): RAM only. The flash write waits for the network task
 * (puzzle_flush_used), because a flash write can take tens of ms and the link task must not stall. */
bool puzzle_consume_pair(int pair, uint32_t gen)
{
    bool cycled = false;
    bool applied = false;
    uint8_t used = 0u;

    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return false;                                   /* lock busy: the caller tries again */
    }
    if (gen == s_set_gen) {
        s_used = pair_mark_used(s_used, pair, PUZZLE_PAIRS, &cycled);
        s_used_dirty = true;
        used = s_used;
        applied = true;
    }
    xSemaphoreGive(s_mutex);
    if (!applied) {
        Serial.printf("[ai] pair %d came from an older set: nothing to mark\n", pair);
        return true;
    }
    Serial.printf("[ai] pair %d used, mask 0x%x%s\n", pair, (unsigned)used, cycled ? " (all pairs used: starting over)" : "");
    return true;
}

void puzzle_flush_used(void)
{
    uint8_t used = 0u;
    bool dirty = false;

    if (!s_used_dirty || s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SET_LOCK_WAIT_MS)) != pdTRUE) {
        return;
    }
    if (s_used_dirty) {
        used = s_used;
        s_used_dirty = false;           /* cleared under the lock: a later change sets it again */
        dirty = true;
    }
    xSemaphoreGive(s_mutex);
    if (dirty) {
        save_used(used);
    }
}

uint32_t puzzle_gen_runs_done(void)
{
    return s_runs_done;                                 /* 32-bit read: atomic */
}

void puzzle_print_set(void)
{
    static PuzzleSet snap;                              /* ~1.1 KB: off the stack */
    uint32_t gen = 0u;

    if (!puzzle_set_snapshot(&snap, &gen)) {
        Serial.println("[ai] puzzle set busy, try again");
        return;
    }
    print_set(snap, gen);
    Serial.printf("[ai] used mask 0x%x, next pair %d%s\n", (unsigned)s_used, puzzle_next_pair(),
                  puzzle_gen_busy() ? ", a run is in progress" : "");
}
