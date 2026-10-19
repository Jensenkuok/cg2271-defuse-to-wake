/*
 * Defuse to Wake - BOMB_CONFIG for the MCX: difficulty rule + the next unused puzzle pair.
 * Network task only. A bomb snapshots its config when it starts, so a config sent at any time
 * before it (with the SET_ALARM, during the 3 s EXPLODED hold, or while ARMED after a snooze)
 * applies to the NEXT bomb.
 *
 * Triggers (bomb_config_send is also called from network_task for the SET_ALARM case):
 *   - every SET_ALARM sent outside a session (the MCX is IDLE or ARMED, no bomb running);
 *   - inside a session: the MCX state just became EXPLODED, or ARMED after a snooze;
 *   - a new puzzle set while the MCX sits ARMED with an older one;
 *   - the demo RING_NOW (BOOT button), through handle_ring_request in the network task.
 * A pair is "used" when a bomb starts after its config was ACKed; when all four are used the
 * list starts over.
 */
#ifndef BOMB_CONFIG_H
#define BOMB_CONFIG_H

#include <stdbool.h>

#include "app_state.h"

void bomb_config_init(void);

/* Queue one BOMB_CONFIG (reliable, through the link task) for the next unused pair. False when
 * nothing was queued: another config is still waiting for its ACK, the link TX queue is full,
 * or the puzzle set is busy. why: short tag for the log. */
bool bomb_config_send(const char *why);

/* Every network-task loop with a fresh copy of the app state: ACK/failure bookkeeping, pair
 * consumption when a bomb starts, the EXPLODED / snooze / new-set triggers. */
void bomb_config_service(const AppState &st);

/* Serial "diff": the difficulty rule applied to the stored mornings. */
void bomb_config_print_difficulty(void);

#endif /* BOMB_CONFIG_H */
