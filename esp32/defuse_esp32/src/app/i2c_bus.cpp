/* Defuse to Wake - shared I2C bus. See i2c_bus.h. */
#include "i2c_bus.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

static SemaphoreHandle_t s_mutex;
static uint32_t s_hz = APP_I2C_HZ;

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

/* ===== Q&A [Synchronization tools 2 (ESP32)] =====
 * Q: Mutex on the ESP32? A: this one guards the shared I2C bus (OLED, and the gyro when
 *    enabled), with a bounded wait. Others guard the app state, flash, timetable, history.
 * ===== end Q&A ===== */
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

int i2c_bus_scan(uint8_t *found, int max)
{
    int n = 0;
    int addr;

    if (!i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        return -1;
    }
    for (addr = APP_I2C_SCAN_FIRST; addr <= APP_I2C_SCAN_LAST && n < max; addr++) {
        Wire.beginTransmission((uint8_t)addr);
        if (Wire.endTransmission() == 0) {
            found[n++] = (uint8_t)addr;
        }
    }
    i2c_bus_unlock();
    return n;
}

static void print_scan(const uint8_t *found, int n, const char *note)
{
    int i;

    Serial.printf("[i2c] SDA=%d SCL=%d %u Hz found:", APP_I2C_SDA_PIN, APP_I2C_SCL_PIN, (unsigned)s_hz);
    if (n < 0) {
        Serial.print(" bus busy");
    } else if (n == 0) {
        Serial.print(" none");
    }
    for (i = 0; i < n; i++) {
        Serial.printf(" 0x%02X", found[i]);
    }
    Serial.println(note);
}

static void set_clock(uint32_t hz)
{
    if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        Wire.setClock(hz);
        s_hz = hz;
        i2c_bus_unlock();
    }
}

void i2c_bus_report(bool allow_fallback)
{
    uint8_t found[APP_I2C_SCAN_MAX];
    int n = i2c_bus_scan(found, APP_I2C_SCAN_MAX);

    if (n == 0 && allow_fallback && s_hz != APP_I2C_FALLBACK_HZ) {
        const uint32_t normal_hz = s_hz;

        print_scan(found, n, "");
        set_clock(APP_I2C_FALLBACK_HZ);
        n = i2c_bus_scan(found, APP_I2C_SCAN_MAX);
        if (n > 0) {
            print_scan(found, n, " (kept 100 kHz: nothing answered at the normal speed)");
        } else {
            set_clock(normal_hz); /* nothing there either: restore the configured speed */
            print_scan(found, n, "");
        }
        return;
    }
    print_scan(found, n, "");
}
