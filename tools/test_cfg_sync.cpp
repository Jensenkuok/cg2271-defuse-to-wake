/* Host-side tests for esp32/defuse_esp32/src/app/cfg_sync.*: matching a BOMB_REPORT to what the ESP32 sent,
 * choosing the report the screen follows, and what is drawn from it (riddle text vs built-in clue). */
#include <stdio.h>
#include <string.h>

#include "cfg_sync.h"
#include <../game/game_dial.h>   /* angle brackets: resolved against -I app, i.e. esp32 src/game, not the repo game/ */

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

/* A config like puzzle_build_config's: dial at position dial_pos, base pre-shifted so the player's code at
 * dial_pos is `answer`. wires/k describe the wire module. */
static proto_bomb_config_t make_cfg(uint8_t id, const uint8_t answer[PROTO_DIAL_DIGITS], unsigned dial_pos, uint8_t wires,
                                    uint8_t k_zero_based)
{
    proto_bomb_config_t c;

    memset(&c, 0, sizeof(c));
    c.module_count = 4;
    c.module_order[0] = PROTO_MODULE_WIRES;
    c.module_order[1] = PROTO_MODULE_TRIPWIRE;
    c.module_order[2] = PROTO_MODULE_LIGHTS;
    c.module_order[3] = PROTO_MODULE_LIGHTS;
    c.module_order[dial_pos] = PROTO_MODULE_DIAL;
    c.time_limit_s = 120;
    dial_code_preshift(answer, dial_pos, c.dial_code);
    c.wire_count = wires;
    c.correct_wire = k_zero_based;
    c.trip_min_cm = 12;
    c.trip_max_cm = 22;
    c.max_strikes = 3;
    c.cfg_id = id;
    return c;
}

static ActivePuzzle make_view(uint8_t pair, unsigned dial_pos, const char *dial_text, const char *wire_text)
{
    ActivePuzzle v;

    memset(&v, 0, sizeof(v));
    v.valid = true;
    v.pair = pair;
    v.dial_pos = (uint8_t)dial_pos;
    snprintf(v.dial_text, sizeof(v.dial_text), "%s", dial_text);
    snprintf(v.wire_text, sizeof(v.wire_text), "%s", wire_text);
    return v;
}

static void add_send(CfgRing *ring, const proto_bomb_config_t &c, const ActivePuzzle &v)
{
    uint8_t pl[PROTO_SIZE_BOMB_CONFIG];

    CHECK(proto_pack_bomb_config(&c, pl, sizeof(pl)) == PROTO_SIZE_BOMB_CONFIG);
    cfg_ring_add(ring, c.cfg_id, pl, v);
}

static proto_bomb_report_t make_report(uint8_t slot, uint8_t status, const proto_bomb_config_t &c)
{
    proto_bomb_report_t r;

    memset(&r, 0, sizeof(r));
    r.slot = slot;
    r.status = status;
    r.cfg = c;
    return r;
}

static const uint8_t k_code_a[PROTO_DIAL_DIGITS] = { 3, 1, 8, 5 };
static const uint8_t k_code_b[PROTO_DIAL_DIGITS] = { 9, 0, 2, 6 };

static void test_id_wrap_skips_zero(void)
{
    uint8_t id = 0;
    unsigned i;
    bool seen_zero = false;

    CHECK(cfg_next_id(0) == 1);
    CHECK(cfg_next_id(1) == 2);
    CHECK(cfg_next_id(254) == 255);
    CHECK(cfg_next_id(255) == 1);                 /* wraps past 0 */
    for (i = 0; i < 600; i++) {
        id = cfg_next_id(id);
        if (id == 0) {
            seen_zero = true;
        }
    }
    CHECK(!seen_zero);
}

