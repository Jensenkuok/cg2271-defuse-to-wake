/* Host-side tests for game/game.{h,c}. Plain asserts, no framework. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

#define TICK_MS 50u
#define SND_MAX 32

static game_t g;
static uint32_t now;
static game_sound_t snd[SND_MAX];
static int nsnd;
static uint32_t t_session; /* now at the first ACTIVE entry of the last setup */

/* ---- helpers ---- */
static void ev(game_event_type_t type, int32_t value)
{
    game_event_t e;
    e.type = type;
    e.value = value;
    game_handle_event(&g, &e, now);
}

/* ms must be a multiple of TICK_MS. */
static void advance(uint32_t ms)
{
    uint32_t i;
    for (i = 0; i < ms / TICK_MS; i++) {
        now += TICK_MS;
        game_tick(&g, now);
    }
}

static proto_game_state_t view(void)
{
    proto_game_state_t v;
    game_get_view(&g, &v, now);
    return v;
}

static void drain(void)
{
    nsnd = 0;
    while (nsnd < SND_MAX && game_pop_sound(&g, &snd[nsnd])) {
        nsnd++;
    }
}

static game_sound_t last_sound(void)
{
    return snd[nsnd - 1];
}

static void default_cfg(proto_bomb_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->module_count = 3;
    c->module_order[0] = PROTO_MODULE_DIAL;
    c->module_order[1] = PROTO_MODULE_WIRES;
    c->module_order[2] = PROTO_MODULE_TRIPWIRE;
    c->time_limit_s = 180;
    c->dial_code[0] = 4; c->dial_code[1] = 2; c->dial_code[2] = 7; c->dial_code[3] = 1;
    c->wire_count = 4;
    c->correct_wire = 2;
    c->trip_min_cm = 18;
    c->trip_max_cm = 22;
    c->max_strikes = 3;
}

/* Fresh game at `start`, optional config, ring now, run to BOMB_ACTIVE. */
static void start_bomb(uint32_t start, const proto_bomb_config_t *cfg)
{
    now = start;
    game_init(&g, now);
    if (cfg != NULL) {
        CHECK(game_set_config(&g, cfg));
    }
    ev(GAME_EVT_CMD_RING_NOW, 0);
    advance(TICK_MS);
    t_session = now;
    drain();
}

static void solve_dial(void)
{
    static const int code[4] = { 4, 2, 7, 1 };
    int i;
    int k;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < code[i]; k++) { ev(GAME_EVT_ENC_STEP, 1); }
        ev(GAME_EVT_ENC_PRESS, 0);
    }
}

static void solve_wires(void)
{
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_PRESS, 0);
}

static void solve_trip(void)
{
    ev(GAME_EVT_DISTANCE, 20);
    advance(3000);
}

/* ---- tests ---- */
static void test_init_default_view(void)
{
    proto_game_state_t v;
    proto_result_msg_t r;
    game_sound_t s;

    now = 12345;
    game_init(&g, now);
    v = view();
    CHECK(v.state == PROTO_STATE_IDLE && v.module_idx == 0 && v.module_type == 0);
    CHECK(v.time_left_s == 0 && v.strikes == 0 && v.progress_pct == 0 && v.module_value == 0);
    CHECK(game_led_mode(&g) == GAME_LED_OFF);
    CHECK(game_tick_interval_ms(&g, now) == 0);
    CHECK(!game_pop_sound(&g, &s) && !game_pop_result(&g, &r));
    advance(5000);
    CHECK(view().state == PROTO_STATE_IDLE); /* nothing happens on its own */
    ev(GAME_EVT_ENC_STEP, 1);
    ev(GAME_EVT_JOY_PRESS, 0);
    ev(GAME_EVT_TAMPER, 5000);
    drain();
    CHECK(nsnd == 0 && view().state == PROTO_STATE_IDLE);
}

static void test_config_rejects(void)
{
    proto_bomb_config_t c;

    game_init(&g, 0);
    default_cfg(&c); c.module_count = 0;            CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.module_count = 5;            CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.module_order[1] = 0;         CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.module_order[2] = 4;         CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.module_count = 2; c.module_order[2] = 9;
    CHECK(game_set_config(&g, &c));                 /* unused entries are ignored */
    default_cfg(&c); c.dial_code[3] = 10;           CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.trip_min_cm = 22;            CHECK(!game_set_config(&g, &c)); /* min == max */
    default_cfg(&c); c.trip_min_cm = 30;            CHECK(!game_set_config(&g, &c)); /* min > max */
    default_cfg(&c); c.trip_min_cm = 4;             CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.trip_max_cm = 101;           CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.trip_min_cm = 101; c.trip_max_cm = 102; CHECK(!game_set_config(&g, &c));
    default_cfg(&c); c.trip_min_cm = 5; c.trip_max_cm = 100;   CHECK(game_set_config(&g, &c));
    default_cfg(&c); c.correct_wire = 4;            CHECK(!game_set_config(&g, &c)); /* >= wire_count */
    default_cfg(&c); c.wire_count = 2; c.correct_wire = 3; CHECK(!game_set_config(&g, &c)); /* clamped to 3 */
    CHECK(!game_set_config(&g, NULL) && !game_set_config(NULL, &c));
}

