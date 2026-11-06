/* Defuse to Wake - BOMB_CONFIG sender. See bomb_config.h. */
#include "bomb_config.h"

#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#include "cfg_sync.h"
#include "config.h"
#include "history.h"
#include "link_task.h"
#include "settings.h"
#include "puzzle_gen.h"
#include "puzzle_logic.h"
#include "../shared/protocol.h"

namespace {

#define STATE_UNKNOWN 0xFFu

/* One BOMB_CONFIG waiting for the MCX's HELD report. The text it was built from lives in s_ring (by id). */
struct Pending {
    bool active;
    uint8_t id;
    uint32_t since_ms;
};

Pending s_pending;
CfgRing s_ring;             /* the last APP_CFG_RING_LEN sends: id + packed bytes + riddle text, to match reports against */
uint8_t s_last_id;          /* cfg_id of the latest send (0 = none yet; the next one is 1) */
bool s_consume_wait;        /* a RUNNING report named a pair that is not marked spent yet (puzzle lock was busy) */
int s_consume_pair;
uint8_t s_consume_id;
bool s_consumed_valid;      /* some cfg_id already consumed its pair: once per cfg_id */
uint8_t s_consumed_id;
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

/* BOMB_REPORTs from link_deliver wait here until bomb_config_service (same task, next pass) handles them: the
 * ring they are matched against is behind s_lock, which a 20 ms wait may miss, and a report must never be lost to
 * that. One slot per report kind (a newer one replaces an older unhandled one: the old one is stale by definition,
 * the MCX does the same on its side). A spinlock, not a mutex: a struct copy only. */
struct InboxSlot {
    bool full;
    proto_bomb_report_t rep;
};
portMUX_TYPE s_inbox_mux = portMUX_INITIALIZER_UNLOCKED;
InboxSlot s_inbox[2];       /* [0] HELD, [1] RUNNING (the order they are handled in: the held config comes first) */

/* Score of the last good history read (DIFF_NO_SCORE until there is one). One int, written by whichever
 * task computed a good score: a stale or torn-free word is all the fallback needs. */
int s_last_good_score = DIFF_NO_SCORE;

/* ok (optional) is false when the history lock timed out: the zero records read then are NOT "no history",
 * and the caller that reacts to a level change must not act on that score. The returned difficulty is
 * still usable: on a timeout it reuses the last good score (difficulty_resolve), so a bomb is never built
 * from the made-up score 50 of an unreadable history. */
Difficulty current_difficulty(bool *ok)
{
    HistRecord recs[HIST_LEN];                          /* the whole ring: the score replays all stored mornings */
    const int n = history_recent(recs, HIST_LEN);
    const bool history_ok = (n > 0) || (history_count() == 0);

    if (ok != NULL) {
        *ok = history_ok;
    }
    return difficulty_resolve(recs, n, settings_get_difficulty_mode(), history_ok, &s_last_good_score);
}

void make_view(ActivePuzzle *v, int pair, uint32_t gen, const Puzzle &d, const Puzzle &w, unsigned dial_pos)
{
    memset(v, 0, sizeof(*v));
    v->valid = true;
    v->pair = (uint8_t)pair;
    v->set_gen = gen;
    memcpy(v->dial_code, d.dial_code, sizeof(v->dial_code));
    v->dial_pos = (uint8_t)dial_pos;
    v->wire_count = w.wire_count;
    v->wire_k = w.wire_k;
    v->dial_source = d.source;
    v->wire_source = w.source;
    snprintf(v->dial_text, sizeof(v->dial_text), "%s", d.text);
    /* a wire riddle is at most PUZZLE_WIRE_TEXT_MAX characters (validate_puzzle); cut explicitly */
    snprintf(v->wire_text, sizeof(v->wire_text), "%.*s", PUZZLE_WIRE_TEXT_MAX, w.text);
}

/* The transport ACK ends the send attempt only: the MCX may still refuse the config (game_set_config) or drop it on
 * a full queue. "The MCX holds this pair" is decided by its HELD report (handle_report), so nothing is shown here. */
void on_ack(void)
{
    if (s_pending.active) {
        Serial.printf("[cfg] BOMB_CONFIG id %u ACKed (link level), waiting for the MCX's HELD report\n", (unsigned)s_pending.id);
    }
}

void on_fail(void)
{
    if (s_pending.active) {
        s_pending.active = false;
        Serial.printf("[cfg] BOMB_CONFIG id %u not ACKed after the retries: the MCX keeps its previous config\n",
                      (unsigned)s_pending.id);
    }
}

/* "pair 2", "default" (the MCX's built-in config, id 0) or "unknown pair" (not in the ring: sent before an ESP32 reset). */
const char *pair_text(const CfgReport &r, char *buf, size_t n)
{
    if (r.matched) {
        snprintf(buf, n, "pair %u", (unsigned)r.view.pair);
    } else {
        snprintf(buf, n, "%s", r.cfg.cfg_id == 0u ? "default" : "unknown pair");
    }
    return buf;
}

/* One MCX report: keep it in app_state for the display and the web page, settle the pending send, spend the pair.
 * s_lock held by the caller (or still the only user of the ring: see bomb_config_service). */
void handle_report(const proto_bomb_report_t &rep)
{
    CfgReport r;
    char pair[16];
    const uint8_t id = rep.cfg.cfg_id;

    cfg_report_fill(&r, s_ring, rep);
    app_state_set_report(rep.slot, r);
    if (rep.slot == PROTO_REPORT_HELD) {
        if (rep.status == PROTO_REPORT_REJECTED) {
            /* the report's config is what the MCX KEPT; the refused one is our latest send */
            Serial.printf("[cfg] MCX rejected BOMB_CONFIG id %u\n", (unsigned)s_last_id);
            if (s_pending.active && s_pending.id == s_last_id) {
                s_pending.active = false;
            }
        } else if (s_pending.active && r.matched && s_pending.id == id) {
            s_pending.active = false;                   /* confirmed: the MCX holds exactly what we sent */
        }
        Serial.printf("[cfg] MCX holds id %u (%s)\n", (unsigned)id, pair_text(r, pair, sizeof(pair)));
    } else {
        if (r.matched) {
            Serial.printf("[cfg] bomb running id %u (pair %u)\n", (unsigned)id, (unsigned)r.view.pair);
            /* The pair is spent when a bomb REALLY started with it, once per cfg_id (an explosion or snooze re-bomb of
             * the same config reports it again). Retried every pass while the puzzle lock is busy (D4). An unmatched
             * report consumes nothing: it names a config this ESP32 does not remember. */
            if (!(s_consumed_valid && s_consumed_id == id)) {
                s_consume_wait = true;
                s_consume_pair = r.view.pair;
                s_consume_id = id;
            }
        } else {
            Serial.printf("[cfg] bomb running id %u (unknown pair: built-in riddle)\n", (unsigned)id);
        }
    }
}

/* Hand over what link_deliver queued, oldest kind first. */
void process_inbox(void)
{
    uint8_t i;

    for (i = 0; i < 2u; i++) {
        proto_bomb_report_t rep;
        bool have;

        taskENTER_CRITICAL(&s_inbox_mux);
        have = s_inbox[i].full;
        if (have) {
            rep = s_inbox[i].rep;
            s_inbox[i].full = false;
        }
        taskEXIT_CRITICAL(&s_inbox_mux);
        if (have) {
            handle_report(rep);
        }
    }
}

void try_consume(void)
{
    if (s_consume_wait && puzzle_consume_pair(s_consume_pair)) {
        s_consume_wait = false;
        s_consumed_valid = true;
        s_consumed_id = s_consume_id;
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
    cfg_ring_clear(&s_ring);
    s_ready = (s_lock != NULL);
}

bool bomb_config_difficulty(Difficulty *out)
{
    bool ok = false;

    if (out != NULL) {
        *out = current_difficulty(&ok);
    }
    return ok;
}

/* s_lock held by the caller. */
static bool send_locked(const char *why)
{
    proto_bomb_config_t cfg;
    Difficulty diff;
    Puzzle dial;
    Puzzle wire;
    uint8_t pl[PROTO_SIZE_BOMB_CONFIG];
    ActivePuzzle view;
    uint32_t gen = 0u;
    int pair = 0;
    const uint8_t id = cfg_next_id(s_last_id);

    if (s_pending.active && (uint32_t)(millis() - s_pending.since_ms) < APP_CONFIG_PENDING_MS) {
        return false;                                   /* one at a time: wait for the MCX's HELD report */
    }
    if (link_tx_space() < 1u || !puzzle_take_next_pair(&dial, &wire, &pair, &gen)) {
        return false;
    }
    diff = current_difficulty(NULL);
    /* First random word moves the tripwire band (O11), the second shuffles the module order of this bomb (D5):
     * the MCX accepts any order of valid module types, and the OLED / web draw by the type it reports. */
    puzzle_build_config(diff, dial, wire, esp_random(), esp_random(), &cfg);
    cfg.cfg_id = id;                                    /* the MCX echoes it in its BOMB_REPORT: that is how a report is matched to this send */
    if (proto_pack_bomb_config(&cfg, pl, sizeof(pl)) == 0u || !link_send(PROTO_MSG_BOMB_CONFIG, pl, PROTO_SIZE_BOMB_CONFIG)) {
        return false;
    }
    make_view(&view, pair, gen, dial, wire, module_order_find(cfg.module_order, PROTO_MODULE_DIAL));
    cfg_ring_add(&s_ring, id, pl, view);                /* packed bytes + text: what a matching report will be drawn from */
    s_last_id = id;
    s_pending.active = true;
    s_pending.id = id;
    s_pending.since_ms = millis();
    Serial.printf("[cfg] BOMB_CONFIG queued (%s): id %u, pair %d, %u modules, %us, %u wires (cut %u), dial %u%u%u%u at #%u, tripwire %u-%ucm [%s]\n",
                  why, (unsigned)id, pair, (unsigned)cfg.module_count, (unsigned)cfg.time_limit_s, (unsigned)cfg.wire_count,
                  (unsigned)cfg.correct_wire + 1u, (unsigned)dial.dial_code[0], (unsigned)dial.dial_code[1],
                  (unsigned)dial.dial_code[2], (unsigned)dial.dial_code[3], module_order_find(cfg.module_order, PROTO_MODULE_DIAL) + 1u,
                  (unsigned)cfg.trip_min_cm,
                  (unsigned)cfg.trip_max_cm, diff.explanation);
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
    process_inbox();
    try_consume();
    if (s_pending.active && (uint32_t)(millis() - s_pending.since_ms) >= APP_CONFIG_PENDING_MS) {
        /* No matching HELD report in time: the config was lost, refused unseen or the MCX is busy. The screens keep
         * following the MCX's last reports, so nothing wrong is shown; a new config may be sent. */
        Serial.printf("[cfg] no HELD report for BOMB_CONFIG id %u within %u ms: send failed\n", (unsigned)s_pending.id,
                      (unsigned)APP_CONFIG_PENDING_MS);
        s_pending.active = false;
    }

    gen = puzzle_set_generation();
    if (gen != s_seen_set_gen) {
        s_seen_set_gen = gen;
        s_resend_new_set = true;
    }
    /* The network task upgraded or refilled a puzzle of the pair the MCX already holds. The MCX keeps the old
     * answers and the OLED shows the text copied at send time, so the two only agree as long as the pair is
     * sent again WHOLE: dial and wire come from the same take, never one new puzzle beside an old answer.
     * Same rule as a new set: ARMED outside a session only (below); the network task never touches the
     * reserved pair during a session, so nothing is lost by waiting. */
    if (puzzle_reserved_changed_take()) {
        s_resend_new_set = true;
    }
    if (!st.game_valid) {
        return;
    }
    state = st.game.state;

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
        }
        /* In any other state (IDLE, a session, ringing) the flag simply stays set until the next ARMED outside a
         * session. It is NOT cleared in IDLE: the ESP32 can still see IDLE for up to ~1 s after SET_ALARM plus the
         * config went out, and an upgrade of the reserved pair raised in that window would be lost. A redundant
         * resend of an unchanged pair is harmless. */
    }
}