static void test_match_by_id_and_bytes(void)
{
    CfgRing ring;
    CfgReport r;
    const proto_bomb_config_t a = make_cfg(5, k_code_a, 1, 4, 2);
    proto_bomb_config_t tampered = a;

    cfg_ring_clear(&ring);
    add_send(&ring, a, make_view(2, 1, "dial riddle A", "wire riddle A"));

    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, a));
    CHECK(r.valid && r.matched);
    CHECK(r.view.pair == 2 && strcmp(r.view.dial_text, "dial riddle A") == 0);

    /* same id, one byte different (the MCX holds something else under that id): unknown, never the ring's text */
    tampered.correct_wire = 3;
    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, tampered));
    CHECK(r.valid && !r.matched);

    /* same bytes, other id */
    tampered = a;
    tampered.cfg_id = 6;
    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_RUNNING, PROTO_REPORT_OK, tampered));
    CHECK(r.valid && !r.matched);

    /* the MCX default config (id 0) was never sent */
    tampered = a;
    tampered.cfg_id = 0;
    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, tampered));
    CHECK(!r.matched);
}

static void test_ring_keeps_only_the_last_sends(void)
{
    CfgRing ring;
    CfgReport r;
    proto_bomb_config_t c[APP_CFG_RING_LEN + 1u];
    unsigned i;

    cfg_ring_clear(&ring);
    for (i = 0; i < APP_CFG_RING_LEN + 1u; i++) {
        c[i] = make_cfg((uint8_t)(10u + i), k_code_a, 0, 4, (uint8_t)(i % 4u));
        add_send(&ring, c[i], make_view((uint8_t)i, 0, "d", "w"));
    }
    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, c[0]));
    CHECK(!r.matched);                            /* overwritten by the 5th send */
    for (i = 1; i < APP_CFG_RING_LEN + 1u; i++) {
        cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, c[i]));
        CHECK(r.matched && r.view.pair == i);
    }
}

static void test_pick_report(void)
{
    CfgReport held;
    CfgReport running;
    CfgRing ring;
    const proto_bomb_config_t a = make_cfg(1, k_code_a, 0, 4, 1);
    const proto_bomb_config_t b = make_cfg(2, k_code_b, 0, 5, 3);

    cfg_ring_clear(&ring);
    memset(&held, 0, sizeof(held));
    memset(&running, 0, sizeof(running));
    CHECK(cfg_pick_report(false, held, running) == NULL);   /* nothing yet: default */
    CHECK(cfg_pick_report(true, held, running) == NULL);

    cfg_report_fill(&held, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, a));
    CHECK(cfg_pick_report(false, held, running) == &held);  /* idle: held */
    CHECK(cfg_pick_report(true, held, running) == &held);   /* bomb shown but no running report yet: best guess is held */

    cfg_report_fill(&running, ring, make_report(PROTO_REPORT_RUNNING, PROTO_REPORT_OK, b));
    CHECK(cfg_pick_report(true, held, running) == &running);   /* a bomb is shown: the running one */
    CHECK(cfg_pick_report(false, held, running) == &held);     /* idle again: held */

    memset(&held, 0, sizeof(held));
    CHECK(cfg_pick_report(false, held, running) == NULL);      /* a running report alone is not used when idle */
    CHECK(cfg_pick_report(true, held, running) == &running);
}

static void test_facts_default_when_unknown(void)
{
    CfgFacts f;
    uint8_t expect[PROTO_DIAL_DIGITS] = { GAME_DEFAULT_DIAL_CODE_0, GAME_DEFAULT_DIAL_CODE_1, GAME_DEFAULT_DIAL_CODE_2,
                                          GAME_DEFAULT_DIAL_CODE_3 };

    cfg_facts(NULL, 0, &f);
    CHECK(!f.from_report);
    CHECK(memcmp(f.dial_code, expect, sizeof(expect)) == 0);
    CHECK(f.dial_text == NULL && f.wire_text == NULL);
    CHECK(f.wire_count == GAME_DEFAULT_WIRE_COUNT && f.wire_k == GAME_DEFAULT_CORRECT_WIRE + 1u);
    CHECK(f.trip_min_cm == GAME_DEFAULT_TRIP_MIN_CM && f.trip_max_cm == GAME_DEFAULT_TRIP_MAX_CM);
    CHECK(f.max_strikes == GAME_DEFAULT_MAX_STRIKES);
}

