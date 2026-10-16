/* Defuse to Wake - shared I2C bus. See i2c_bus.h. */
#include "i2c_bus.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

static SemaphoreHandle_t s_mutex;

bool i2c_bus_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return false;
    }
    Wire.begin(APP_I2C_SDA_PIN, APP_I2C_SCL_PIN);
    Wire.setClock(APP_I2C_HZ);
    return true;
}

bool i2c_bus_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void i2c_bus_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

bool i2c_bus_probe(uint8_t addr)
{
    uint8_t err;

    if (!i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        return false;
    }
    Wire.beginTransmission(addr);
    err = Wire.endTransmission();
    i2c_bus_unlock();
    return err == 0;
}
