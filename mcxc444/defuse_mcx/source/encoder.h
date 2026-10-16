/*
 * Defuse to Wake - rotary encoder (PTA12 = A, PTA5 = B) and on-board SW3 (PTA4).
 * Both live on Port A, so one PORTA_IRQHandler serves them.
 *   - Encoder: interrupt on both edges of A and B; a 4-state transition table
 *     decodes quadrature and cancels contact bounce. Posts ENC_STEP +1/-1 every
 *     APP_ENC_TRANSITIONS_PER_STEP valid transitions (one detent).
 *   - SW3: falling edge posts CMD_RING_NOW (value APP_SW3_RING_DELAY_S), a bench
 *     trigger that rings the alarm without the ESP32.
 * Rubric: Poll+IRQ (interrupt-driven input), ISR design.
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

void encoder_init(void);

/* Signed running detent count since boot (for telemetry). */
int32_t encoder_count(void);

#endif /* ENCODER_H */