static void test_facts_dial_text_only_at_dial_pos(void)
{
    CfgRing ring;
    CfgReport r;
    CfgFacts f;
    const unsigned dial_pos = 1;
    const proto_bomb_config_t a = make_cfg(7, k_code_a, dial_pos, 5, 3);
    uint8_t derived[PROTO_DIAL_DIGITS];

    cfg_ring_clear(&ring);
    add_send(&ring, a, make_view(3, dial_pos, "the dial riddle", "the wire riddle"));
    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_RUNNING, PROTO_REPORT_OK, a));

    cfg_facts(&r, (uint8_t)dial_pos, &f);
    CHECK(f.from_report);
    CHECK(f.dial_text != NULL && strcmp(f.dial_text, "the dial riddle") == 0);
    CHECK(memcmp(f.dial_code, k_code_a, sizeof(k_code_a)) == 0);   /* the answer at the dial's own position */

    /* an extra dial module past module_count (an explosion / snooze repeat): same base, other index */
    cfg_facts(&r, 4, &f);
    CHECK(f.dial_text == NULL);
    game_dial_code(a.dial_code, 4, derived);
    CHECK(memcmp(f.dial_code, derived, sizeof(derived)) == 0);
    CHECK(memcmp(f.dial_code, k_code_a, sizeof(k_code_a)) != 0);

    /* any other index inside module_count too */
    cfg_facts(&r, 0, &f);
    CHECK(f.dial_text == NULL);
    game_dial_code(a.dial_code, 0, derived);
    CHECK(memcmp(f.dial_code, derived, sizeof(derived)) == 0);
}

static void test_facts_wire_text_vs_template(void)
{
    CfgRing ring;
    CfgReport r;
    CfgFacts f;
    const proto_bomb_config_t a = make_cfg(9, k_code_b, 0, 6, 4);   /* 6 wires, cut the 5th */

    cfg_ring_clear(&ring);
    add_send(&ring, a, make_view(1, 0, "d", "cut the fifth"));

    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, a));
    cfg_facts(&r, 2, &f);
    CHECK(f.wire_count == 6 && f.wire_k == 5);
    CHECK(f.wire_text != NULL && strcmp(f.wire_text, "cut the fifth") == 0);
    CHECK(f.trip_min_cm == 12 && f.trip_max_cm == 22 && f.max_strikes == 3);

    /* an unmatched report (ESP32 was reset): count and k still from the report, text is the built-in template */
    cfg_ring_clear(&ring);
    cfg_report_fill(&r, ring, make_report(PROTO_REPORT_HELD, PROTO_REPORT_OK, a));
    cfg_facts(&r, 2, &f);
    CHECK(r.valid && !r.matched);
    CHECK(f.from_report && f.wire_text == NULL && f.dial_text == NULL);
    CHECK(f.wire_count == 6 && f.wire_k == 5);
    cfg_facts(&r, 0, &f);
    CHECK(memcmp(f.dial_code, a.dial_code, sizeof(a.dial_code)) == 0);
}

static void test_facts_clamp_bad_report_values(void)
{
    CfgReport r;
    CfgFacts f;

    memset(&r, 0, sizeof(r));
    r.valid = true;
    r.cfg.wire_count = 9;          /* the report is not range-checked by the codec */
    r.cfg.correct_wire = 200;
    cfg_facts(&r, 0, &f);
    CHECK(f.wire_count == GAME_MAX_WIRES);
    CHECK(f.wire_k >= 1 && f.wire_k <= f.wire_count);
    r.cfg.wire_count = 1;
    r.cfg.correct_wire = 0;
    cfg_facts(&r, 0, &f);
    CHECK(f.wire_count == GAME_MIN_WIRES && f.wire_k == 1);
}

int main(void)
{
    test_id_wrap_skips_zero();
    test_match_by_id_and_bytes();
    test_ring_keeps_only_the_last_sends();
    test_pick_report();
    test_facts_default_when_unknown();
    test_facts_dial_text_only_at_dial_pos();
    test_facts_wire_text_vs_template();
    test_facts_clamp_bad_report_values();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
