/* Defuse to Wake - morning history records. See history_ring.h. */
#include "history_ring.h"

#include <stdio.h>
#include <string.h>

namespace {

uint8_t add_saturating(uint8_t a, uint8_t b)
{
    return (a > (uint8_t)(UINT8_MAX - b)) ? (uint8_t)UINT8_MAX : (uint8_t)(a + b);
}

/* Reduce a module code to alphanumerics so it is safe inside a JSON string and a URL-free body. */
void copy_alnum(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;

    if (src != NULL) {
        for (; *src != '\0' && n < dst_size - 1u; src++) {
            if ((*src >= '0' && *src <= '9') || (*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z')) {
                dst[n++] = *src;
            }
        }
    }
    dst[n] = '\0';
}

int64_t local_day(uint32_t unix_s)
{
    return ((int64_t)unix_s + SCHED_TZ_OFFSET_S) / SCHED_SEC_PER_DAY;
}

bool has_day(const HistoryRing &ring, int64_t day)
{
    int i;

    for (i = 0; i < ring.size(); i++) {
        if (ring.oldest(i).end_unix != 0u && local_day(ring.oldest(i).end_unix) == day) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool morning_on_result(MorningTracker *t, bool defused, uint16_t duration_s, uint8_t strikes, uint8_t snoozes,
                       uint32_t end_unix, const char *module, HistRecord *rec)
{
    if (t == NULL) {
        return false;
    }
    t->strikes = add_saturating(t->strikes, strikes);
    if (!defused) {
        t->explosions = add_saturating(t->explosions, 1u);
        return false;
    }
    if (rec == NULL) {
        return false;
    }
    memset(rec, 0, sizeof(*rec));
    rec->end_unix = end_unix;
    rec->duration_s = duration_s;
    rec->strikes = t->strikes;
    rec->snoozes = snoozes;
    rec->explosions = t->explosions;
    copy_alnum(rec->module, sizeof(rec->module), module);
    t->explosions = 0;
    t->strikes = 0;
    return true;
}

int history_streak(const HistoryRing &ring, uint32_t now_unix)
{
    int64_t newest_day = -1;
    int64_t d;
    int streak = 0;
    int i;

    for (i = 0; i < ring.size(); i++) {
        if (ring.oldest(i).end_unix != 0u && local_day(ring.oldest(i).end_unix) > newest_day) {
            newest_day = local_day(ring.oldest(i).end_unix);
        }
    }
    if (newest_day < 0) {
        return 0;
    }
    if (now_unix != 0u && local_day(now_unix) - newest_day > 1) {
        return 0;       /* a whole day was missed since the last defuse */
    }
    for (d = newest_day; has_day(ring, d) && streak < HIST_LEN; d--) {
        streak++;
    }
    return streak;
}

size_t history_to_json(const HistRecord &r, char *buf, size_t size)
{
    int n;

    if (buf == NULL || size == 0u) {
        return 0;
    }
    n = snprintf(buf, size,
                 "{\"ts\":%lu,\"duration_s\":%u,\"strikes\":%u,\"snoozes\":%u,\"explosions\":%u,"
                 "\"module\":\"%s\",\"device\":\"%s\"}",
                 (unsigned long)r.end_unix, (unsigned)r.duration_s, (unsigned)r.strikes, (unsigned)r.snoozes,
                 (unsigned)r.explosions, r.module, HIST_DEVICE_ID);
    return (n > 0 && (size_t)n < size) ? (size_t)n : 0u;
}
