/* Defuse to Wake - bomb game engine. See game.h and docs/game.md. */
#include "game.h"

#include <string.h>

/* ---- time helpers (wraparound-safe) ---- */
static bool time_reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static uint32_t ms_until(uint32_t end, uint32_t now)
{
    int32_t d = (int32_t)(end - now);
    return (d > 0) ? (uint32_t)d : 0u;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* ---- defaults ---- */
static void load_default_config(proto_bomb_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->module_count = GAME_DEFAULT_MODULE_COUNT;
    c->module_order[0] = PROTO_MODULE_DIAL;
    c->module_order[1] = PROTO_MODULE_WIRES;
    c->module_order[2] = PROTO_MODULE_TRIPWIRE;
    c->time_limit_s = GAME_DEFAULT_TIME_LIMIT_S;
    c->dial_code[0] = GAME_DEFAULT_DIAL_CODE_0;
    c->dial_code[1] = GAME_DEFAULT_DIAL_CODE_1;
    c->dial_code[2] = GAME_DEFAULT_DIAL_CODE_2;
    c->dial_code[3] = GAME_DEFAULT_DIAL_CODE_3;
    c->wire_count = GAME_DEFAULT_WIRE_COUNT;
    c->correct_wire = GAME_DEFAULT_CORRECT_WIRE;
    c->trip_min_cm = GAME_DEFAULT_TRIP_MIN_CM;
    c->trip_max_cm = GAME_DEFAULT_TRIP_MAX_CM;
    c->max_strikes = GAME_DEFAULT_MAX_STRIKES;
}

/* ---- output FIFOs ---- */
static void push_sound(game_t *g, game_sound_t s)
{
    if (g->sound_count >= GAME_SOUND_FIFO_DEPTH) {
        return; /* newest dropped */
    }
    g->sounds[(g->sound_head + g->sound_count) % GAME_SOUND_FIFO_DEPTH] = s;
    g->sound_count++;
}

static void push_result(game_t *g, uint8_t outcome, uint32_t now_ms)
{
    proto_result_msg_t r;
    uint32_t dur_s = (uint32_t)(now_ms - g->session_start_ms) / GAME_MS_PER_S;

    r.outcome = outcome;
    r.duration_s = (uint16_t)clamp_u32(dur_s, 0u, UINT16_MAX);
    r.strikes = (uint8_t)clamp_u32(g->session_strikes, 0u, UINT8_MAX);
    r.snoozes = g->snoozes;
    if (g->result_count >= GAME_RESULT_FIFO_DEPTH) {
        return; /* newest dropped */
    }
    g->results[(g->result_head + g->result_count) % GAME_RESULT_FIFO_DEPTH] = r;
    g->result_count++;
}

/* ---- view ---- */
static uint16_t hold_pct(const game_t *g, uint32_t now_ms)
{
    uint32_t elapsed;

    if (!g->trip_holding) {
        return 0u;
    }
    elapsed = clamp_u32((uint32_t)(now_ms - g->trip_hold_start_ms), 0u, GAME_TRIP_HOLD_MS);
    return (uint16_t)(elapsed * GAME_PERCENT / GAME_TRIP_HOLD_MS);
}

static int16_t module_value(const game_t *g, uint32_t now_ms)
{
    uint32_t lo;
    uint32_t hi;

    switch (g->modules[g->cleared]) {
    case PROTO_MODULE_DIAL:
        lo = g->dial_value;
        hi = g->dial_digit;
        break;
    case PROTO_MODULE_WIRES:
        lo = g->wire_cursor;
        hi = g->wire_cut_mask;
        break;
    case PROTO_MODULE_TRIPWIRE:
        lo = g->trip_distance_cm;
        hi = hold_pct(g, now_ms);
        break;
    default:
        return 0;
    }
    return (int16_t)(lo | (hi << GAME_VALUE_HI_SHIFT));
}

static void compute_view(const game_t *g, uint32_t now_ms, proto_game_state_t *v)
{
    bool has_bomb = (g->state == PROTO_STATE_BOMB_ACTIVE || g->state == PROTO_STATE_DEFUSED ||
                     g->state == PROTO_STATE_EXPLODED) && g->module_total > 0u;
    uint8_t idx;

    memset(v, 0, sizeof(*v));
    v->state = g->state;
    if (has_bomb) {
        idx = (g->cleared < g->module_total) ? g->cleared : (uint8_t)(g->module_total - 1u);
        v->module_idx = idx;
        v->module_type = g->modules[idx];
        v->strikes = g->bomb_strikes;
        v->progress_pct = (uint8_t)((uint32_t)g->cleared * GAME_PERCENT / g->module_total);
    }
    if (g->state == PROTO_STATE_BOMB_ACTIVE) {
        v->time_left_s = (uint16_t)((ms_until(g->bomb_end_ms, now_ms) + GAME_MS_PER_S - 1u) / GAME_MS_PER_S);
        v->module_value = module_value(g, now_ms);
    }
}

static bool view_equal(const proto_game_state_t *a, const proto_game_state_t *b)
{
    return a->state == b->state && a->module_idx == b->module_idx &&
           a->module_type == b->module_type && a->time_left_s == b->time_left_s &&
           a->strikes == b->strikes && a->progress_pct == b->progress_pct &&
           a->module_value == b->module_value;
}

/* Called at the end of every mutator: bump the version if the view changed. */
static void sync_view(game_t *g, uint32_t now_ms)
{
    proto_game_state_t v;

    compute_view(g, now_ms, &v);
    if (!view_equal(&v, &g->view_cache)) {
        g->view_cache = v;
        g->view_version++;
    }
}

/* ---- session / state transitions ---- */
static void reset_session(game_t *g)
{
    g->session_active = false;
    g->session_strikes = 0u;
    g->snoozes = 0u;
    g->extras = 0u;
}

static void to_idle(game_t *g)
{
    g->state = PROTO_STATE_IDLE;
    reset_session(g);
    g->module_total = 0u;
    g->cleared = 0u;
    g->bomb_strikes = 0u;
}

static void init_module(game_t *g)
{
    g->dial_value = 0u;
    g->dial_digit = 0u;
    g->wire_cursor = 0u;
    g->wire_cut_mask = 0u;
    g->trip_distance_cm = 0u;
    g->trip_holding = false;
}

/* Start a fresh bomb from the current config plus one extra module per extra. */
static void enter_bomb(game_t *g, uint32_t now_ms)
{
    uint8_t total;
    uint8_t i;
    uint8_t prev;

    g->bomb_cfg = g->cfg;
    total = g->bomb_cfg.module_count;
    memset(g->modules, 0, sizeof(g->modules));
    for (i = 0u; i < total; i++) {
        g->modules[i] = g->bomb_cfg.module_order[i];
    }
    for (i = 0u; i < g->extras && total < PROTO_MAX_MODULES; i++) {
        prev = g->modules[total - 1u];
        g->modules[total] = (uint8_t)((prev % GAME_MODULE_TYPE_COUNT) + 1u);
        total++;
    }
    g->module_total = total;
    g->cleared = 0u;
    g->bomb_strikes = 0u;
    g->bomb_end_ms = now_ms + (uint32_t)g->bomb_cfg.time_limit_s * GAME_MS_PER_S;
    g->tamper_cooling = false;
    init_module(g);
    if (!g->session_active) {
        g->session_active = true;
        g->session_start_ms = now_ms;
        g->session_strikes = 0u;
    }
    g->state = PROTO_STATE_BOMB_ACTIVE;
}

static void explode(game_t *g, uint32_t now_ms)
{
    g->state = PROTO_STATE_EXPLODED;
    g->state_end_ms = now_ms + GAME_EXPLODED_HOLD_MS;
    g->extras++;
    push_result(g, PROTO_OUTCOME_EXPLODED, now_ms);
    push_sound(g, GAME_SOUND_EXPLODED);
}

static void defuse(game_t *g, uint32_t now_ms)
{
    g->state = PROTO_STATE_DEFUSED;
    g->state_end_ms = now_ms + GAME_DEFUSED_HOLD_MS;
    push_result(g, PROTO_OUTCOME_DEFUSED, now_ms);
    push_sound(g, GAME_SOUND_DEFUSED);
}

/* A strike; the fatal one plays only EXPLODED (the terminal sound replaces it). */
static void add_strike(game_t *g, uint32_t now_ms)
{
    g->bomb_strikes++;
    g->session_strikes++;
    if (g->bomb_strikes >= g->bomb_cfg.max_strikes) {
        explode(g, now_ms);
    } else {
        push_sound(g, GAME_SOUND_STRIKE);
    }
}

/* The final module plays only DEFUSED; others play MODULE_CLEARED. */
static void module_cleared(game_t *g, uint32_t now_ms)
{
    g->cleared++;
    if (g->cleared >= g->module_total) {
        defuse(g, now_ms);
        return;
    }
    push_sound(g, GAME_SOUND_MODULE_CLEARED);
    init_module(g);
}

/* ---- module logic ---- */
static void dial_step(game_t *g, int32_t step)
{
    int32_t base = (int32_t)GAME_DIAL_BASE;
    int32_t delta = step % base; /* keeps the sign; -9..9 */

    if (step == 0) {
        return;
    }
    g->dial_value = (uint8_t)(((int32_t)g->dial_value + delta + base) % base);
    push_sound(g, GAME_SOUND_CLICK);
}

static void dial_press(game_t *g, uint32_t now_ms)
{
    if (g->dial_value != g->bomb_cfg.dial_code[g->dial_digit]) {
        g->dial_value = 0u;
        add_strike(g, now_ms);
        return;
    }
    g->dial_digit++;
    g->dial_value = 0u;
    if (g->dial_digit >= PROTO_DIAL_DIGITS) {
        module_cleared(g, now_ms);
    } else {
        push_sound(g, GAME_SOUND_DIGIT_OK);
    }
}

static void wire_move(game_t *g, int32_t dir)
{
    int32_t next = (int32_t)g->wire_cursor + dir;

    if (next < 0 || next >= (int32_t)g->bomb_cfg.wire_count) {
        return; /* clamped, no wrap */
    }
    g->wire_cursor = (uint8_t)next;
    push_sound(g, GAME_SOUND_CLICK);
}

static void wire_press(game_t *g, uint32_t now_ms)
{
    uint8_t bit = (uint8_t)(1u << g->wire_cursor);

    if ((g->wire_cut_mask & bit) != 0u) {
        push_sound(g, GAME_SOUND_DENIED);
        return;
    }
    g->wire_cut_mask |= bit;
    if (g->wire_cursor == g->bomb_cfg.correct_wire) {
        module_cleared(g, now_ms);
    } else {
        add_strike(g, now_ms);
    }
}

static void trip_distance(game_t *g, int32_t cm, uint32_t now_ms)
{
    bool in_band = cm >= (int32_t)g->bomb_cfg.trip_min_cm && cm <= (int32_t)g->bomb_cfg.trip_max_cm &&
                   cm <= GAME_TRIP_MAX_VALID_CM;

    g->trip_distance_cm = (uint8_t)((cm < 0) ? 0 : ((cm > GAME_VIEW_MAX_DISTANCE_CM) ? GAME_VIEW_MAX_DISTANCE_CM : cm));
    if (!in_band) {
        g->trip_holding = false;
    } else if (!g->trip_holding) {
        g->trip_holding = true;
        g->trip_hold_start_ms = now_ms;
    }
}

static void tamper(game_t *g, int32_t mg, uint32_t now_ms)
{
    if (mg < GAME_TAMPER_THRESHOLD_MG) {
        return;
    }
    if (g->tamper_cooling && !time_reached(now_ms, g->tamper_cooldown_end_ms)) {
        return;
    }
    g->tamper_cooling = true;
    g->tamper_cooldown_end_ms = now_ms + GAME_TAMPER_COOLDOWN_MS;
    add_strike(g, now_ms);
}

static void snooze(game_t *g, uint32_t now_ms)
{
    if (g->snoozes >= GAME_MAX_SNOOZES) {
        push_sound(g, GAME_SOUND_DENIED);
        return;
    }
    g->snoozes++;
    g->extras++;
    g->bomb_strikes = 0u;
    g->state = PROTO_STATE_ARMED;
    g->alarm_deadline_ms = now_ms + GAME_SNOOZE_MS;
}

static void ring_now(game_t *g, int32_t delay_s, uint32_t now_ms)
{
    uint32_t s = (uint32_t)((delay_s < 0) ? 0 : ((delay_s > GAME_RING_NOW_MAX_S) ? GAME_RING_NOW_MAX_S : delay_s));

    if (g->state != PROTO_STATE_IDLE && g->state != PROTO_STATE_ARMED) {
        return;
    }
    g->alarm_deadline_ms = now_ms + s * GAME_MS_PER_S;
    g->state = PROTO_STATE_ARMED;
}

/* Module inputs apply only to the current module; tamper to any module. */
static void module_event(game_t *g, const game_event_t *ev, uint32_t now_ms)
{
    uint8_t type = g->modules[g->cleared];

    switch (ev->type) {
    case GAME_EVT_ENC_STEP:
        if (type == PROTO_MODULE_DIAL) { dial_step(g, ev->value); }
        break;
    case GAME_EVT_ENC_PRESS:
        if (type == PROTO_MODULE_DIAL) { dial_press(g, now_ms); }
        break;
    case GAME_EVT_JOY_LEFT:
        if (type == PROTO_MODULE_WIRES) { wire_move(g, -1); }
        break;
    case GAME_EVT_JOY_RIGHT:
        if (type == PROTO_MODULE_WIRES) { wire_move(g, 1); }
        break;
    case GAME_EVT_JOY_PRESS:
        if (type == PROTO_MODULE_WIRES) { wire_press(g, now_ms); }
        break;
    case GAME_EVT_DISTANCE:
        if (type == PROTO_MODULE_TRIPWIRE) { trip_distance(g, ev->value, now_ms); }
        break;
    case GAME_EVT_TAMPER:
        tamper(g, ev->value, now_ms);
        break;
    default:
        break;
    }
}

/* ---- public API ---- */
void game_init(game_t *g, uint32_t now_ms)
{
    if (g == NULL) {
        return;
    }
    memset(g, 0, sizeof(*g));
    g->state = PROTO_STATE_IDLE;
    g->last_now_ms = now_ms;
    load_default_config(&g->cfg);
    g->bomb_cfg = g->cfg;
    compute_view(g, now_ms, &g->view_cache);
}

bool game_set_config(game_t *g, const proto_bomb_config_t *cfg)
{
    proto_bomb_config_t c;
    uint8_t i;

    if (g == NULL || cfg == NULL) {
        return false;
    }
    c = *cfg;
    if (c.module_count < GAME_MIN_MODULES || c.module_count > PROTO_MAX_MODULES) {
        return false;
    }
    for (i = 0u; i < c.module_count; i++) {
        if (c.module_order[i] < GAME_MODULE_TYPE_MIN || c.module_order[i] > GAME_MODULE_TYPE_MAX) {
            return false;
        }
    }
    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        if (c.dial_code[i] >= GAME_DIAL_BASE) {
            return false;
        }
    }
    if (c.trip_min_cm >= c.trip_max_cm ||
        c.trip_min_cm < GAME_TRIP_BOUND_MIN_CM || c.trip_min_cm > GAME_TRIP_BOUND_MAX_CM ||
        c.trip_max_cm < GAME_TRIP_BOUND_MIN_CM || c.trip_max_cm > GAME_TRIP_BOUND_MAX_CM) {
        return false;
    }
    c.time_limit_s = (uint16_t)clamp_u32(c.time_limit_s, GAME_MIN_TIME_LIMIT_S, GAME_MAX_TIME_LIMIT_S);
    c.wire_count = (uint8_t)clamp_u32(c.wire_count, GAME_MIN_WIRES, GAME_MAX_WIRES);
    c.max_strikes = (uint8_t)clamp_u32(c.max_strikes, GAME_MIN_STRIKES, GAME_MAX_STRIKES_LIMIT);
    if (c.correct_wire >= c.wire_count) {
        return false;
    }
    g->cfg = c;
    return true;
}

