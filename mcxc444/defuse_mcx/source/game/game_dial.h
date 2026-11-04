/*
 * Code a DIAL module expects at a given module index. Header-only and pure C99 so the MCX game
 * (validation) and the ESP32 display (clue, hint) compile the SAME derivation: if they ever
 * disagreed the module would be unwinnable.
 */
#ifndef GAME_DIAL_H
#define GAME_DIAL_H

#include <stdint.h>

#include "game_config.h"
#include "protocol.h"

/*
 * Code a DIAL module expects at module index `module_idx` (GAME_STATE.module_idx):
 * index 0 is the configured code, index k > 0 shifts every digit by
 * GAME_DIAL_REPEAT_STEP * k (mod 10).
 */
static inline void game_dial_code(const uint8_t base[PROTO_DIAL_DIGITS], uint8_t module_idx,
                                  uint8_t out[PROTO_DIAL_DIGITS])
{
    uint8_t i;

    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        out[i] = (uint8_t)((base[i] + GAME_DIAL_REPEAT_STEP * module_idx) % GAME_DIAL_BASE);
    }
}

#endif /* GAME_DIAL_H */
