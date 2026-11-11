/* Defuse to Wake - report matching and fallback rules. See cfg_sync.h. */
#include "cfg_sync.h"

#include <string.h>

#include "../game/game_dial.h"   /* game_dial_code(): the MCX validates dials with the same function */

uint8_t cfg_next_id(uint8_t prev)
{
    const uint8_t next = (uint8_t)(prev + 1u);

    return (next == 0u) ? 1u : next;
}

void cfg_ring_clear(CfgRing *ring)
{
    memset(ring, 0, sizeof(*ring));
}

void cfg_ring_add(CfgRing *ring, uint8_t cfg_id, const uint8_t packed[PROTO_SIZE_BOMB_CONFIG], const ActivePuzzle &view)
{
    CfgSend &e = ring->entry[ring->next];

    e.valid = true;
    e.cfg_id = cfg_id;
    memcpy(e.packed, packed, sizeof(e.packed));
    e.view = view;
    ring->next = (uint8_t)((ring->next + 1u) % APP_CFG_RING_LEN);
}

const CfgSend *cfg_ring_find(const CfgRing &ring, uint8_t cfg_id, const uint8_t packed[PROTO_SIZE_BOMB_CONFIG])
{
    unsigned i;

    for (i = 0u; i < APP_CFG_RING_LEN; i++) {
        const CfgSend &e = ring.entry[i];

        if (e.valid && e.cfg_id == cfg_id && memcmp(e.packed, packed, sizeof(e.packed)) == 0) {
            return &e;
        }
    }
    return NULL;
}

void cfg_report_fill(CfgReport *out, const CfgRing &ring, const proto_bomb_report_t &rep)
{
    uint8_t packed[PROTO_SIZE_BOMB_CONFIG];
    const CfgSend *hit = NULL;

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->status = rep.status;
    out->cfg = rep.cfg;
    if (proto_pack_bomb_config(&rep.cfg, packed, sizeof(packed)) != 0u) {
        hit = cfg_ring_find(ring, rep.cfg.cfg_id, packed);
    }
    if (hit != NULL) {
        out->matched = true;
        out->view = hit->view;
    }
}

const CfgReport *cfg_pick_report(bool bomb_shown, const CfgReport &held, const CfgReport &running)
{
    if (bomb_shown && running.valid) {
        return &running;
    }
    return held.valid ? &held : NULL;
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

void cfg_facts(const CfgReport *rep, uint8_t module_idx, CfgFacts *out)
{
    uint8_t base[PROTO_DIAL_DIGITS] = { GAME_DEFAULT_DIAL_CODE_0, GAME_DEFAULT_DIAL_CODE_1, GAME_DEFAULT_DIAL_CODE_2,
                                        GAME_DEFAULT_DIAL_CODE_3 };
    uint8_t wires = APP_DEFAULT_WIRE_COUNT;
    uint8_t wire_k = (uint8_t)(GAME_DEFAULT_CORRECT_WIRE + 1u);      /* game_config.h is zero-based */

    memset(out, 0, sizeof(*out));
    out->trip_min_cm = APP_DEFAULT_TRIP_MIN_CM;
    out->trip_max_cm = APP_DEFAULT_TRIP_MAX_CM;
    out->max_strikes = APP_DEFAULT_MAX_STRIKES;
    if (rep != NULL && rep->valid) {
        out->from_report = true;
        memcpy(base, rep->cfg.dial_code, sizeof(base));
        wires = rep->cfg.wire_count;
        wire_k = (uint8_t)(rep->cfg.correct_wire + 1u);
        out->trip_min_cm = rep->cfg.trip_min_cm;
        out->trip_max_cm = rep->cfg.trip_max_cm;
        out->max_strikes = rep->cfg.max_strikes;
        if (rep->matched && rep->view.valid) {
            /* The riddle is written for the pair's own dial module and its own wires only: a dial module at any other
             * index (a repeat after an explosion or snooze) expects a different code, which the riddle does not describe. */
            if (module_idx == rep->view.dial_pos && rep->view.dial_text[0] != '\0') {
                out->dial_text = rep->view.dial_text;
            }
            if (rep->view.wire_text[0] != '\0') {
                out->wire_text = rep->view.wire_text;
            }
        }
    }
    game_dial_code(base, module_idx, out->dial_code);
    out->wire_count = clamp_u8(wires, GAME_MIN_WIRES, GAME_MAX_WIRES);
    out->wire_k = clamp_u8(wire_k, 1u, out->wire_count);
}