static void test_rejected_config_keeps_previous(void)
{
    proto_bomb_config_t c;

    default_cfg(&c);
    c.time_limit_s = 90;
    now = 0;
    game_init(&g, now);
    CHECK(game_set_config(&g, &c));
    c.module_count = 0;
    CHECK(!game_set_config(&g, &c));
    ev(GAME_EVT_CMD_RING_NOW, 0);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE && view().time_left_s == 90);
}

static void test_config_clamps(void)
{
    proto_bomb_config_t c;
    int i;

    default_cfg(&c); c.time_limit_s = 10;
    start_bomb(0, &c);
    CHECK(view().time_left_s == 30);
    default_cfg(&c); c.time_limit_s = 1000;
    start_bomb(0, &c);
    CHECK(view().time_left_s == 600);

    default_cfg(&c); c.module_count = 1; c.module_order[0] = PROTO_MODULE_WIRES; c.wire_count = 1;
    start_bomb(0, &c);
    for (i = 0; i < 10; i++) { ev(GAME_EVT_JOY_RIGHT, 0); }
    CHECK((view().module_value & 0xFF) == 2); /* 1 wire clamped up to 3 */
    default_cfg(&c); c.module_count = 1; c.module_order[0] = PROTO_MODULE_WIRES; c.wire_count = 10;
    start_bomb(0, &c);
    for (i = 0; i < 10; i++) { ev(GAME_EVT_JOY_RIGHT, 0); }
    CHECK((view().module_value & 0xFF) == 5); /* 10 wires clamped down to 6 */

    default_cfg(&c); c.max_strikes = 0;
    start_bomb(0, &c);
    ev(GAME_EVT_ENC_PRESS, 0); /* wrong digit */
    CHECK(view().state == PROTO_STATE_EXPLODED); /* 0 clamped to 1 strike */
    default_cfg(&c); c.max_strikes = 9;
    start_bomb(0, &c);
    for (i = 0; i < 4; i++) { ev(GAME_EVT_ENC_PRESS, 0); }
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE && view().strikes == 4);
    ev(GAME_EVT_ENC_PRESS, 0);
    CHECK(view().state == PROTO_STATE_EXPLODED); /* 9 clamped to 5 */
}

static void test_config_applies_to_next_bomb_only(void)
{
    proto_bomb_config_t c;

    start_bomb(0, NULL);
    default_cfg(&c); c.time_limit_s = 600;
    CHECK(game_set_config(&g, &c));
    CHECK(view().time_left_s == 180);
}

static void test_ring_now_without_time_sync(void)
{
    start_bomb(1000, NULL); /* RING_NOW 0 worked with no TIME_SYNC */
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);

    now = 0;
    game_init(&g, now);
    ev(GAME_EVT_CMD_RING_NOW, 10);
    CHECK(view().state == PROTO_STATE_ARMED && game_led_mode(&g) == GAME_LED_ARMED_DIM_BLUE);
    advance(9950);
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);

    now = 0;
    game_init(&g, now);
    ev(GAME_EVT_CMD_RING_NOW, -5); /* clamped to 0 */
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);

    now = 0;
    game_init(&g, now);
    ev(GAME_EVT_CMD_RING_NOW, 99999); /* clamped to 3600 s */
    advance(3599950);
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);

    ev(GAME_EVT_CMD_RING_NOW, 5); /* ignored while the bomb is live */
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE && view().time_left_s == 180);
}

static void test_ring_now_reschedules_when_armed(void)
{
    now = 0;
    game_init(&g, now);
    ev(GAME_EVT_CMD_RING_NOW, 100);
    advance(1000);
    ev(GAME_EVT_CMD_RING_NOW, 1);
    advance(900);
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(100);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);
}

static void test_set_alarm(void)
{
    now = 5000;
    game_init(&g, now);
    CHECK(!game_set_alarm(&g, 1000010));          /* no time sync yet */
    CHECK(view().state == PROTO_STATE_IDLE);
    CHECK(!game_set_time(&g, 0, now));
    CHECK(game_set_time(&g, 1000000, now));
    CHECK(!game_set_alarm(&g, 999999));           /* in the past */
    CHECK(!game_set_alarm(&g, 1000000 + 8u * 86400u)); /* too far ahead */
    CHECK(view().state == PROTO_STATE_IDLE);
    CHECK(game_set_alarm(&g, 1000010));
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(9950);
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);
    CHECK(!game_set_alarm(&g, 1000100));          /* not while a bomb is live */
}