bool game_set_time(game_t *g, uint32_t unix_s, uint32_t now_ms)
{
    if (g == NULL || unix_s == 0u) {
        return false;
    }
    g->last_now_ms = now_ms;
    g->have_time = true;
    g->sync_unix = unix_s;
    g->sync_ms = now_ms;
    return true;
}

bool game_set_alarm(game_t *g, uint32_t alarm_unix)
{
    uint32_t unix_now;

    if (g == NULL || !g->have_time) {
        return false;
    }
    if (g->state != PROTO_STATE_IDLE && g->state != PROTO_STATE_ARMED) {
        return false;
    }
    unix_now = g->sync_unix + (uint32_t)(g->last_now_ms - g->sync_ms) / GAME_MS_PER_S;
    if (alarm_unix < unix_now || alarm_unix - unix_now > GAME_MAX_ALARM_AHEAD_S) {
        return false;
    }
    /* Relative to the sync point, so the stale last_now_ms adds no error. */
    g->alarm_deadline_ms = g->sync_ms + (alarm_unix - g->sync_unix) * GAME_MS_PER_S;
    g->state = PROTO_STATE_ARMED;
    sync_view(g, g->last_now_ms);
    return true;
}

void game_handle_event(game_t *g, const game_event_t *ev, uint32_t now_ms)
{
    if (g == NULL || ev == NULL) {
        return;
    }
    g->last_now_ms = now_ms;
    switch (ev->type) {
    case GAME_EVT_CMD_RING_NOW:
        ring_now(g, ev->value, now_ms);
        break;
    case GAME_EVT_CMD_CANCEL:
        if (g->state == PROTO_STATE_ARMED || g->state == PROTO_STATE_BOMB_ACTIVE) {
            to_idle(g);
        }
        break;
    case GAME_EVT_CMD_SNOOZE:
    case GAME_EVT_ENC_LONG_PRESS:
        if (g->state == PROTO_STATE_BOMB_ACTIVE) {
            snooze(g, now_ms);
        }
        break;
    default:
        if (g->state == PROTO_STATE_BOMB_ACTIVE) {
            module_event(g, ev, now_ms);
        }
        break;
    }
    sync_view(g, now_ms);
}

