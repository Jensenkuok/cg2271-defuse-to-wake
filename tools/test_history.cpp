/* Host-side tests for esp32/defuse_esp32/src/app/{fixed_ring.h,history_ring.*}. Plain checks. */
#include <stdio.h>
#include <string.h>

#include "history_ring.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

static HistRecord rec_at(int y, int m, int d, int hh, int mm)
{
    HistRecord r;

    memset(&r, 0, sizeof(r));
    r.end_unix = sched_local_to_unix(y, m, d, hh, mm);
    r.duration_s = 60;
    return r;
}

static void test_ring_basics(void)
{
    FixedRing<int, 3> r;
    bool dropped;

    r.clear();
    CHECK(r.size() == 0 && r.valid());
    CHECK(!r.pop_oldest());
    CHECK(!r.push(1));
    CHECK(!r.push(2));
    CHECK(!r.push(3));
    CHECK(r.size() == 3);
    CHECK(r.oldest(0) == 1 && r.oldest(2) == 3);
    CHECK(r.newest(0) == 3 && r.newest(2) == 1);
    dropped = r.push(4);                       /* full: 1 is dropped */
    CHECK(dropped && r.size() == 3);
    CHECK(r.oldest(0) == 2 && r.newest(0) == 4);
    CHECK(r.pop_oldest() && r.size() == 2 && r.oldest(0) == 3);
    CHECK(!r.push(5) && r.newest(0) == 5 && r.oldest(0) == 3);
    r.clear();
    CHECK(r.size() == 0);
}

static void test_ring_wraps_many_times(void)
{
    FixedRing<int, 4> r;
    int i;

    r.clear();
    for (i = 1; i <= 50; i++) {
        (void)r.push(i);
    }
    CHECK(r.size() == 4 && r.oldest(0) == 47 && r.newest(0) == 50 && r.valid());
}

static void test_tracker(void)
{
    MorningTracker t = { 0, 0 };
    HistRecord rec;

    CHECK(!morning_on_result(&t, false, 180, 3, 0, 1000, "CG2271", &rec));   /* exploded */
    CHECK(!morning_on_result(&t, false, 180, 3, 1, 1100, "CG2271", &rec));
    CHECK(t.explosions == 2 && t.strikes == 6);
    CHECK(morning_on_result(&t, true, 95, 1, 1, 1200, "CG2271", &rec));
    CHECK(rec.explosions == 2 && rec.strikes == 7 && rec.snoozes == 1);
    CHECK(rec.duration_s == 95 && rec.end_unix == 1200 && strcmp(rec.module, "CG2271") == 0);
    CHECK(t.explosions == 0 && t.strikes == 0);                              /* reset for the next morning */

    CHECK(morning_on_result(&t, true, 30, 0, 0, 0, "", &rec));               /* manual ring, no clock */
    CHECK(rec.explosions == 0 && rec.module[0] == '\0' && rec.end_unix == 0);

    CHECK(morning_on_result(&t, true, 30, 0, 0, 5, "A\"B\\C-1 D", &rec));    /* sanitised */
    CHECK(strcmp(rec.module, "ABC1D") == 0);
    CHECK(morning_on_result(&t, true, 30, 0, 0, 5, "VERYLONGMODULECODE", &rec));
    CHECK(strlen(rec.module) == SCHED_MODULE_LEN - 1u);
    CHECK(morning_on_result(&t, true, 30, 0, 0, 5, NULL, &rec) && rec.module[0] == '\0');

    {   /* counters saturate at 255 instead of wrapping */
        MorningTracker big = { 255, 250 };
        CHECK(!morning_on_result(&big, false, 1, 10, 0, 1, "", &rec));
        CHECK(big.explosions == 255 && big.strikes == 255);
    }
    CHECK(!morning_on_result(NULL, true, 1, 1, 1, 1, "", &rec));
}

static void test_streak(void)
{
    HistoryRing h;

    h.clear();
    CHECK(history_streak(h, 0) == 0);
    h.push(rec_at(2026, 10, 12, 8, 0));
    h.push(rec_at(2026, 10, 13, 8, 0));
    h.push(rec_at(2026, 10, 14, 23, 59));
    CHECK(history_streak(h, 0) == 3);                                           /* time unknown: count back */
    CHECK(history_streak(h, sched_local_to_unix(2026, 10, 14, 23, 59)) == 3);
    CHECK(history_streak(h, sched_local_to_unix(2026, 10, 15, 6, 0)) == 3);     /* yesterday still counts */
    CHECK(history_streak(h, sched_local_to_unix(2026, 10, 16, 6, 0)) == 0);     /* a day was missed */

    h.push(rec_at(2026, 10, 14, 7, 0));                                         /* second record, same day */
    CHECK(history_streak(h, 0) == 3);

    h.push(rec_at(2026, 10, 16, 8, 0));                                         /* gap on the 15th */
    CHECK(history_streak(h, sched_local_to_unix(2026, 10, 16, 9, 0)) == 1);

    h.push(rec_at(2026, 10, 15, 8, 0));                                         /* gap filled */
    CHECK(history_streak(h, sched_local_to_unix(2026, 10, 16, 9, 0)) == 5);

    {   /* local-day boundary: 23:30 on the 13th and 07:30 on the 14th (UTC+8) are two consecutive days */
        HistoryRing b;
        HistRecord r;
        memset(&r, 0, sizeof(r));
        b.clear();
        r.end_unix = sched_local_to_unix(2026, 10, 14, 7, 30);
        b.push(r);
        r.end_unix = sched_local_to_unix(2026, 10, 13, 23, 30);
        b.push(r);
        CHECK(history_streak(b, 0) == 2);
    }
    {   /* records without a valid clock are ignored */
        HistoryRing z;
        HistRecord r;
        memset(&r, 0, sizeof(r));
        z.clear();
        z.push(r);
        CHECK(history_streak(z, 0) == 0);
    }
}

static void test_json(void)
{
    HistRecord r;
    char buf[200];
    char tiny[20];

    memset(&r, 0, sizeof(r));
    r.end_unix = 1760000000u;
    r.duration_s = 95;
    r.strikes = 2;
    r.snoozes = 1;
    r.explosions = 3;
    strcpy(r.module, "CG2271");
    CHECK(history_to_json(r, buf, sizeof(buf)) == strlen(buf));
    CHECK(strcmp(buf, "{\"ts\":1760000000,\"duration_s\":95,\"strikes\":2,\"snoozes\":1,\"explosions\":3,"
                      "\"module\":\"CG2271\",\"device\":\"defuse-1\"}") == 0);
    CHECK(history_to_json(r, tiny, sizeof(tiny)) == 0);
    CHECK(history_to_json(r, NULL, 10) == 0);
    CHECK(history_to_json(r, buf, 0) == 0);
}

int main(void)
{
    test_ring_basics();
    test_ring_wraps_many_times();
    test_tracker();
    test_streak();
    test_json();
    printf("test_history: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