static void test_set_alarm_after_time_has_passed(void)
{
    now = 1000;
    game_init(&g, now);
    CHECK(game_set_time(&g, 2000000, now));
    advance(3000); /* unix now = 2000003 */
    CHECK(!game_set_alarm(&g, 2000002));
    CHECK(game_set_alarm(&g, 2000010)); /* deadline = sync point + 10 s */
    advance(6950); /* now = 10950 */
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(TICK_MS); /* 11000 */
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);
}

static void test_armed_to_active_view(void)
{
    proto_game_state_t v;

    start_bomb(0, NULL);
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.module_idx == 0 && v.module_type == PROTO_MODULE_DIAL);
    CHECK(v.time_left_s == 180 && v.strikes == 0 && v.progress_pct == 0 && v.module_value == 0);
    CHECK(game_led_mode(&g) == GAME_LED_ACTIVE_RED_BLINK);
    CHECK(game_tick_interval_ms(&g, now) == 1000);
}

static void test_dial_module(void)
{
    proto_game_state_t v;
    int k;

    start_bomb(0, NULL);
    ev(GAME_EVT_ENC_STEP, -1);
    CHECK((view().module_value & 0xFF) == 9); /* wraps below 0 */
    ev(GAME_EVT_ENC_STEP, 1);
    CHECK((view().module_value & 0xFF) == 0); /* wraps above 9 */
    ev(GAME_EVT_ENC_STEP, 13);
    CHECK((view().module_value & 0xFF) == 3);
    ev(GAME_EVT_ENC_STEP, -13);
    CHECK((view().module_value & 0xFF) == 0);
    ev(GAME_EVT_ENC_STEP, 0);
    drain();
    CHECK(nsnd == 4 && snd[0] == GAME_SOUND_CLICK && snd[3] == GAME_SOUND_CLICK); /* 0 step is silent */

    for (k = 0; k < 3; k++) { ev(GAME_EVT_ENC_STEP, 1); }
    ev(GAME_EVT_ENC_PRESS, 0); /* 3 != 4: strike, value reset */
    v = view();
    CHECK(v.strikes == 1 && (v.module_value & 0xFF) == 0 && (v.module_value >> 8) == 0);
    drain();
    CHECK(last_sound() == GAME_SOUND_STRIKE);

    for (k = 0; k < 4; k++) { ev(GAME_EVT_ENC_STEP, 1); }
    ev(GAME_EVT_ENC_PRESS, 0);
    drain();
    CHECK(last_sound() == GAME_SOUND_DIGIT_OK);
    v = view();
    CHECK(v.module_value == (0 | (1 << 8))); /* digit index 1, value reset */

    for (k = 0; k < 2; k++) { ev(GAME_EVT_ENC_STEP, 1); }
    ev(GAME_EVT_ENC_PRESS, 0);
    for (k = 0; k < 5; k++) { ev(GAME_EVT_ENC_STEP, 1); }
    CHECK(view().module_value == (5 | (2 << 8)));
    for (k = 0; k < 2; k++) { ev(GAME_EVT_ENC_STEP, 1); }
    ev(GAME_EVT_ENC_PRESS, 0);
    for (k = 0; k < 1; k++) { ev(GAME_EVT_ENC_STEP, 1); }
    drain();
    ev(GAME_EVT_ENC_PRESS, 0); /* 4th digit correct */
    drain();
    CHECK(nsnd == 1 && last_sound() == GAME_SOUND_MODULE_CLEARED);
    v = view();
    CHECK(v.module_idx == 1 && v.module_type == PROTO_MODULE_WIRES && v.progress_pct == 33);
    CHECK(v.strikes == 1 && v.module_value == 0);
}

static void test_inputs_for_other_modules_ignored(void)
{
    start_bomb(0, NULL); /* current module: DIAL */
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_PRESS, 0);
    ev(GAME_EVT_DISTANCE, 20);
    drain();
    CHECK(nsnd == 0 && view().strikes == 0 && view().module_value == 0);
    solve_dial(); /* now WIRES */
    drain();
    ev(GAME_EVT_ENC_STEP, 1);
    ev(GAME_EVT_ENC_PRESS, 0);
    ev(GAME_EVT_DISTANCE, 20);
    drain();
    CHECK(nsnd == 0 && view().strikes == 0 && view().module_value == 0);
}

