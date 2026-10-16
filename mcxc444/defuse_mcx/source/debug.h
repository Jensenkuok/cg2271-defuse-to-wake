/*
 * Defuse to Wake - debug / stats task (priority 1, lowest app task).
 *   APP_DRIVER_TEST 1: consumes the input event queue and prints every event;
 *     every 5 s prints raw joystick, distance, encoder count, drops and the stack
 *     high-water mark of every task; at boot plays every sound and cycles every
 *     LED mode (2 s each).
 *   APP_DRIVER_TEST 0: prints link + stack stats every 10 s (APP_DEBUG_STATS 1);
 *     with APP_DEBUG_STATS 0 the task is not created at all.
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

/* Creates the task if this build needs one; true if nothing failed. */
bool debug_start(void);

#endif /* DEBUG_H */
