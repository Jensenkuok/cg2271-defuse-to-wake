/*
 * Defuse to Wake - fault injection switched from the web page's Engineering tab (demo of the link
 * protocol's recovery and of the offline fallbacks). Two independent faults, both safe by design:
 *
 *   corrupt next packet   fault_corrupt_arm() arms a one-shot flag. The LINK task's proto_link write
 *                         callback takes it with fault_corrupt_take() and flips the CRC byte of the
 *                         next outgoing frame (see link_task.cpp); the MCX NACKs, proto_link
 *                         retransmits. Nothing else changes, the bomb is unaffected.
 *   simulated outage      fault_outage_start() makes fault_net_blocked() return true for
 *                         APP_FAULT_OUTAGE_S. llm_client, the NUSMods fetch and the Firebase mirror
 *                         call it before every HTTPS request and fail fast. Wi-Fi stays connected
 *                         (the web page itself is served over it); NTP is not an HTTPS call and is
 *                         not blocked.
 *
 * Thread-safe: the web task sets, the link and network tasks read. A tiny critical section guards
 * the few words, so the priority-4 link task is never blocked behind a lower-priority task.
 */
#ifndef FAULT_H
#define FAULT_H

#include <stdbool.h>
#include <stdint.h>

void fault_init(void);

/* Web task: arm the one-shot "corrupt the next frame" flag. */
void fault_corrupt_arm(void);
bool fault_corrupt_armed(void);
/* Link task: true exactly once per arm; the caller then corrupts the frame it is about to write. */
bool fault_corrupt_take(void);

/* Web task: start (or restart) the simulated outage for APP_FAULT_OUTAGE_S. */
void fault_outage_start(void);
/* Any task: is the simulated outage active now? */
bool fault_outage_active(void);
/* Whole seconds left (0 = not active). */
uint32_t fault_outage_left_s(void);

/* Every HTTPS caller, before it opens a connection: true = fail fast with "simulated outage". */
bool fault_net_blocked(void);

#endif /* FAULT_H */