static void test_wires_module(void)
{
    proto_game_state_t v;
    int k;

    start_bomb(0, NULL);
    solve_dial();
    drain();
    ev(GAME_EVT_JOY_LEFT, 0); /* already at wire 0: clamped, silent */
    drain();
    CHECK(nsnd == 0 && (view().module_value & 0xFF) == 0);
    for (k = 0; k < 10; k++) { ev(GAME_EVT_JOY_RIGHT, 0); }
    drain();
    CHECK((view().module_value & 0xFF) == 3 && nsnd == 3); /* 4 wires: no wrap */
    for (k = 0; k < 3; k++) { ev(GAME_EVT_JOY_LEFT, 0); }
    ev(GAME_EVT_JOY_PRESS, 0); /* wire 0 is wrong */
    v = view();
    CHECK(v.strikes == 1 && v.module_value == (0 | (1 << 8)));
    drain();
    CHECK(last_sound() == GAME_SOUND_STRIKE);
    ev(GAME_EVT_JOY_PRESS, 0); /* already cut */
    drain();
    CHECK(nsnd == 1 && snd[0] == GAME_SOUND_DENIED && view().strikes == 1);
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_PRESS, 0); /* wire 2 is correct */
    drain();
    CHECK(last_sound() == GAME_SOUND_MODULE_CLEARED);
    v = view();
    CHECK(v.module_idx == 2 && v.module_type == PROTO_MODULE_TRIPWIRE && v.progress_pct == 66);
    CHECK(v.module_value == 0);
}

static void test_wire_cut_mask_encoding(void)
{
    proto_game_state_t v;

    start_bomb(0, NULL);
    solve_dial();
    ev(GAME_EVT_JOY_PRESS, 0);   /* cut 0 (wrong) */
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_RIGHT, 0);
    ev(GAME_EVT_JOY_PRESS, 0);   /* cut 3 (wrong) */
    v = view();
    CHECK(v.module_value == (3 | (0x09 << 8)) && v.strikes == 2);
}

static void test_tripwire_module(void)
{
    proto_game_state_t v;

    start_bomb(0, NULL);
    solve_dial();
    solve_wires();
    CHECK(view().module_type == PROTO_MODULE_TRIPWIRE);
    ev(GAME_EVT_ENC_PRESS, 0); /* wrong module */
    CHECK(view().strikes == 0);

    ev(GAME_EVT_DISTANCE, 17); /* just below the band */
    CHECK(view().module_value == 17);
    ev(GAME_EVT_DISTANCE, 18); /* inclusive lower bound */
    advance(1500);
    v = view();
    CHECK(v.module_value == (18 | (50 << 8)));
    ev(GAME_EVT_DISTANCE, 23); /* above the band: reset, no strike */
    v = view();
    CHECK(v.module_value == (23 | (0 << 8)) && v.strikes == 0);

    ev(GAME_EVT_DISTANCE, 22); /* inclusive upper bound */
    advance(2950);
    CHECK(view().module_type == PROTO_MODULE_TRIPWIRE && view().state == PROTO_STATE_BOMB_ACTIVE);
    ev(GAME_EVT_DISTANCE, 401); /* >400 is noise: resets the hold */
    CHECK(view().module_value == 255);
    ev(GAME_EVT_DISTANCE, 300);
    CHECK(view().module_value == 255); /* low byte saturates */

    ev(GAME_EVT_DISTANCE, 20);
    advance(2950);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);
    advance(TICK_MS);
    v = view();
    CHECK(v.state == PROTO_STATE_DEFUSED && v.progress_pct == 100);
    CHECK(v.module_idx == 2 && v.module_type == PROTO_MODULE_TRIPWIRE);
}

static void test_defuse_result_and_return_to_idle(void)
{
    proto_result_msg_t r;
    uint32_t t_defused;
    game_sound_t s;

    start_bomb(0, NULL);
    advance(2000);
    ev(GAME_EVT_ENC_PRESS, 0); /* one strike */
    solve_dial();
    drain(); /* keep the 8-deep sound FIFO from dropping the final sound */
    solve_wires();
    drain();
    solve_trip();
    t_defused = now;
    CHECK(view().state == PROTO_STATE_DEFUSED);
    CHECK(game_led_mode(&g) == GAME_LED_DEFUSED_GREEN);
    CHECK(view().time_left_s == 0 && game_tick_interval_ms(&g, now) == 0);
    drain();
    CHECK(last_sound() == GAME_SOUND_DEFUSED);
    CHECK(game_pop_result(&g, &r));
    CHECK(r.outcome == PROTO_OUTCOME_DEFUSED && r.strikes == 1 && r.snoozes == 0);
    CHECK(r.duration_s == (uint16_t)((t_defused - t_session) / 1000u));
    CHECK(!game_pop_result(&g, &r));

    ev(GAME_EVT_ENC_PRESS, 0); /* ignored while DEFUSED */
    ev(GAME_EVT_CMD_CANCEL, 0);
    CHECK(view().state == PROTO_STATE_DEFUSED);
    CHECK(!game_pop_sound(&g, &s));
    advance(29950);
    CHECK(view().state == PROTO_STATE_DEFUSED);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_IDLE && game_led_mode(&g) == GAME_LED_OFF);
    CHECK(view().module_type == 0 && view().progress_pct == 0);
}

