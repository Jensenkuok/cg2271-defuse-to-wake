/* Defuse to Wake - BOMB_CONFIG sender. See bomb_config.h. */
#include "bomb_config.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "history.h"
#include "link_task.h"
#include "puzzle_gen.h"
#include "puzzle_logic.h"
#include "../shared/protocol.h"

namespace {

#define STATE_UNKNOWN 0xFFu

struct Pending {
    bool active;
    int pair;
    uint32_t gen;
    uint32_t since_ms;
    ActivePuzzle view;      /* what app_state shows once the MCX ACKs it */
};

Pending s_pending;
int s_active_pair = -1;     /* the pair the MCX holds (last ACKed config) */
uint32_t s_active_gen;
bool s_active_consumed;     /* a bomb already started with it */
uint32_t s_seen_acks;
uint32_t s_seen_fails;
uint8_t s_prev_state = STATE_UNKNOWN;
bool s_need_trigger;        /* a trigger fired but the config could not be queued yet */
uint32_t s_seen_set_gen;
bool s_resend_new_set;
volatile bool s_ready;      /* the puzzle set and the history exist (the link task starts before the network task) */
SemaphoreHandle_t s_lock;   /* guards every static above: bomb_config_send is called by the link task AND (for the
                             * SET_ALARM case) the network task, bomb_config_service by the link task */
#define CFG_LOCK_WAIT_MS 20u /* short: the link task has 100 ms retransmit timing; a missed pass is retried in 50 ms */

Difficulty current_difficulty(void)
{
    HistRecord recs[DIFF_WINDOW];
    const int n = history_recent(recs, DIFF_WINDOW);

    return difficulty(recs, n);
}

void make_view(ActivePuzzle *v, int pair, uint32_t gen, const Puzzle &d, const Puzzle &w)
{
    memset(v, 0, sizeof(*v));
    v->valid = true;
    v->pair = (uint8_t)pair;
    v->set_gen = gen;
    memcpy(v->dial_code, d.dial_code, sizeof(v->dial_code));
    v->wire_count = w.wire_count;
    v->wire_k = w.wire_k;
    v->dial_source = d.source;
    v->wire_source = w.source;
    snprintf(v->dial_text, sizeof(v->dial_text), "%s", d.text);
    /* a wire riddle is at most PUZZLE_WIRE_TEXT_MAX characters (validate_puzzle); cut explicitly */
    snprintf(v->wire_text, sizeof(v->wire_text), "%.*s", PUZZLE_WIRE_TEXT_MAX, w.text);
}

void on_ack(void)
{
    if (!s_pending.active) {
        return;
    }
    s_active_pair = s_pending.pair;
    s_active_gen = s_pending.gen;
    s_active_consumed = false;
    app_state_set_active_puzzle(s_pending.view);
    s_pending.active = false;
    Serial.printf("[cfg] BOMB_CONFIG ACKed: pair %d is now the MCX's config for the next bomb\n", s_active_pair);
}

void on_fail(void)
{
    if (s_pending.active) {
        s_pending.active = false;
        Serial.println("[cfg] BOMB_CONFIG not ACKed after the retries: the MCX keeps its previous config");
    }
}

bool in_trigger_state(const AppState &st)
{
    return st.session_active &&
           (st.game.state == PROTO_STATE_EXPLODED || st.game.state == PROTO_STATE_ARMED);
}

}  // namespace

void bomb_config_init(void)
{
    s_lock = xSemaphoreCreateMutex();                   /* priority inheritance: the link task is never held up by a low-priority holder */
    s_seen_set_gen = puzzle_set_generation();
    s_ready = (s_lock != NULL);
}

void bomb_config_difficulty(Difficulty *out)
{
    if (out != NULL) {
        *out = current_difficulty();
    }
}

