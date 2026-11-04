/*
 * Defuse to Wake - local estimate of the LIGHTS hold progress (pure, header-only, no Arduino headers,
 * tested by tools/test_light_hold.cpp).
 *
 * Why an estimate: GAME_STATE carries the light level and the target in module_value but progress_pct
 * must stay the overall bomb progress (the web page infers the module total from it, plan decision L10),
 * so the MCX never sends how long the level has been at or above the target. The display therefore
 * replays the MCX rule (game.c light_level / light_expire_grace) from the states it receives:
 *   - the hold starts the first time a received level is >= target;
 *   - a level below the target is forgiven for GAME_LIGHT_GRACE_MS; once it has stayed below that long
 *     the hold is lost and starts again at the next level >= target;
 *   - the hold is complete after GAME_LIGHT_HOLD_MS (the MCX then clears the module; the bar is capped
 *     at 100 until the next state shows the next module).
 * The estimate can be a state-update late, never ahead of the MCX by more than that.
 */
#ifndef LIGHT_HOLD_H
#define LIGHT_HOLD_H

#include <stdbool.h>
#include <stdint.h>

#include "../game/game_config.h"

struct LightHold {
    bool holding;               /* a level >= target was seen and the hold has not been lost */
    bool below;                 /* the level is below the target right now (inside the grace) */
    uint32_t start_ms;          /* when the hold started */
    uint32_t below_since_ms;    /* when the level first dropped below the target */
};

static inline void light_hold_reset(LightHold *h)
{
    h->holding = false;
    h->below = false;
    h->start_ms = 0u;
    h->below_since_ms = 0u;
}

/* Call at every display frame. `known` is false while there is no baseline (target 0) or the current
 * module is not LIGHTS: the tracker resets and 0 is returned. Otherwise returns the hold progress
 * 0..100 and updates h (h->holding tells whether the hold is running, the grace included). */
static inline uint8_t light_hold_update(LightHold *h, bool known, uint8_t level, uint8_t target, uint32_t now_ms)
{
    uint32_t held_ms;

    if (!known || target == 0u) {
        light_hold_reset(h);
        return 0u;
    }
    if (h->below && (uint32_t)(now_ms - h->below_since_ms) >= GAME_LIGHT_GRACE_MS) {
        h->holding = false;                     /* below for longer than the grace: the hold is lost */
        h->below = false;
    }
    if (level >= target) {
        h->below = false;
        if (!h->holding) {
            h->holding = true;
            h->start_ms = now_ms;
        }
    } else if (h->holding && !h->below) {
        h->below = true;
        h->below_since_ms = now_ms;
    }
    if (!h->holding) {
        return 0u;
    }
    held_ms = (uint32_t)(now_ms - h->start_ms);
    if (held_ms >= GAME_LIGHT_HOLD_MS) {
        return (uint8_t)GAME_PERCENT;           /* capped: never past full */
    }
    return (uint8_t)((held_ms * GAME_PERCENT) / GAME_LIGHT_HOLD_MS);
}

#endif /* LIGHT_HOLD_H */