static void test_tamper(void)
{
    start_bomb(0, NULL);
    ev(GAME_EVT_TAMPER, 1499);
    CHECK(view().strikes == 0);
    ev(GAME_EVT_TAMPER, 1500);
    CHECK(view().strikes == 1);
    advance(1950);
    ev(GAME_EVT_TAMPER, 3000); /* inside the 2 s cooldown */
    CHECK(view().strikes == 1);
    advance(TICK_MS);
    ev(GAME_EVT_TAMPER, 3000); /* cooldown over */
    CHECK(view().strikes == 2);

    now = 0;
    game_init(&g, now);
    ev(GAME_EVT_TAMPER, 5000); /* IDLE: ignored */
    ev(GAME_EVT_CMD_RING_NOW, 10);
    ev(GAME_EVT_TAMPER, 5000); /* ARMED: ignored */
    drain();
    CHECK(nsnd == 0);
    advance(10000);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE && view().strikes == 0);
}

static void test_explosion_by_strikes_and_second_bomb(void)
{
    proto_result_msg_t r;
    proto_game_state_t v;
    uint32_t t_boom;

    start_bomb(0, NULL);
    advance(2000);
    ev(GAME_EVT_ENC_PRESS, 0);
    ev(GAME_EVT_ENC_PRESS, 0);
    ev(GAME_EVT_ENC_PRESS, 0);
    t_boom = now;
    drain();
    CHECK(nsnd == 3 && snd[0] == GAME_SOUND_STRIKE && snd[1] == GAME_SOUND_STRIKE &&
          snd[2] == GAME_SOUND_EXPLODED); /* the fatal strike plays only EXPLODED */
    CHECK(view().state == PROTO_STATE_EXPLODED && game_led_mode(&g) == GAME_LED_EXPLODED_RED_WHITE);
    CHECK(view().time_left_s == 0 && game_tick_interval_ms(&g, now) == 0);
    CHECK(game_pop_result(&g, &r));
    CHECK(r.outcome == PROTO_OUTCOME_EXPLODED && r.strikes == 3 && r.snoozes == 0);
    CHECK(r.duration_s == (uint16_t)((t_boom - t_session) / 1000u));

    ev(GAME_EVT_ENC_PRESS, 0);
    ev(GAME_EVT_CMD_SNOOZE, 0);
    drain();
    CHECK(nsnd == 0 && view().state == PROTO_STATE_EXPLODED);
    advance(2950);
    CHECK(view().state == PROTO_STATE_EXPLODED);
    advance(TICK_MS);
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.strikes == 0 && v.time_left_s == 180);
    CHECK(v.module_idx == 0 && v.progress_pct == 0 && v.module_type == PROTO_MODULE_DIAL);

    /* The new bomb has 4 modules: DIAL, WIRES, TRIPWIRE and the extra DIAL. */
    solve_dial();
    CHECK(view().module_type == PROTO_MODULE_WIRES && view().progress_pct == 25);
    solve_wires();
    CHECK(view().module_type == PROTO_MODULE_TRIPWIRE && view().progress_pct == 50);
    solve_trip();
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.module_idx == 3);
    CHECK(v.module_type == PROTO_MODULE_DIAL && v.progress_pct == 75);
    solve_dial();
    CHECK(view().state == PROTO_STATE_DEFUSED && view().progress_pct == 100);
    CHECK(game_pop_result(&g, &r) && r.outcome == PROTO_OUTCOME_DEFUSED);
    CHECK(r.strikes == 3); /* session total keeps counting across bombs */
    CHECK(!game_pop_result(&g, &r));
}

static void test_explosion_by_timeout(void)
{
    proto_result_msg_t r;
    proto_game_state_t v;

    start_bomb(0, NULL);
    advance(179950);
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.time_left_s == 1);
    advance(TICK_MS);
    v = view();
    CHECK(v.state == PROTO_STATE_EXPLODED && v.time_left_s == 0);
    drain();
    CHECK(nsnd == 1 && snd[0] == GAME_SOUND_EXPLODED);
    CHECK(game_pop_result(&g, &r));
    CHECK(r.outcome == PROTO_OUTCOME_EXPLODED && r.duration_s == 180 && r.strikes == 0);
}

