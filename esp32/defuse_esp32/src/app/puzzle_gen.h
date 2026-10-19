/*
 * Defuse to Wake - nightly puzzle generation (network task only, lowest priority).
 *
 * A SET is 4 dial + 4 wire puzzles; pair i = dial[i] + wire[i] is used for one bomb. Per puzzle:
 *   1. the ESP32 picks the answer (esp_random): a 4-digit code, or a wire 1..N;
 *   2. the SETTER model writes a riddle for it (JSON {"puzzle":".."}), validate_puzzle() checks it;
 *   3. a different CHECKER model gets only the riddle and must answer with the same answer;
 *   4. an exact match keeps the puzzle (source AI, verified); up to 3 attempts, then the
 *      template bank (correct by construction, host-tested).
 * The LLM never decides an answer. The set is stored in flash; if a run produces no verified
 * puzzle at all the previous set is kept, and with no set at all the bank is used.
 *
 * The run is a small state machine: puzzle_gen_service() does at most ONE LLM call (<= 30 s)
 * per pass, so the network task keeps serving the link, the alarm sync and the BOMB_CONFIG
 * triggers between calls. It never runs during a bomb session or close to the alarm.
 */
#ifndef PUZZLE_GEN_H
#define PUZZLE_GEN_H

#include <stdbool.h>
#include <stdint.h>

#include "puzzle_logic.h"

/* After flash_store_init(): load the stored set, else build one from the bank. */
bool puzzle_gen_init(void);

/* Ask for a run now (serial "puzzles"). It starts as soon as the Wi-Fi, the clock and the game
 * state allow it. */
void puzzle_gen_request(void);

bool puzzle_gen_busy(void);

/* One scheduling pass: starts a run at boot (no fresh set), every evening and on request, and
 * advances a running one by one step. net_ok = Wi-Fi up and clock valid; gen_ok = no bomb
 * session and the alarm is not imminent. unix_now is 0 when the clock is not valid. */
void puzzle_gen_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok);

/* Thread-safe copy of the active set and its generation (+1 whenever the set is replaced). */
bool puzzle_set_snapshot(PuzzleSet *out, uint32_t *gen);
uint32_t puzzle_set_generation(void);

/* Index of the pair the next bomb should use (first one not yet used). */
int puzzle_next_pair(void);

/* Link task: the next unused pair and the set generation it came from, read in ONE short lock of
 * the puzzle mutex (a generation run swaps a whole new set in under that same lock, so the two
 * puzzles always belong together). Copies just the two puzzles. False if the set is busy. */
bool puzzle_take_next_pair(Puzzle *dial, Puzzle *wire, int *pair, uint32_t *gen);

/* A bomb started with this pair: mark it used (ignored if the set was replaced meanwhile).
 * When all pairs are used the list starts over. RAM only, safe for the link task: the flash
 * write is deferred to puzzle_flush_used() in the network task (dirty flag). False only when the
 * set lock could not be taken in time (nothing was marked: call again). */
bool puzzle_consume_pair(int pair, uint32_t gen);

/* Network task, every loop: write the used mask to flash if the link task changed it. */
void puzzle_flush_used(void);

/* Finished generation runs since boot (any outcome); the briefing starts after each one. */
uint32_t puzzle_gen_runs_done(void);

/* Serial "ai": the active set with sources, models, verification and the answers (debug only). */
void puzzle_print_set(void);

#endif /* PUZZLE_GEN_H */
