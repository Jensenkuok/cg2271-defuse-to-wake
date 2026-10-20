/* Defuse to Wake - periodic Serial status. See status.h. */
#include "status.h"

#include <Arduino.h>
#include <time.h>

#include "app_state.h"
#include "config.h"
#include "display_task.h"
#include "gyro_task.h"
#include "history.h"
#include "link_task.h"
#include "network_task.h"
#include "web_task.h"

static void print_stack(const char *name, TaskHandle_t h)
{
    if (h != NULL) {
        Serial.printf(" %s=%u", name, (unsigned)uxTaskGetStackHighWaterMark(h)); /* bytes still unused */
    }
}

static void status_print(uint32_t now_ms)
{
    static const char *const names[] = { "IDLE", "ARMED", "BOMB_ACTIVE", "DEFUSED", "EXPLODED" };
    AppState st;

    if (!app_state_read(&st)) {
        return;
    }
    Serial.printf("[status] up=%us link=%s wifi=%s ip=%s time=%s unix=%u game=%s v%u\n", (unsigned)(now_ms / 1000u),
                  st.link_up ? "UP" : "DOWN", st.wifi_up ? "up" : "down", st.ip[0] != '\0' ? st.ip : "-",
                  st.time_valid ? "valid" : "invalid",
                  st.time_valid ? (unsigned)time(nullptr) : 0u,
                  st.game_valid ? (st.game.state < 5 ? names[st.game.state] : "?") : "none",
                  (unsigned)st.game_version);
    Serial.printf("[link] tx=%u rx=%u retx=%u fail=%u crc=%u len=%u dup=%u nack=%u backpressure=%u lock_to=%u\n",
                  (unsigned)st.link_stats.tx_frames, (unsigned)st.link_stats.rx_frames,
                  (unsigned)st.link_stats.retransmits, (unsigned)st.link_stats.send_failures,
                  (unsigned)st.link_stats.crc_errors, (unsigned)st.link_stats.len_errors,
                  (unsigned)st.link_stats.dup_drops, (unsigned)st.link_stats.nacks_sent,
                  (unsigned)st.tx_backpressure, (unsigned)app_state_lock_timeouts());
    Serial.printf("[online] lessons=%u fetched=%u alarm=%u mirror_pending=%u hist=%d streak=%d\n",
                  (unsigned)st.tt_lessons, (unsigned)st.tt_fetched_unix, (unsigned)st.alarm_unix,
                  (unsigned)st.mirror_pending, history_count(),
                  history_current_streak(st.time_valid ? (uint32_t)time(nullptr) : 0u));
    Serial.print("[stack bytes free]");
    print_stack("link", link_task_handle());
    print_stack("gyro", gyro_task_handle());
    print_stack("display", display_task_handle());
    print_stack("net", network_task_handle());
    print_stack("web", web_task_handle());
    Serial.printf("  heap_free=%u min_heap=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

void status_tick(uint32_t now_ms)
{
    static uint32_t s_last;

    if ((uint32_t)(now_ms - s_last) >= APP_STATUS_PERIOD_MS) {
        s_last = now_ms;
        status_print(now_ms);
    }
}