static void test_module_cap_of_four(void)
{
    proto_bomb_config_t c;
    int i;

    start_bomb(0, NULL); /* 3 base modules, two explosions -> still 4 */
    for (i = 0; i < 2; i++) {
        ev(GAME_EVT_ENC_PRESS, 0); ev(GAME_EVT_ENC_PRESS, 0); ev(GAME_EVT_ENC_PRESS, 0);
        advance(3000);
    }
    solve_dial();
    CHECK(view().progress_pct == 25);

    default_cfg(&c);
    c.module_count = 4;
    c.module_order[3] = PROTO_MODULE_WIRES;
    start_bomb(0, &c);
    solve_dial();
    CHECK(view().progress_pct == 25);
    ev(GAME_EVT_TAMPER, 2000);
    advance(2000);
    ev(GAME_EVT_TAMPER, 2000);
    advance(2000);
    ev(GAME_EVT_TAMPER, 2000); /* third strike */
    CHECK(view().state == PROTO_STATE_EXPLODED);
    advance(3000);
    solve_dial();
    CHECK(view().progress_pct == 25); /* 4 base + 1 extra, capped at 4 */
}

/* Order {X} with one strike allowed: explode, then check the extra module's type. */
static void extra_type_case(uint8_t first, uint8_t expected_extra)
{
    proto_bomb_config_t c;
    proto_game_state_t v;

    default_cfg(&c);
    c.module_count = 1;
    c.module_order[0] = first;
    c.max_strikes = 1;
    start_bomb(0, &c);
    ev(GAME_EVT_TAMPER, 2000); /* any module: one strike explodes */
    CHECK(view().state == PROTO_STATE_EXPLODED);
    advance(3000);
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.module_type == first);
    if (first == PROTO_MODULE_DIAL) {
        solve_dial();
    } else if (first == PROTO_MODULE_WIRES) {
        solve_wires();
    } else {
        solve_trip();
    }
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.module_idx == 1 && v.module_type == expected_extra);
}

static void test_extra_module_type_rule(void)
{
    proto_bomb_config_t c;
    proto_game_state_t v;

    extra_type_case(PROTO_MODULE_DIAL, PROTO_MODULE_WIRES);
    extra_type_case(PROTO_MODULE_WIRES, PROTO_MODULE_TRIPWIRE);
    extra_type_case(PROTO_MODULE_TRIPWIRE, PROTO_MODULE_DIAL);

    /* Two explosions: TRIPWIRE, DIAL, WIRES. */
    default_cfg(&c);
    c.module_count = 1;
    c.module_order[0] = PROTO_MODULE_TRIPWIRE;
    c.max_strikes = 1;
    start_bomb(0, &c);
    ev(GAME_EVT_TAMPER, 2000);
    advance(3000);
    ev(GAME_EVT_TAMPER, 2000); /* cooldown was reset with the new bomb */
    advance(3000);
    solve_trip();
    CHECK(view().module_idx == 1 && view().module_type == PROTO_MODULE_DIAL);
    solve_dial();
    v = view();
    CHECK(v.module_idx == 2 && v.module_type == PROTO_MODULE_WIRES && v.progress_pct == 66);
}

static void test_snooze_twice_then_denied(void)
{
    proto_game_state_t v;
    proto_result_msg_t r;

    start_bomb(0, NULL);
    ev(GAME_EVT_CMD_SNOOZE, 0);
    v = view();
    CHECK(v.state == PROTO_STATE_ARMED && v.time_left_s == 0 && v.strikes == 0);
    CHECK(game_led_mode(&g) == GAME_LED_ARMED_DIM_BLUE);
    ev(GAME_EVT_CMD_SNOOZE, 0); /* ignored while ARMED */
    advance(299950);
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(TICK_MS);
    v = view();
    CHECK(v.state == PROTO_STATE_BOMB_ACTIVE && v.time_left_s == 180 && v.module_idx == 0);

    ev(GAME_EVT_ENC_LONG_PRESS, 0); /* second snooze, via the encoder */
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(300000);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);

    drain();
    ev(GAME_EVT_CMD_SNOOZE, 0); /* third: denied */
    drain();
    CHECK(nsnd == 1 && snd[0] == GAME_SOUND_DENIED && view().state == PROTO_STATE_BOMB_ACTIVE);
    ev(GAME_EVT_ENC_LONG_PRESS, 0);
    drain();
    CHECK(nsnd == 1 && snd[0] == GAME_SOUND_DENIED);

    /* Two snoozes -> 3 + 2 modules capped at 4. */
    solve_dial();
    CHECK(view().progress_pct == 25);
    solve_wires();
    solve_trip();
    solve_dial();
    CHECK(view().state == PROTO_STATE_DEFUSED);
    CHECK(game_pop_result(&g, &r));
    CHECK(r.outcome == PROTO_OUTCOME_DEFUSED && r.snoozes == 2);
    CHECK(r.duration_s >= 600); /* the session spans the snooze waits */
}