void bomb_config_note_report(const proto_bomb_report_t &rep)
{
    const uint8_t i = (rep.slot == PROTO_REPORT_RUNNING) ? 1u : 0u;

    taskENTER_CRITICAL(&s_inbox_mux);
    s_inbox[i].rep = rep;
    s_inbox[i].full = true;
    taskEXIT_CRITICAL(&s_inbox_mux);
}

void bomb_config_service(const AppState &st)
{
    if (!s_ready) {
        /* Before the network task has built the puzzle set a report is still shown at once (an ESP32 reset in the
         * middle of a bomb): the ring is empty then and nothing else touches these statics yet, so every report is
         * simply "unknown pair" and no lock is needed. */
        process_inbox();
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(CFG_LOCK_WAIT_MS)) != pdTRUE) {
        return;                                         /* retried on the next 50 ms pass; the reports wait in the inbox */
    }
    service_locked(st);
    xSemaphoreGive(s_lock);
}

void bomb_config_print_difficulty(void)
{
    HistRecord recs[HIST_LEN];
    const int n = history_recent(recs, HIST_LEN);
    const Difficulty d = difficulty(recs, n, settings_get_difficulty_mode());
    const LevelSpec spec = difficulty_level_spec(d.level);
    unsigned snoozes = 0u;
    unsigned explosions = 0u;
    int i;

    for (i = 0; i < n; i++) {
        snoozes += recs[i].snoozes;
        explosions += recs[i].explosions;
    }
    Serial.printf("[diff] %d mornings stored: %u explosions, %u snoozes\n", n, explosions, snoozes);
    Serial.printf("[diff] mode %s, score %u -> level %s\n", difficulty_mode_name(d.mode), (unsigned)d.score,
                  pr_level_name(d.level));
    Serial.printf("[diff] bomb: %u modules, %u s, %u-%u wires [%s]\n", (unsigned)d.module_count,
                  (unsigned)d.time_limit_s, (unsigned)spec.wire_min, (unsigned)spec.wire_max, d.explanation);
}
