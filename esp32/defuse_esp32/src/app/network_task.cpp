/* Defuse to Wake - ESP32 network task. See network_task.h. */
#include "network_task.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <time.h>

#include "../../secrets.h"
#include "app_state.h"
#include "config.h"
#include "link_task.h"
#include "status.h"
#include "../shared/protocol.h"

static TaskHandle_t s_task;
static WiFiMulti s_multi;

/* Queue TIME_SYNC, then SET_ALARM if one is stored. Both or neither: the MCX needs them
 * in this order (a TIME_SYNC moves its clock, and an armed deadline is not moved with it). */
static bool send_time_and_alarm(uint32_t unix_now, uint32_t alarm_unix)
{
    proto_time_sync_t ts;
    proto_set_alarm_t sa;
    uint8_t pl[PROTO_SIZE_TIME_SYNC];
    uint8_t pl2[PROTO_SIZE_SET_ALARM];

    if (link_tx_space() < 2u) {
        return false; /* retry on the next loop */
    }
    ts.unix_time = unix_now;
    if (proto_pack_time_sync(&ts, pl, sizeof(pl)) == 0 ||
        !link_send(PROTO_MSG_TIME_SYNC, pl, PROTO_SIZE_TIME_SYNC)) {
        return false;
    }
    Serial.printf("[net] TIME_SYNC %u queued\n", (unsigned)unix_now);
    if (alarm_unix != 0u) {
        sa.alarm_unix = alarm_unix;
        if (proto_pack_set_alarm(&sa, pl2, sizeof(pl2)) != 0 &&
            link_send(PROTO_MSG_SET_ALARM, pl2, PROTO_SIZE_SET_ALARM)) {
            Serial.printf("[net] SET_ALARM %u queued\n", (unsigned)alarm_unix);
        }
    }
    return true;
}

static void network_task(void *arg)
{
    uint32_t next_wifi = 0;
    uint32_t last_sync = 0;
    uint32_t seen_link_count = 0;
    bool ntp_started = false;
    bool wifi_up = false;
    bool time_valid = false;
    bool prev_wifi = false;
    bool prev_time = false;
    AppState st;
    uint32_t now;
    uint32_t unix_now;

    (void)arg;
    WiFi.mode(WIFI_STA);
    s_multi.addAP(WIFI_SSID_1, WIFI_PASS_1);
    s_multi.addAP(WIFI_SSID_2, WIFI_PASS_2);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_NET_LOOP_MS));
        now = millis();

        /* WiFiMulti.run() blocks while it scans/connects, so it runs on a slow cadence. */
        if ((int32_t)(now - next_wifi) >= 0) {
            wifi_up = (s_multi.run(APP_WIFI_CONNECT_TIMEOUT_MS) == WL_CONNECTED);
            next_wifi = millis() + (wifi_up ? APP_WIFI_POLL_CONNECTED_MS : APP_WIFI_POLL_DOWN_MS);
        }
        if (wifi_up && !ntp_started) {
            configTime(APP_TZ_OFFSET_S, 0, APP_NTP_SERVER_1, APP_NTP_SERVER_2); /* SNTP keeps itself synced */
            ntp_started = true;
        }
        unix_now = (uint32_t)time(nullptr);
        time_valid = ntp_started && (unix_now >= APP_MIN_VALID_UNIX);

        if (wifi_up != prev_wifi) {
            prev_wifi = wifi_up;
            app_state_set_wifi(wifi_up);
            Serial.printf("[net] Wi-Fi %s\n", wifi_up ? "connected" : "disconnected");
        }
        if (time_valid != prev_time) {
            prev_time = time_valid;
            app_state_set_time_valid(time_valid);
            Serial.printf("[net] time %s\n", time_valid ? "valid" : "invalid");
        }

        if (time_valid && app_state_read(&st) && st.link_up) {
            bool new_link = (st.link_up_count != seen_link_count);
            bool periodic = (last_sync != 0u) && ((uint32_t)(now - last_sync) >= APP_TIME_SYNC_PERIOD_MS);
            if ((new_link || last_sync == 0u || periodic) && send_time_and_alarm(unix_now, st.alarm_unix)) {
                seen_link_count = st.link_up_count;
                last_sync = (now == 0u) ? 1u : now; /* 0 means "never" */
            }
        }

        status_tick(now);
    }
}

bool network_task_start(void)
{
    return xTaskCreate(network_task, "network", APP_STACK_NETWORK, NULL, APP_PRIO_NETWORK, &s_task) == pdPASS;
}

TaskHandle_t network_task_handle(void)
{
    return s_task;
}
