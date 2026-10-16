/* Defuse to Wake - MPU-6050 tamper detector. See gyro_task.h. */
#include "gyro_task.h"

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

#include "app_state.h"
#include "config.h"
#include "i2c_bus.h"
#include "link_task.h"
#include "../shared/protocol.h"

static TaskHandle_t s_task;
static Adafruit_MPU6050 s_mpu;

/* Acceleration magnitude minus 1 g, absolute, in mg. */
static uint32_t deviation_mg(const sensors_event_t &a)
{
    float mag = sqrtf(a.acceleration.x * a.acceleration.x + a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);
    float dev = fabsf(mag - APP_STANDARD_GRAVITY) * (APP_MG_PER_G / APP_STANDARD_GRAVITY);
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

static bool sensor_init(void)
{
    bool ok = false;

    if (!i2c_bus_probe(APP_MPU_ADDR)) {
        return false;
    }
    if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        ok = s_mpu.begin(APP_MPU_ADDR, &Wire);
        if (ok) {
            s_mpu.setAccelerometerRange(MPU6050_RANGE_16_G); /* shakes exceed 8 g */
            s_mpu.setFilterBandwidth(MPU6050_BAND_260_HZ);   /* keep spikes, no smoothing */
        }
        i2c_bus_unlock();
    }
    return ok;
}

static void gyro_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    sensors_event_t a, g, t;
    AppState st;
    uint32_t dev;
    uint32_t peak = 0;
    uint32_t last_log = 0;
    uint32_t last_send = 0;
    bool sent_once = false;
    uint32_t now;

    (void)arg;
    if (!sensor_init()) {
        Serial.println("[gyro] MPU-6050 not found: tamper detection disabled (task exits)");
        s_task = NULL; /* so the status print does not query a deleted task */
        vTaskDelete(NULL);
        return;
    }
    Serial.println("[gyro] MPU-6050 ready");

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_GYRO_PERIOD_MS));

        /* The OLED frame transfer can hold the bus; a bounded wait just skips a sample. */
        if (!i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
            continue;
        }
        s_mpu.getEvent(&a, &g, &t);
        i2c_bus_unlock();

        dev = deviation_mg(a);
        if (dev > peak) {
            peak = dev;
        }
        now = millis();

        if (app_state_read(&st) && st.game_valid && st.game.state == PROTO_STATE_BOMB_ACTIVE &&
            dev >= APP_TAMPER_SEND_MG && (!sent_once || (uint32_t)(now - last_send) >= APP_TAMPER_COOLDOWN_MS)) {
            send_tamper(dev);
            sent_once = true;
            last_send = now;
            Serial.printf("[gyro] TAMPER sent %u mg\n", (unsigned)dev);
        }

        if ((uint32_t)(now - last_log) >= APP_GYRO_LOG_MS) {
            last_log = now;
            Serial.printf("[gyro] peak deviation %u mg in last %u s (send >= %u, MCX strikes >= %u)\n",
                          (unsigned)peak, (unsigned)(APP_GYRO_LOG_MS / 1000u), (unsigned)APP_TAMPER_SEND_MG,
                          (unsigned)GAME_TAMPER_THRESHOLD_MG);
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