void game_tick(game_t *g, uint32_t now_ms)
{
    if (g == NULL) {
        return;
    }
    g->last_now_ms = now_ms;
    switch (g->state) {
    case PROTO_STATE_ARMED:
        if (time_reached(now_ms, g->alarm_deadline_ms)) {
            enter_bomb(g, now_ms);
        }
        break;
    case PROTO_STATE_BOMB_ACTIVE:
        /* Tripwire first, so a hold completing on the deadline tick still wins. */
        if (g->modules[g->cleared] == PROTO_MODULE_TRIPWIRE && g->trip_holding &&
            (uint32_t)(now_ms - g->trip_hold_start_ms) >= GAME_TRIP_HOLD_MS) {
            module_cleared(g, now_ms);
        }
        if (g->state == PROTO_STATE_BOMB_ACTIVE && time_reached(now_ms, g->bomb_end_ms)) {
            explode(g, now_ms);
        }
        break;
    case PROTO_STATE_EXPLODED:
        if (time_reached(now_ms, g->state_end_ms)) {
            enter_bomb(g, now_ms);
        }
        break;
    case PROTO_STATE_DEFUSED:
        if (time_reached(now_ms, g->state_end_ms)) {
            to_idle(g);
        }
        break;
    default:
        break;
    }
    sync_view(g, now_ms);
}

