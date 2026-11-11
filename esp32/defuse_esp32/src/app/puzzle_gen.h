/*
 * Defuse to Wake - puzzle generation: bank refill and slot-by-slot AI upgrade (network task only, lowest priority).
 *
 * A SET is 4 dial + 4 wire puzzles; pair i = dial[i] + wire[i] is used for one bomb. Per puzzle:
 *   1. the ESP32 picks the answer (esp_random): a 4-digit code, or a wire 1..N (N from the level's
 *      wire-count range), and the LEVEL (EASY / NORMAL / HARD = the effective difficulty level);
 *   2. the ESP32 writes the FACTS for that answer and level (bank_generate: exactly one solution = the answer,
 *      the level is measured), and the SETTER model only rewrites them as a riddle (JSON {"puzzle":".."});
 *   3. the gate (puzzle_gate.cpp, gate_check_phrase) accepts the riddle only if the text passes validate_puzzle
 *      (length, ASCII, no leaked code / wire number) and fits the OLED rows;
 *   4. a CHECKER model from a different model ID gets only the riddle text and must reach the same
 *      answer (the setter is never its own checker);
 *   5. an exact match keeps the puzzle (source AI, verified); up to 3 attempts, then the procedural
 *      bank (puzzle_bank.cpp: random rules at the same level, proven by the same solver).
 * The LLM never decides an answer. The set is stored in flash and always full: a slot that is not
 * (yet) LLM-written holds a bank puzzle. While online the network task keeps upgrading ONE bank slot
 * at a time to an LLM puzzle (a failed attempt waits 60 s); a used pair is spent for good and its two
 * slots are refilled from the bank at once, then upgraded again when the Wi-Fi allows.
 *
 * The upgrade is a small state machine: puzzle_gen_service() does at most ONE LLM call (<= 20 s)
 * per pass, so the network task keeps serving the link, the alarm sync and the BOMB_CONFIG
 * triggers between calls. The LLM part never runs during a bomb session or close to the alarm; the
 * bank refill (offline, no LLM) does.
 */
#ifndef PUZZLE_GEN_H
#define PUZZLE_GEN_H

#include <stdbool.h>
#include <stdint.h>

#include "puzzle_logic.h"

/* After flash_store_init(): load the stored set, else build one from the bank at `level` (PrLevel,
 * the effective difficulty level). */
bool puzzle_gen_init(int level);

/* Network task, every pass: the effective difficulty level (PR_EASY..PR_HARD) and whether a bomb session
 * is running. Every later puzzle asks for that level. When the cached set was built for another level
 * (O10: the user saved a new mode, or Auto crossed a band after a morning) every unspent slot is replaced
 * by a bank puzzle at the new level (proven, offline, one slot per pass) and the upgrades to AI go on
 * from there. An upgrade running for the old level is cancelled. Inside a session (first BOMB_ACTIVE
 * until DEFUSED -> IDLE) that refill waits: the check repeats every pass, so it happens when the session
 * ends. Returns true on the pass that starts a refill. */
bool puzzle_gen_apply_level(int level, bool session_active);

/* Ask for a whole-set redo now (serial "puzzles", web button): every slot is marked as needing an upgrade, AI
 * or not. They are redone one at a time as soon as the Wi-Fi, the clock and the game state allow it. There
 * is no boot or nightly run any more: while online the upgrade of bank puzzles never stops by itself. */
void puzzle_gen_request(void);

/* True while an LLM call chain for one slot is under way (not between two slots, so a briefing gets a turn). */
bool puzzle_gen_busy(void);

/* One scheduling pass. First the offline bank work (refill of spent pairs, then of a level change; one slot per
 * pass, also during a bomb session); then, if nothing of that was left, the LLM upgrade of ONE slot (bank
 * puzzle or wrong level first, round-robin, spent pairs skipped), advanced by one step. net_ok = Wi-Fi up and
 * clock valid; gen_ok = no bomb session and the alarm is not imminent. unix_now is 0 when the clock is not valid. */
void puzzle_gen_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok);

/* Thread-safe copy of the active set and its generation. The generation is constant now: a single slot
 * changes in place without bumping it (R1), so it only tells a snapshot's origin, not its freshness. */
bool puzzle_set_snapshot(PuzzleSet *out, uint32_t *gen);
uint32_t puzzle_set_generation(void);

/* Index of the pair the next bomb should use (lowest unspent; the least recently spent if all are spent). */
int puzzle_next_pair(void);

/* Link task: the next pair and the set generation, read in ONE short lock of the puzzle mutex so the two
 * puzzles always belong together. Copies just the two puzzles. The pair becomes the RESERVED pair: if the
 * network task later replaces one of its puzzles, puzzle_reserved_changed_take() tells bomb_config to
 * re-send it. If every pair is spent the least recently spent one is returned (D6: the bomb never waits)
 * and "[ai] all pairs spent: reusing pair N" is logged. False if the set is busy. */
bool puzzle_take_next_pair(Puzzle *dial, Puzzle *wire, int *pair, uint32_t *gen);

/* A bomb started with this pair: it is spent for good (D4). Marked by index, no generation check. RAM only,
 * safe for the link task: the flash write is deferred to puzzle_flush_used() in the network task (dirty
 * flag), and so is the bank refill of the pair's two slots. False only when the set lock could not be
 * taken in time (nothing was marked: call again). */
bool puzzle_consume_pair(int pair);

/* Link task: true once after the network task replaced a puzzle of the reserved pair (the one last taken
 * and not consumed yet). bomb_config then re-sends BOMB_CONFIG (ARMED outside a session only), so the MCX's
 * answers and the OLED text of the next bomb come from the same puzzle. Never changes the set. */
bool puzzle_reserved_changed_take(void);

/* Network task, every loop: write the spent mask to flash if the link task changed it. */
void puzzle_flush_used(void);

/* Finished whole-set events since boot: a manual redo of every slot, or a level refill. NOT counted per
 * slot, so the briefing does not fire for every upgraded puzzle. */
uint32_t puzzle_gen_runs_done(void);

/* What the web page's AI panel shows about the generator itself. Plain words written by the network
 * task and read by the web task without a lock: each field is a single machine word, and a reader
 * that catches two fields from different moments only shows a slightly stale picture. */
struct PuzzleRunInfo {
    bool busy;              /* an LLM upgrade of one slot is in progress */
    bool queued;            /* a manual redo of every slot is pending or running */
    int8_t slot;            /* slot being upgraded (0..3 dial, 4..7 wire), -1 = none */
    uint8_t spent;          /* bit per pair: used up, bank refill pending */
    uint16_t retry_s;       /* seconds until the next attempt after a failure, 0 = none waiting */
};
void puzzle_gen_info(PuzzleRunInfo *out);

/* Serial "ai": the active set with sources, models, verification and the answers (debug only). */
void puzzle_print_set(void);

#endif /* PUZZLE_GEN_H */
