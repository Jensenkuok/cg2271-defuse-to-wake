/*
 * Defuse to Wake - push buttons on the shared PORTC_PORTD vector.
 *   - ENC_SW (PTC1): both edges; on release posts ENC_PRESS (< APP_LONG_PRESS_MS)
 *     or ENC_LONG_PRESS (snooze).
 *   - JOY_SW (PTC2): both edges; posts JOY_PRESS on press.
 *   - SHOCK (PTD6), only if APP_USE_SHOCK_FALLBACK: posts TAMPER on a hit.
 * Debounce: an edge within APP_BTN_DEBOUNCE_MS of the last accepted edge on the
 * same pin is ignored (time from xTaskGetTickCountFromISR, no timers, no loops).
 * Rubric: Poll+IRQ, ISR design.
 */
#ifndef BUTTONS_H
#define BUTTONS_H

void buttons_init(void);

#endif /* BUTTONS_H */