static void test_cancel(void)
{
    proto_result_msg_t r;

    now = 0;
    game_init(&g, now);
    ev(GAME_EVT_CMD_CANCEL, 0); /* IDLE: nothing */
    CHECK(view().state == PROTO_STATE_IDLE);
    ev(GAME_EVT_CMD_RING_NOW, 100);
    ev(GAME_EVT_CMD_CANCEL, 0);
    CHECK(view().state == PROTO_STATE_IDLE);
    advance(200000);
    CHECK(view().state == PROTO_STATE_IDLE); /* the alarm is gone */

    start_bomb(0, NULL);
    ev(GAME_EVT_CMD_SNOOZE, 0);
    advance(300000); /* snoozed bomb is back: session has 1 snooze, 1 extra */
    ev(GAME_EVT_ENC_PRESS, 0);
    ev(GAME_EVT_CMD_CANCEL, 0);
    CHECK(view().state == PROTO_STATE_IDLE && view().strikes == 0 && view().module_type == 0);
    CHECK(!game_pop_result(&g, &r)); /* cancel never produces a RESULT */

    ev(GAME_EVT_CMD_RING_NOW, 0); /* a new session starts clean: 3 modules */
    advance(TICK_MS);
    solve_dial();
    CHECK(view().progress_pct == 33);
    ev(GAME_EVT_CMD_CANCEL, 0);
    CHECK(view().state == PROTO_STATE_IDLE);
}

static void test_tick_interval(void)
{
    uint32_t end;

    start_bomb(0, NULL);
    end = now + 180000u;
    CHECK(game_tick_interval_ms(&g, end - 120000u) == 1000); /* 120 s left */
    CHECK(game_tick_interval_ms(&g, end - 60000u) == 1000);
    CHECK(game_tick_interval_ms(&g, end - 10050u) == 1000);
    CHECK(game_tick_interval_ms(&g, end - 10000u) == 1000);  /* formula meets 1000 at 10 s */
    CHECK(game_tick_interval_ms(&g, end - 5000u) == 575);
    CHECK(game_tick_interval_ms(&g, end - 1000u) == 235);
    CHECK(game_tick_interval_ms(&g, end) == 150);
    CHECK(game_tick_interval_ms(&g, end + 500u) == 150);   /* never below the floor */

    now = 0;
    game_init(&g, now);
    CHECK(game_tick_interval_ms(&g, now) == 0);
    ev(GAME_EVT_CMD_RING_NOW, 50);
    CHECK(game_tick_interval_ms(&g, now) == 0);             /* ARMED */
}

static void test_view_version(void)
{
    uint32_t v0;

    now = 0;
    game_init(&g, now);
    v0 = game_view_version(&g);
    ev(GAME_EVT_ENC_STEP, 1); /* ignored in IDLE */
    advance(1000);
    CHECK(game_view_version(&g) == v0);
    CHECK(game_set_time(&g, 1000000, now));
    CHECK(game_view_version(&g) == v0);
    CHECK(game_set_alarm(&g, 1000100));
    CHECK(game_view_version(&g) == v0 + 1); /* IDLE -> ARMED */

    start_bomb(0, NULL);
    v0 = game_view_version(&g);
    advance(900); /* 180 s left rounds up: no visible change */
    CHECK(game_view_version(&g) == v0);
    advance(TICK_MS); /* 179.05 s left: still shows 180 */
    advance(TICK_MS); /* 179.00 s left: shows 179 */
    CHECK(game_view_version(&g) == v0 + 1); /* crossed into 179 s */
    v0 = game_view_version(&g);
    ev(GAME_EVT_ENC_STEP, 1);
    CHECK(game_view_version(&g) == v0 + 1);
    ev(GAME_EVT_JOY_RIGHT, 0); /* wrong module: no change */
    CHECK(game_view_version(&g) == v0 + 1);
    ev(GAME_EVT_ENC_PRESS, 0); /* strike + value reset */
    CHECK(game_view_version(&g) == v0 + 2);
}