void game_get_view(const game_t *g, proto_game_state_t *out, uint32_t now_ms)
{
    if (g == NULL || out == NULL) {
        return;
    }
    compute_view(g, now_ms, out);
}

uint32_t game_view_version(const game_t *g)
{
    return (g != NULL) ? g->view_version : 0u;
}

uint16_t game_tick_interval_ms(const game_t *g, uint32_t now_ms)
{
    uint32_t left;

    if (g == NULL || g->state != PROTO_STATE_BOMB_ACTIVE) {
        return 0u;
    }
    left = ms_until(g->bomb_end_ms, now_ms);
    if (left > GAME_TICK_SLOW_AFTER_MS) {
        return (uint16_t)GAME_TICK_SLOW_MS;
    }
    return (uint16_t)(GAME_TICK_FAST_BASE_MS + left * GAME_TICK_FAST_SPAN_MS / GAME_TICK_SLOW_AFTER_MS);
}

game_led_t game_led_mode(const game_t *g)
{
    if (g == NULL) {
        return GAME_LED_OFF;
    }
    switch (g->state) {
    case PROTO_STATE_ARMED:       return GAME_LED_ARMED_DIM_BLUE;
    case PROTO_STATE_BOMB_ACTIVE: return GAME_LED_ACTIVE_RED_BLINK;
    case PROTO_STATE_DEFUSED:     return GAME_LED_DEFUSED_GREEN;
    case PROTO_STATE_EXPLODED:    return GAME_LED_EXPLODED_RED_WHITE;
    default:                      return GAME_LED_OFF;
    }
}

bool game_pop_sound(game_t *g, game_sound_t *s)
{
    if (g == NULL || s == NULL || g->sound_count == 0u) {
        return false;
    }
    *s = g->sounds[g->sound_head];
    g->sound_head = (uint8_t)((g->sound_head + 1u) % GAME_SOUND_FIFO_DEPTH);
    g->sound_count--;
    return true;
}

bool game_pop_result(game_t *g, proto_result_msg_t *r)
{
    if (g == NULL || r == NULL || g->result_count == 0u) {
        return false;
    }
    *r = g->results[g->result_head];
    g->result_head = (uint8_t)((g->result_head + 1u) % GAME_RESULT_FIFO_DEPTH);
    g->result_count--;
    return true;
}
