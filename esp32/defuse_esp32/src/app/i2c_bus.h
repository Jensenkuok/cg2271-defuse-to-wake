/*
 * Defuse to Wake - the I2C bus is shared by the OLED (display task) and the MPU-6050
 * (gyro task), which run at different priorities and can pre-empt each other in the
 * middle of a transaction. Every Wire use therefore happens with this mutex held.
 * FreeRTOS mutexes have priority inheritance, so the gyro task (prio 3) waiting for a
 * frame transfer lends its priority to the display task (prio 2) holding the bus.
 */
#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

/* Wire.begin on the board pins + creates the mutex. Call once from setup(). */
bool i2c_bus_init(void);

/* Bounded wait; false on timeout. Pair every successful lock with i2c_bus_unlock(). */
bool i2c_bus_lock(uint32_t timeout_ms);
void i2c_bus_unlock(void);

/* Does anything ACK at this address? (takes the lock itself) */
bool i2c_bus_probe(uint8_t addr);

/* Probe 0x08-0x77 (takes the lock, bounded wait). Fills found[] with up to max addresses that
 * ACK and returns how many; -1 if the bus lock timed out. */
int i2c_bus_scan(uint8_t *found, int max);

/* Print "[i2c] SDA=.. SCL=.. <hz> Hz found: ..." (the "i2c" command and the boot line). With
 * allow_fallback, an empty scan is retried at APP_I2C_FALLBACK_HZ and that speed is kept if
 * it finds devices. */
void i2c_bus_report(bool allow_fallback);

#endif /* I2C_BUS_H */