static void test_view_version_tracks_tripwire_hold(void)
{
    uint32_t v0;

    start_bomb(0, NULL);
    solve_dial();
    solve_wires();
    ev(GAME_EVT_DISTANCE, 20);
    v0 = game_view_version(&g);
    advance(TICK_MS); /* hold 50 ms: 1 % */
    CHECK(game_view_version(&g) == v0 + 1);
}

static void test_sound_fifo_overflow(void)
{
    int i;

    start_bomb(0, NULL);
    for (i = 0; i < 7; i++) { ev(GAME_EVT_ENC_STEP, 1); }
    ev(GAME_EVT_ENC_PRESS, 0);       /* 8th entry: STRIKE */
    ev(GAME_EVT_ENC_STEP, 1);        /* dropped: FIFO full */
    ev(GAME_EVT_ENC_PRESS, 0);       /* dropped */
    drain();
    CHECK(nsnd == 8);
    for (i = 0; i < 7; i++) { CHECK(snd[i] == GAME_SOUND_CLICK); }
    CHECK(snd[7] == GAME_SOUND_STRIKE);
    CHECK(!game_pop_sound(&g, &snd[0]));
}

static void test_result_fifo_overflow(void)
{
    proto_bomb_config_t c;
    proto_result_msg_t r;
    int i;

    default_cfg(&c);
    c.module_count = 1;
    c.module_order[0] = PROTO_MODULE_DIAL;
    c.max_strikes = 1;
    start_bomb(0, &c);
    for (i = 0; i < 5; i++) {
        ev(GAME_EVT_ENC_PRESS, 0); /* wrong digit: explode */
        advance(3000);
    }
    for (i = 1; i <= 4; i++) {
        CHECK(game_pop_result(&g, &r));
        CHECK(r.outcome == PROTO_OUTCOME_EXPLODED && r.strikes == i); /* oldest kept */
    }
    CHECK(!game_pop_result(&g, &r)); /* the fifth was dropped */
}

static void test_wraparound_alarm_hold_and_timeout(void)
{
    proto_bomb_config_t c;
    uint32_t base = 0xFFFFFFFFu - 5000u;
    int i;

    default_cfg(&c);
    c.module_count = 1;
    c.module_order[0] = PROTO_MODULE_TRIPWIRE;
    c.time_limit_s = 30;

    /* ARMED deadline and tripwire hold both cross the 32-bit wrap. */
    now = base;
    game_init(&g, now);
    CHECK(game_set_config(&g, &c));
    ev(GAME_EVT_CMD_RING_NOW, 10);
    advance(9950);
    CHECK(view().state == PROTO_STATE_ARMED);
    CHECK(now < base); /* the counter wrapped at +5000 ms */
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE && view().time_left_s == 30);
    ev(GAME_EVT_DISTANCE, 20);
    advance(2950);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_DEFUSED);

    /* Bomb timer expires after the wrap. */
    now = base;
    game_init(&g, now);
    CHECK(game_set_config(&g, &c));
    ev(GAME_EVT_CMD_RING_NOW, 0);
    advance(TICK_MS);
    for (i = 0; i < 4; i++) { advance(5000); }  /* crosses the wrap */
    CHECK(now < base);
    CHECK(view().time_left_s == 10 && game_tick_interval_ms(&g, now) == 1000);
    advance(9950);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE && view().time_left_s == 1);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_EXPLODED);

    /* set_time / set_alarm across the wrap. */
    now = base;
    game_init(&g, now);
    CHECK(game_set_time(&g, 3000000, now));
    CHECK(game_set_alarm(&g, 3000010));
    advance(9950);
    CHECK(view().state == PROTO_STATE_ARMED);
    advance(TICK_MS);
    CHECK(view().state == PROTO_STATE_BOMB_ACTIVE);
}

int main(void)
{
    test_init_default_view();
    test_config_rejects();
    test_rejected_config_keeps_previous();
    test_config_clamps();
    test_config_applies_to_next_bomb_only();
    test_ring_now_without_time_sync();
    test_ring_now_reschedules_when_armed();
    test_set_alarm();
    test_set_alarm_after_time_has_passed();
    test_armed_to_active_view();
    test_dial_module();
    test_inputs_for_other_modules_ignored();
    test_wires_module();
    test_wire_cut_mask_encoding();
    test_tripwire_module();
    test_defuse_result_and_return_to_idle();
    test_tamper();
    test_explosion_by_strikes_and_second_bomb();
    test_explosion_by_timeout();
    test_module_cap_of_four();
    test_extra_module_type_rule();
    test_snooze_twice_then_denied();
    test_cancel();
    test_tick_interval();
    test_view_version();
    test_view_version_tracks_tripwire_hold();
    test_sound_fifo_overflow();
    test_result_fifo_overflow();
    test_wraparound_alarm_hold_and_timeout();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
