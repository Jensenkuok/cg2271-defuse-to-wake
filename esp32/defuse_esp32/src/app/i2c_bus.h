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

#endif /* I2C_BUS_H */
