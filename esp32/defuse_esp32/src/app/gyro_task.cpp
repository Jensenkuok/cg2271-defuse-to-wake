/* Defuse to Wake - MPU-6050 tamper detector. See gyro_task.h. */
#include "gyro_task.h"

#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "i2c_bus.h"
#include "link_task.h"
#include "../shared/protocol.h"

static TaskHandle_t s_task;
static uint8_t s_addr = APP_MPU_ADDR;

/* Wire result of one register transaction, kept for failure diagnostics.
 * endtx: Wire endTransmission code (0 ok, 2 address NACK, 3 data NACK, 4 other, 5 timeout),
 * -1 = the bus lock timed out before any transfer. got/want: requestFrom byte counts
 * (both 0 for a write). */
struct MpuRes {
    int endtx;
    int got;
    int want;
};

/* Failure reason of the last sensor_init attempt and the last one printed. Static, not stack:
 * the gyro task stack has little headroom. Only the gyro task touches these. */
static char s_reason[APP_GYRO_REASON_LEN];
static char s_last_reason[APP_GYRO_REASON_LEN];

/* Register access. The caller holds the I2C bus lock; any error returns false. */
static bool mpu_write_reg(uint8_t addr, uint8_t reg, uint8_t val, MpuRes *r)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    r->endtx = (int)Wire.endTransmission();
    r->got = 0;
    r->want = 0;
    return r->endtx == 0;
}

