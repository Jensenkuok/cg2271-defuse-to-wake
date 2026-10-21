/* Defuse to Wake - MPU-6050 tamper detector. See gyro_task.h. */
#include "gyro_task.h"

#include <Wire.h>
#include <math.h>

#include "app_state.h"
#include "config.h"
#include "i2c_bus.h"
#include "link_task.h"
#include "../shared/protocol.h"

static TaskHandle_t s_task;
static uint8_t s_addr = APP_MPU_ADDR;

/* Register access. The caller holds the I2C bus lock; any error returns false. */
static bool mpu_write_reg(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool mpu_read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    size_t i;

    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((int)s_addr, (int)n) != (int)n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

/* Run one register write under the bus lock; false on lock timeout or I2C error. */
static bool locked_write(uint8_t reg, uint8_t val)
{
    bool ok = false;

    if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        ok = mpu_write_reg(reg, val);
        i2c_bus_unlock();
    }
    return ok;
}

/* Acceleration magnitude minus 1 g, absolute, in mg, from the raw counts. */
static uint32_t deviation_mg(const uint8_t *raw)
{
    int16_t x = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    int16_t y = (int16_t)(((uint16_t)raw[2] << 8) | raw[3]);
    int16_t z = (int16_t)(((uint16_t)raw[4] << 8) | raw[5]);
    float mag = sqrtf((float)x * x + (float)y * y + (float)z * z) * (APP_MG_PER_G / APP_MPU_LSB_PER_G);
    float dev = fabsf(mag - APP_MG_PER_G);

    return (dev > (float)APP_TAMPER_MAX_MG) ? APP_TAMPER_MAX_MG : (uint32_t)dev;
}

static void send_tamper(uint32_t mg)
{
    proto_tamper_event_t t;
    uint8_t pl[PROTO_SIZE_TAMPER_EVENT];

    t.magnitude_mg = (uint16_t)mg;
    if (proto_pack_tamper_event(&t, pl, sizeof(pl)) != 0) {
        (void)link_send(PROTO_MSG_TAMPER_EVENT, pl, PROTO_SIZE_TAMPER_EVENT);
    }
}

/* Own register-level init: the Adafruit library rejects clones whose WHO_AM_I is not 0x68. */
static bool sensor_init(void)
{
    uint8_t who = 0;
    bool ok = false;
    bool genuine;

    s_addr = APP_MPU_ADDR;
    if (!i2c_bus_probe(s_addr)) {
        s_addr = APP_MPU_ADDR_ALT; /* AD0 high */
        if (!i2c_bus_probe(s_addr)) {
            return false;
        }
    }

    if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        ok = mpu_read_regs(APP_MPU_REG_WHO_AM_I, &who, 1);
        i2c_bus_unlock();
    }
    if (!ok || who == APP_MPU_WHO_AM_I_BUS_LOW || who == APP_MPU_WHO_AM_I_BUS_HIGH) {
        return false;
    }
    genuine = (who == APP_MPU_WHO_AM_I_MPU6050);

    if (!locked_write(APP_MPU_REG_PWR_MGMT_1, APP_MPU_PWR_RESET)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(APP_GYRO_RESET_WAIT_MS)); /* lock NOT held: the OLED may use the bus */
    if (!locked_write(APP_MPU_REG_PWR_MGMT_1, APP_MPU_PWR_WAKE_PLL) ||
        !locked_write(APP_MPU_REG_CONFIG, APP_MPU_CONFIG_DLPF_260HZ) ||
        !locked_write(APP_MPU_REG_ACCEL_CONFIG, APP_MPU_ACCEL_RANGE_16G)) {
        return false;
    }

    if (!genuine) {
        Serial.printf("[gyro] WARNING: WHO_AM_I=0x%02X is not 0x68, continuing as a compatible clone\n", who);
    }
    Serial.printf("[gyro] chip at 0x%02X WHO_AM_I=0x%02X (%s) ready\n", s_addr, who,
                  genuine ? "MPU-6050" : "compatible clone");
    return true;
}

static void gyro_task(void *arg)
{
    TickType_t wake;
    uint8_t raw[APP_MPU_ACCEL_BYTES];
    AppState st;
    uint32_t dev;
    uint32_t peak = 0;
    uint32_t last_log = 0;
    uint32_t last_send = 0;
    uint32_t last_fail_log = 0;
    uint32_t read_err = 0;
    bool sent_once = false;
    bool fail_logged = false;
    bool got;
    uint32_t now;

    (void)arg;
    /* A loose wire or a late-powered GY-521 must recover without a reset: keep retrying. */
    while (!sensor_init()) {
        now = millis();
        if (!fail_logged || (uint32_t)(now - last_fail_log) >= APP_GYRO_FAIL_LOG_MS) {
            fail_logged = true;
            last_fail_log = now;
            Serial.printf("[gyro] MPU init failed at 0x%02X/0x%02X (run \"i2c\" to scan); retrying every %u s\n",
                          APP_MPU_ADDR, APP_MPU_ADDR_ALT, (unsigned)(APP_GYRO_RETRY_MS / 1000u));
        }
        vTaskDelay(pdMS_TO_TICKS(APP_GYRO_RETRY_MS));
    }
    wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_GYRO_PERIOD_MS));
        got = false;

        /* The OLED frame transfer can hold the bus; a bounded wait just skips a sample. */
        if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
            got = mpu_read_regs(APP_MPU_REG_ACCEL_XOUT_H, raw, sizeof(raw));
            i2c_bus_unlock();
            if (!got) {
                read_err++;
            }
        }
        now = millis();

        if (got) {
            dev = deviation_mg(raw);
            if (dev > peak) {
                peak = dev;
            }
            if (app_state_read(&st) && st.game_valid && st.game.state == PROTO_STATE_BOMB_ACTIVE &&
                dev >= APP_TAMPER_SEND_MG &&
                (!sent_once || (uint32_t)(now - last_send) >= APP_TAMPER_COOLDOWN_MS)) {
                send_tamper(dev);
                sent_once = true;
                last_send = now;
                Serial.printf("[gyro] TAMPER sent %u mg\n", (unsigned)dev);
            }
        }

        if ((uint32_t)(now - last_log) >= APP_GYRO_LOG_MS) {
            last_log = now;
            Serial.printf("[gyro] peak deviation %u mg in last %u s (send >= %u, MCX strikes >= %u) read_err=%u\n",
                          (unsigned)peak, (unsigned)(APP_GYRO_LOG_MS / 1000u), (unsigned)APP_TAMPER_SEND_MG,
                          (unsigned)GAME_TAMPER_THRESHOLD_MG, (unsigned)read_err);
            peak = 0;
        }
    }
}

bool gyro_task_start(void)
{
    return xTaskCreate(gyro_task, "gyro", APP_STACK_GYRO, NULL, APP_PRIO_GYRO, &s_task) == pdPASS;
}

TaskHandle_t gyro_task_handle(void)
{
    return s_task;
}
