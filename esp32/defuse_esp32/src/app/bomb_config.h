/*
 * Defuse to Wake - BOMB_CONFIG for the MCX: difficulty rule + the next unused puzzle pair.
 * Runs in the LINK task (priority 4); the network task only calls bomb_config_send for the SET_ALARM
 * case. All state is guarded by one short mutex (20 ms bound). A bomb snapshots its config when it starts, so a config sent at any
 * time before it (with the SET_ALARM, during the 3 s EXPLODED hold, or while ARMED after a snooze)
 * applies to the NEXT bomb.
 *
 * Why the link task and not the network task: the MCX starts the next bomb 3 s after EXPLODED, and
 * the network task (priority 1) can be stuck in an LLM call for up to 30 s, so a fresh config sent
 * from there could miss that window and the BOOT demo ring could take 30 s to ring. The link task
 * never blocks on the network or on flash; everything it touches here is RAM:
 *   - the puzzle set is read in one short lock of its mutex (puzzle_take_next_pair);
 *   - "used" pair flags are only changed in RAM, the flash write is deferred to the network task
 *     (puzzle_flush_used, dirty flag);
 *   - the difficulty rule reads the history ring under its own short lock (no flash).
 *
 * Triggers (bomb_config_send is also called from the network task for the SET_ALARM case):
 *   - every SET_ALARM sent outside a session (the MCX is IDLE or ARMED, no bomb running);
 *   - inside a session: the MCX state just became EXPLODED, or ARMED after a snooze;
 *   - a new puzzle set while the MCX sits ARMED with an older one;
 *   - the demo RING_NOW (BOOT button), straight from the link task.
 * A pair is "used" when a bomb starts after its config was ACKed; when all four are used the
 * list starts over.
 */
#ifndef BOMB_CONFIG_H
#define BOMB_CONFIG_H

#include <stdbool.h>

#include "app_state.h"

/* Network task, once the puzzle set and the history exist. Until then every call below is a
 * no-op (the link task is already running). */
void bomb_config_init(void);

/* Queue one BOMB_CONFIG (reliable, through the link TX queue) for the next unused pair. False
 * when nothing was queued: another config is still waiting for its ACK, the link TX queue is
 * full, or the puzzle set is busy. why: short tag for the log. Link task or network task. */
bool bomb_config_send(const char *why);

/* Link task, every APP_BOMB_CFG_POLL_MS with a fresh copy of the app state: ACK/failure
 * bookkeeping, pair consumption when a bomb starts, the EXPLODED / snooze / new-set triggers. */
void bomb_config_service(const AppState &st);

/* The difficulty rule applied to the stored mornings (also the briefing's "difficulty" input). */
void bomb_config_difficulty(Difficulty *out);

/* Serial "diff": the difficulty rule applied to the stored mornings. */
void bomb_config_print_difficulty(void);

#endif /* BOMB_CONFIG_H */