static bool mpu_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n, MpuRes *r)
{
    size_t i;

    r->got = 0;
    r->want = (int)n;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    r->endtx = (int)Wire.endTransmission(false);
    if (r->endtx != 0) {
        return false;
    }
    r->got = (int)Wire.requestFrom((int)addr, (int)n);
    if (r->got != (int)n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

/* Run one register write under the bus lock; false on lock timeout (r->endtx = -1) or I2C error. */
static bool locked_write(uint8_t reg, uint8_t val, MpuRes *r)
{
    bool ok = false;

    r->endtx = -1;
    r->got = 0;
    r->want = 0;
    if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        ok = mpu_write_reg(s_addr, reg, val, r);
        i2c_bus_unlock();
    }
    return ok;
}

static void set_reason(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_reason(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_reason, sizeof(s_reason), fmt, ap);
    va_end(ap);
}

/* Record why a register write failed: step name plus the Wire code. */
static void set_write_reason(const char *step, const MpuRes *r)
{
    if (r->endtx < 0) {
        set_reason("%s write: bus busy (lock timeout)", step);
    } else {
        set_reason("%s write endTx=%d", step, r->endtx);
    }
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

/* Own register-level init: the Adafruit library rejects clones whose WHO_AM_I is not 0x68.
 * On failure s_reason says which step failed and the Wire evidence. */
static bool sensor_init(void)
{
    uint8_t who = 0;
    bool ok = false;
    bool locked = false;
    bool genuine;
    MpuRes r = { -1, 0, 0 };

    s_addr = APP_MPU_ADDR;
    if (!i2c_bus_probe(s_addr)) {
        s_addr = APP_MPU_ADDR_ALT; /* AD0 high */
        if (!i2c_bus_probe(s_addr)) {
            set_reason("probe: no ACK at 0x%02X or 0x%02X", APP_MPU_ADDR, APP_MPU_ADDR_ALT);
            return false;
        }
    }

    if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        locked = true;
        ok = mpu_read_regs(s_addr, APP_MPU_REG_WHO_AM_I, &who, APP_MPU_REG_BYTES, &r);
        i2c_bus_unlock();
    }
    if (!locked) {
        set_reason("WHO_AM_I read: bus busy (lock timeout)");
        return false;
    }
    if (!ok) {
        set_reason("WHO_AM_I read requestFrom got %d/%d (endTx=%d)", r.got, r.want, r.endtx);
        return false;
    }
    if (who == APP_MPU_WHO_AM_I_BUS_LOW || who == APP_MPU_WHO_AM_I_BUS_HIGH) {
        set_reason("WHO_AM_I=0x%02X (bus %s)", who, who == APP_MPU_WHO_AM_I_BUS_LOW ? "stuck low" : "floating");
        return false;
    }
    genuine = (who == APP_MPU_WHO_AM_I_MPU6050);

    if (!locked_write(APP_MPU_REG_PWR_MGMT_1, APP_MPU_PWR_RESET, &r)) {
        set_write_reason("reset", &r);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(APP_GYRO_RESET_WAIT_MS)); /* lock NOT held: the OLED may use the bus */
    if (!locked_write(APP_MPU_REG_PWR_MGMT_1, APP_MPU_PWR_WAKE_PLL, &r)) {
        set_write_reason("wake", &r);
        return false;
    }
    if (!locked_write(APP_MPU_REG_CONFIG, APP_MPU_CONFIG_DLPF_260HZ, &r)) {
        set_write_reason("CONFIG", &r);
        return false;
    }
    if (!locked_write(APP_MPU_REG_ACCEL_CONFIG, APP_MPU_ACCEL_RANGE_16G, &r)) {
        set_write_reason("ACCEL_CONFIG", &r);
        return false;
    }

    if (!genuine) {
        Serial.printf("[gyro] WARNING: WHO_AM_I=0x%02X is not 0x68, continuing as a compatible clone\n", who);
    }
    Serial.printf("[gyro] chip at 0x%02X WHO_AM_I=0x%02X (%s) ready\n", s_addr, who,
                  genuine ? "MPU-6050" : "compatible clone");
    return true;
}

/* Print one register read result: value or "--", then the Wire codes. */
static void print_reg(uint8_t addr, const char *name, bool ok, uint8_t val, const MpuRes *r)
{
    if (ok) {
        Serial.printf("[mpu] 0x%02X %s=0x%02X [endTx=%d got %d/%d]\n", addr, name, val, r->endtx, r->got, r->want);
    } else {
        Serial.printf("[mpu] 0x%02X %s=-- [endTx=%d got %d/%d]\n", addr, name, r->endtx, r->got, r->want);
    }
}

/* Read-only dump of one address for the "mpu" command (no register writes). */
static void dump_one(uint8_t addr)
{
    uint8_t who = 0, pwr = 0, acfg = 0;
    uint8_t raw[APP_MPU_ACCEL_BYTES];
    MpuRes rw, rp, ra, rs;
    bool ok_w, ok_p, ok_a, ok_s;

    if (!i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        Serial.printf("[mpu] 0x%02X: bus busy (lock timeout)\n", addr);
        return;
    }
    ok_w = mpu_read_regs(addr, APP_MPU_REG_WHO_AM_I, &who, APP_MPU_REG_BYTES, &rw);
    ok_p = mpu_read_regs(addr, APP_MPU_REG_PWR_MGMT_1, &pwr, APP_MPU_REG_BYTES, &rp);
    ok_a = mpu_read_regs(addr, APP_MPU_REG_ACCEL_CONFIG, &acfg, APP_MPU_REG_BYTES, &ra);
    ok_s = mpu_read_regs(addr, APP_MPU_REG_ACCEL_XOUT_H, raw, sizeof(raw), &rs);
    i2c_bus_unlock();

    print_reg(addr, "WHO_AM_I(0x75)", ok_w, who, &rw);
    print_reg(addr, "PWR_MGMT_1(0x6B)", ok_p, pwr, &rp);
    print_reg(addr, "ACCEL_CONFIG(0x1C)", ok_a, acfg, &ra);
    if (ok_s) {
        Serial.printf("[mpu] 0x%02X accel(0x3B)=%d %d %d [endTx=%d got %d/%d]\n", addr,
                      (int)(int16_t)(((uint16_t)raw[0] << 8) | raw[1]),
                      (int)(int16_t)(((uint16_t)raw[2] << 8) | raw[3]),
                      (int)(int16_t)(((uint16_t)raw[4] << 8) | raw[5]), rs.endtx, rs.got, rs.want);
    } else {
        Serial.printf("[mpu] 0x%02X accel(0x3B)=-- [endTx=%d got %d/%d]\n", addr, rs.endtx, rs.got, rs.want);
    }
}

void gyro_task_dump_regs(void)
{
    dump_one(APP_MPU_ADDR);
    dump_one(APP_MPU_ADDR_ALT);
}

static void gyro_task(void *arg)
{
    TickType_t wake;
    uint8_t raw[APP_MPU_ACCEL_BYTES];
    MpuRes rr;
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
        /* Always print the first failure and any changed reason; repeats at most once a minute. */
        if (!fail_logged || strcmp(s_reason, s_last_reason) != 0 ||
            (uint32_t)(now - last_fail_log) >= APP_GYRO_FAIL_LOG_MS) {
            fail_logged = true;
            last_fail_log = now;
            strlcpy(s_last_reason, s_reason, sizeof(s_last_reason));
            Serial.printf("[gyro] MPU init failed at 0x%02X: %s; retrying every %u s (run \"mpu\" or \"i2c\")\n",
                          s_addr, s_reason, (unsigned)(APP_GYRO_RETRY_MS / 1000u));
        }
        vTaskDelay(pdMS_TO_TICKS(APP_GYRO_RETRY_MS));
    }
    wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_GYRO_PERIOD_MS));
        got = false;

        /* The OLED frame transfer can hold the bus; a bounded wait just skips a sample. */
        if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
            got = mpu_read_regs(s_addr, APP_MPU_REG_ACCEL_XOUT_H, raw, sizeof(raw), &rr);
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
