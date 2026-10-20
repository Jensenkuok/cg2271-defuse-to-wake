/*
 * Defuse to Wake - ESP32-S2 firmware (renderer + network gateway).
 * The MCXC444 runs the game; this board shows it on the OLED, detects tamper with the
 * MPU-6050, keeps time over Wi-Fi/NTP and talks to the MCX over Serial1 (proto_link).
 *
 * Single-core ESP32-S2, Arduino-on-FreeRTOS. Tasks (priority, why): see src/app/config.h
 *   link 4 > gyro 3 > display 2 > network 1 = web 1.
 * setup() only creates the shared objects and the tasks; loop() deletes itself.
 */
#include "src/app/app_state.h"
#include "src/app/config.h"
#include "src/app/display_task.h"
#include "src/app/fault.h"
#include "src/app/gyro_task.h"
#include "src/app/i2c_bus.h"
#include "src/app/link_task.h"
#include "src/app/network_task.h"
#include "src/app/web_task.h"

/* Library headers are also listed here so the Arduino build always finds the libraries
 * used by the files under src/app/. */
#include <Adafruit_GFX.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <Wire.h>

void setup()
{
    bool ok;

    Serial.setRxBufferSize(APP_SERIAL_RX_BUF); /* room for a pasted "link <url>" test command */
    Serial.begin(APP_DEBUG_BAUD);
    Serial.println("\n[boot] Defuse to Wake ESP32-S2");

    fault_init();
    ok = app_state_init() && i2c_bus_init() && link_task_init();
    if (!ok) {
        Serial.println("[boot] FATAL: could not create shared objects");
        return; /* loop() deletes the loop task; nothing else runs */
    }
    ok = link_task_start() && gyro_task_start() && display_task_start() && network_task_start() &&
         web_task_start();
    if (!ok) {
        Serial.println("[boot] FATAL: could not create a task");
    }
}

void loop()
{
    vTaskDelete(NULL); /* everything runs in the tasks created in setup() */
}
