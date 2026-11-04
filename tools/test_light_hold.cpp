/* Host-side tests for esp32/defuse_esp32/src/app/light_hold.h: the display's estimate of the LIGHTS hold. */
#include <stdio.h>

#include "light_hold.h"

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

#define TARGET 150u
#define BELOW  100u
#define ABOVE  200u

static void test_unknown_baseline_shows_nothing(void)
{
    LightHold h;

    light_hold_reset(&h);
    CHECK(light_hold_update(&h, true, ABOVE, 0u, 1000u) == 0u && !h.holding);         /* target 0 = no baseline */
    CHECK(light_hold_update(&h, false, ABOVE, TARGET, 1000u) == 0u && !h.holding);    /* not the LIGHTS module */
}

static void test_below_target_never_starts(void)
{
    LightHold h;
    uint32_t t;

    light_hold_reset(&h);
    for (t = 0u; t < 10000u; t += 100u) {
        CHECK(light_hold_update(&h, true, BELOW, TARGET, t) == 0u && !h.holding);
    }
}

static void test_hold_fills_over_the_hold_time_and_caps(void)
{
    LightHold h;

    light_hold_reset(&h);
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, 5000u) == 0u && h.holding);                  /* starts at the first state */
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, 5000u + GAME_LIGHT_HOLD_MS / 2u) == 50u);
    CHECK(light_hold_update(&h, true, TARGET, TARGET, 5000u + GAME_LIGHT_HOLD_MS - 1u) == 99u);   /* exactly at target counts */
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, 5000u + GAME_LIGHT_HOLD_MS) == 100u);
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, 5000u + 10u * GAME_LIGHT_HOLD_MS) == 100u);  /* never past full */
}

static void test_short_dip_is_forgiven(void)
{
    LightHold h;
    const uint32_t t0 = 1000u;

    light_hold_reset(&h);
    (void)light_hold_update(&h, true, ABOVE, TARGET, t0);
    CHECK(light_hold_update(&h, true, BELOW, TARGET, t0 + 500u) == 25u && h.holding && h.below);
    CHECK(light_hold_update(&h, true, BELOW, TARGET, t0 + 500u + GAME_LIGHT_GRACE_MS - 1u) > 0u && h.holding);
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, t0 + 500u + GAME_LIGHT_GRACE_MS - 1u) > 0u && !h.below);
    /* the hold kept its original start: it completes GAME_LIGHT_HOLD_MS after t0 */
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, t0 + GAME_LIGHT_HOLD_MS) == 100u);
}

static void test_long_dip_resets_and_restarts(void)
{
    LightHold h;
    const uint32_t t0 = 1000u;
    uint32_t t1;

    light_hold_reset(&h);
    (void)light_hold_update(&h, true, ABOVE, TARGET, t0);
    (void)light_hold_update(&h, true, BELOW, TARGET, t0 + 400u);
    CHECK(light_hold_update(&h, true, BELOW, TARGET, t0 + 400u + GAME_LIGHT_GRACE_MS) == 0u && !h.holding);   /* grace over */
    t1 = t0 + 400u + GAME_LIGHT_GRACE_MS + 50u;
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, t1) == 0u && h.holding);                       /* new hold starts */
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, t1 + GAME_LIGHT_HOLD_MS / 2u) == 50u);
}

static void test_dip_reported_late_still_resets(void)
{
    LightHold h;

    /* the next state only arrives after the grace and already shows a good level: the lost hold is not revived */
    light_hold_reset(&h);
    (void)light_hold_update(&h, true, ABOVE, TARGET, 0u);
    (void)light_hold_update(&h, true, BELOW, TARGET, 600u);
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, 600u + GAME_LIGHT_GRACE_MS + 100u) == 0u && h.holding);
}

static void test_leaving_the_module_resets(void)
{
    LightHold h;

    light_hold_reset(&h);
    (void)light_hold_update(&h, true, ABOVE, TARGET, 0u);
    CHECK(h.holding);
    CHECK(light_hold_update(&h, false, ABOVE, TARGET, 500u) == 0u && !h.holding && !h.below);
}

static void test_clock_wrap(void)
{
    LightHold h;
    const uint32_t t0 = 0xFFFFFF00u;

    light_hold_reset(&h);
    (void)light_hold_update(&h, true, ABOVE, TARGET, t0);
    CHECK(light_hold_update(&h, true, ABOVE, TARGET, t0 + GAME_LIGHT_HOLD_MS / 2u) == 50u);   /* wraps past 2^32 */
}

int main(void)
{
    printf("test_light_hold: hold estimate\n");
    test_unknown_baseline_shows_nothing();
    test_below_target_never_starts();
    test_hold_fills_over_the_hold_time_and_caps();
    test_short_dip_is_forgiven();
    test_long_dip_resets_and_restarts();
    test_dip_reported_late_still_resets();
    test_leaving_the_module_resets();
    test_clock_wrap();
    printf("test_light_hold: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
