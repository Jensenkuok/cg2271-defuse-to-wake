/* Defuse to Wake - morning history in flash. See history.h. */
#include "history.h"

#include <Arduino.h>

#include "config.h"
#include "flash_store.h"

namespace {

struct HistBlob {
    uint32_t magic;
    HistoryRing ring;
};

SemaphoreHandle_t s_mutex;
HistoryRing s_ring;                 /* guarded by s_mutex */
MorningTracker s_tracker;           /* network task only */
HistBlob s_blob;                    /* flash load/save buffer, network task / boot only */

void save_ring(void)
{
    memset(static_cast<void *>(&s_blob), 0, sizeof(s_blob));   /* no uninitialised padding in flash */
    s_blob.magic = APP_HIST_MAGIC;
    s_blob.ring = s_ring;
    if (!flash_write_blob(APP_KEY_HISTORY, &s_blob, sizeof(s_blob))) {
        Serial.println("[hist] could not write the history to flash");
    }
}

}  // namespace

bool history_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return false;
    }
    s_ring.clear();
    memset(&s_tracker, 0, sizeof(s_tracker));
    if (flash_read_blob(APP_KEY_HISTORY, &s_blob, sizeof(s_blob)) && s_blob.magic == APP_HIST_MAGIC &&
        s_blob.ring.valid()) {
        s_ring = s_blob.ring;
    }
    Serial.printf("[hist] %d mornings in flash\n", s_ring.size());
    return true;
}

bool history_on_result(bool defused, uint16_t duration_s, uint8_t strikes, uint8_t snoozes, uint32_t end_unix,
                       const char *module, HistRecord *rec_out)
{
    HistRecord rec;

    if (!morning_on_result(&s_tracker, defused, duration_s, strikes, snoozes, end_unix, module, &rec)) {
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) != pdTRUE) {
        return false;
    }
    (void)s_ring.push(rec);          /* the oldest of 14 is overwritten */
    xSemaphoreGive(s_mutex);
    save_ring();
    if (rec_out != NULL) {
        *rec_out = rec;
    }
    Serial.printf("[hist] DEFUSED recorded: duration=%us strikes=%u snoozes=%u explosions=%u module=%s\n",
                  (unsigned)rec.duration_s, rec.strikes, rec.snoozes, rec.explosions,
                  rec.module[0] != '\0' ? rec.module : "-");
    return true;
}

int history_count(void)
{
    return s_ring.size();
}

int history_recent(HistRecord *out, int n)
{
    int i;
    int copied = 0;

    if (out == NULL || n <= 0 || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) != pdTRUE) {
        return 0;
    }
    for (i = 0; i < s_ring.size() && i < n; i++) {
        out[i] = s_ring.newest(i);
        copied++;
    }
    xSemaphoreGive(s_mutex);
    return copied;
}

int history_current_streak(uint32_t now_unix)
{
    int streak = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) == pdTRUE) {
        streak = history_streak(s_ring, now_unix);
        xSemaphoreGive(s_mutex);
    }
    return streak;
}
