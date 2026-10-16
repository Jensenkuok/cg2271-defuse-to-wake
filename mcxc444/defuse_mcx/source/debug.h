/*
 * Defuse to Wake - debug / stats task (priority 1, lowest app task).
 *   APP_DRIVER_TEST 1: consumes the input event queue and prints every event;
 *     every 5 s prints raw joystick, distance, encoder count, drops and the stack
 *     high-water mark of every task; at boot plays every sound and cycles every
 *     LED mode (2 s each).
 *   APP_DRIVER_TEST 0: the bench stand-in for the OLED. With APP_GAME_DEBUG it prints
 *     one line each time the game snapshot changes (state, module, timer, strikes,
 *     decoded module value). With APP_DEBUG_STATS it prints link, drop-counter, control
 *     and stack statistics every 10 s. With both 0 the task is not created.
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

/* Creates the task if this build needs one; true if nothing failed. */
bool debug_start(void);

/* Game task -> debug task: "the snapshot changed" (task notification, never blocks).
 * No-op when the debug task does not exist. */
void debug_notify_state_changed(void);

#endif /* DEBUG_H */