/* s_lock held by the caller. */
static bool send_locked(const char *why)
{
    proto_bomb_config_t cfg;
    Difficulty diff;
    Puzzle dial;
    Puzzle wire;
    uint8_t pl[PROTO_SIZE_BOMB_CONFIG];
    uint32_t gen = 0u;
    int pair = 0;

    if (s_pending.active && (uint32_t)(millis() - s_pending.since_ms) < APP_CONFIG_PENDING_MS) {
        return false;                                   /* one at a time: wait for the ACK */
    }
    if (link_tx_space() < 1u || !puzzle_take_next_pair(&dial, &wire, &pair, &gen)) {
        return false;
    }
    diff = current_difficulty();
    puzzle_build_config(diff, dial, wire, &cfg);
    if (proto_pack_bomb_config(&cfg, pl, sizeof(pl)) == 0u || !link_send(PROTO_MSG_BOMB_CONFIG, pl, PROTO_SIZE_BOMB_CONFIG)) {
        return false;
    }
    s_pending.active = true;
    s_pending.pair = pair;
    s_pending.gen = gen;
    s_pending.since_ms = millis();
    make_view(&s_pending.view, pair, gen, dial, wire);
    Serial.printf("[cfg] BOMB_CONFIG queued (%s): pair %d, %u modules, %us, %u wires (cut %u), dial %u%u%u%u [%s]\n", why,
                  pair, (unsigned)cfg.module_count, (unsigned)cfg.time_limit_s, (unsigned)cfg.wire_count,
                  (unsigned)cfg.correct_wire + 1u, (unsigned)cfg.dial_code[0], (unsigned)cfg.dial_code[1],
                  (unsigned)cfg.dial_code[2], (unsigned)cfg.dial_code[3], diff.explanation);
    return true;
}

bool bomb_config_send(const char *why)
{
    bool sent;

    if (!s_ready || xSemaphoreTake(s_lock, pdMS_TO_TICKS(CFG_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }
    sent = send_locked(why);
    xSemaphoreGive(s_lock);
    return sent;
}

/* s_lock held by the caller. */
static void service_locked(const AppState &st)
{
    uint8_t state;
    uint32_t gen;

    if (st.cfg_acks != s_seen_acks) {
        s_seen_acks = st.cfg_acks;
        on_ack();
    }
    if (st.cfg_fails != s_seen_fails) {
        s_seen_fails = st.cfg_fails;
        on_fail();
    }
    if (s_pending.active && (uint32_t)(millis() - s_pending.since_ms) >= 2u * APP_CONFIG_PENDING_MS) {
        s_pending.active = false;                       /* the result never came: allow a new config */
        Serial.println("[cfg] no result for the last BOMB_CONFIG: giving up on it");
    }

    gen = puzzle_set_generation();
    if (gen != s_seen_set_gen) {
        s_seen_set_gen = gen;
        s_resend_new_set = true;
    }
    if (!st.game_valid) {
        return;
    }
    state = st.game.state;

    if (bomb_just_started(s_prev_state, state) && s_active_pair >= 0 && !s_active_consumed) {
        /* the bomb took the config the MCX held; if the puzzle lock is busy, try again next pass */
        s_active_consumed = puzzle_consume_pair(s_active_pair, s_active_gen);
    }
    if (config_trigger_needed(s_prev_state, state, st.session_active)) {
        s_need_trigger = true;
    }
    s_prev_state = state;

    if (s_need_trigger) {
        if (!in_trigger_state(st)) {
            s_need_trigger = false;                     /* the state moved on before we could send */
        } else if (st.link_up && send_locked(state == PROTO_STATE_EXPLODED ? "explosion" : "snooze")) {
            s_need_trigger = false;
        }
    }
    if (s_resend_new_set) {
        if (state == PROTO_STATE_ARMED && !st.session_active) {
            if (st.link_up && send_locked("new puzzle set")) {
                s_resend_new_set = false;
            }
        } else if (state == PROTO_STATE_IDLE) {
            s_resend_new_set = false;                   /* the next SET_ALARM carries the new set */
        }
    }
}

void bomb_config_service(const AppState &st)
{
    if (!s_ready || xSemaphoreTake(s_lock, pdMS_TO_TICKS(CFG_LOCK_WAIT_MS)) != pdTRUE) {
        return;                                         /* retried on the next 50 ms pass */
    }
    service_locked(st);
    xSemaphoreGive(s_lock);
}

void bomb_config_print_difficulty(void)
{
    HistRecord recs[HIST_LEN];
    const int n = history_recent(recs, HIST_LEN);
    const Difficulty d = difficulty(recs, n);
    unsigned snoozes = 0u;
    unsigned strikes = 0u;
    int i;

    for (i = 0; i < n && i < DIFF_WINDOW; i++) {
        snoozes += recs[i].snoozes;
        strikes += recs[i].strikes;
    }
    Serial.printf("[diff] %d mornings stored, last %d: %u snoozes, %u strikes\n", n, n < DIFF_WINDOW ? n : DIFF_WINDOW,
                  snoozes, strikes);
    Serial.printf("[diff] -> %u modules, %u s: %s\n", (unsigned)d.module_count, (unsigned)d.time_limit_s, d.explanation);
}
